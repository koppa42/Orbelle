import csv
import gc
import importlib.util
import math
from pathlib import Path
import shutil
import tempfile
import unittest
from unittest.mock import patch

import orbitforge as of
from orbitforge import units as u

EOP = Path(__file__).resolve().parents[3] / "data/samples/eop/iers_c04_2000_01_01_2000_01_02/manifest.json"


def scene(data=None):
    data = data if data is not None else of.DataSnapshot.load(eop_manifest=EOP)
    project = of.Project("Access", of.Epoch(0), of.Epoch(7_200_000_000_000))
    initial = of.CartesianState(project.first, of.OriginId.EARTH, of.FrameId.GCRF,
                               [7e6, 0, 0], [0, math.sqrt(3.986004418e14 / 7e6), 0])
    satellite = project.objects.satellite("Observer", orbit=of.orbits.TwoBody(initial=initial))
    r = project.prepare(data=data).state(satellite, at=project.first, frame=of.FrameId.ITRF).position_m
    station = project.objects.ground_station("Station", location=of.Wgs84Location(
        u.rad(math.atan2(r[1], r[0])), u.rad(math.atan2(r[2], math.hypot(r[0], r[1])))))
    sensor = project.objects.sensor("Camera", parent=satellite,
                                   field_of_view=of.sensors.Cone(half_angle=u.deg(10)))
    return project, station, satellite, sensor, data


class AnalysisAccessTests(unittest.TestCase):
    def test_query_is_iterable_exact_owned_and_matches_tracking_constraint(self):
        project, station, satellite, sensor, data = scene()
        analysis = project.prepare(data=data)
        wide = analysis.access(station, satellite, search_step=u.s(10))
        narrow = analysis.access(station, sensor, search_step=u.s(10))
        self.assertIsInstance(narrow, of.AccessWindows)
        self.assertIsInstance(narrow[0], of.AccessWindow)
        self.assertLess(narrow[0].duration.nanoseconds, wide[0].duration.nanoseconds)
        self.assertEqual(len(tuple(narrow)), len(narrow))
        self.assertEqual(narrow[-1].end.tai_nanoseconds, tuple(narrow)[-1].end.tai_nanoseconds)
        self.assertEqual(len(narrow[:1]), 1)
        with self.assertRaises(IndexError):
            _ = narrow[len(narrow)]
        with self.assertRaises(IndexError):
            _ = narrow[-len(narrow) - 1]
        with self.assertRaises(TypeError):
            _ = narrow[1.0]
        for window in narrow:
            self.assertEqual(window.duration.nanoseconds, window.end.tai_nanoseconds - window.start.tai_nanoseconds)
        self.assertTrue(narrow[0].start_clipped)
        self.assertFalse(narrow[0].end_clipped)
        self.assertEqual(narrow.provenance.station_id, station.id)
        self.assertEqual(narrow.provenance.target_name, "Camera")
        self.assertEqual(narrow.provenance.input_revision, analysis.revision)
        self.assertEqual(narrow.data_sha256, data.data_lock.sha256)
        self.assertEqual(narrow.search_step, u.s(10))
        self.assertEqual(narrow.boundary_tolerance, u.ms(1))
        self.assertEqual(narrow.first.tai_nanoseconds, 0)
        self.assertEqual(narrow.last.tai_nanoseconds, project.last.tai_nanoseconds)
        with self.assertRaises(AttributeError):
            narrow[0].duration = u.s(1)
        sensor.pointing = of.sensors.Pointing.track(station)
        tracked = project.prepare(data=data).access(station, sensor, search_step=u.s(10))
        self.assertEqual(tracked[0].duration, wide[0].duration)
        old_end = narrow[0].end.tai_nanoseconds
        project.objects.remove(satellite)
        self.assertEqual(analysis.access(station, sensor, search_step=u.s(10))[0].end.tai_nanoseconds, old_end)
        del project, station, satellite, sensor, analysis, data
        gc.collect()
        self.assertEqual(narrow[0].end.tai_nanoseconds, old_end)

    def test_pass_extrema_statistics_modes_and_report_export_agree(self):
        project, station, satellite, _, data = scene()
        analysis = project.prepare(data=data)
        windows = analysis.access(station, satellite, search_step=u.s(60))
        self.assertEqual(windows.search_mode, of.AccessSearchMode.EXTREMUM_ASSISTED)
        stats = windows.statistics
        self.assertEqual(stats.count, len(windows))
        self.assertAlmostEqual(stats.total_duration_s, sum(w.duration.nanoseconds/1e9 for w in windows))
        self.assertAlmostEqual(stats.mean_duration_s, stats.total_duration_s/len(windows))
        fixed = analysis.access(station, satellite, search_step=u.s(1), search_mode=of.AccessSearchMode.FIXED_GRID)
        self.assertEqual(len(windows), len(fixed))
        for w, f in zip(windows, fixed):
            self.assertLessEqual(abs(w.start.tai_nanoseconds-f.start.tai_nanoseconds), 1_000_000)
            self.assertLessEqual(abs(w.end.tai_nanoseconds-f.end.tai_nanoseconds), 1_000_000)
            self.assertIsNotNone(w.extrema)
            for extremum in (w.extrema.maximum_elevation, w.extrema.minimum_range):
                self.assertLessEqual(w.start.tai_nanoseconds, extremum.epoch.tai_nanoseconds)
                self.assertLessEqual(extremum.epoch.tai_nanoseconds, w.end.tai_nanoseconds)
            aer = analysis.aer(station,satellite,times=[w.extrema.maximum_elevation.epoch,w.extrema.minimum_range.epoch])
            self.assertAlmostEqual(w.extrema.maximum_elevation.value,aer[0].elevation_rad)
            self.assertAlmostEqual(w.extrema.minimum_range.value,aer[1].range_m)
        report = analysis.report(of.ReportKind.ACCESS_INTERVALS,station=station,target=satellite,step=u.s(60))
        self.assertEqual(report.access_statistics.count,stats.count)
        self.assertEqual(report.access_statistics.total_duration_s,stats.total_duration_s)
        with tempfile.TemporaryDirectory() as directory:
            path=Path(directory)/"passes.csv"
            windows.to_csv(path)
            with path.open(encoding="utf-8-sig",newline="") as stream:
                rows=list(csv.DictReader(stream))
            self.assertEqual(int(rows[0]["max_elevation_tai_ns"]),windows[0].extrema.maximum_elevation.epoch.tai_nanoseconds)
            self.assertEqual(float(rows[0]["min_range_m"]),windows[0].extrema.minimum_range.value)

    def test_subinterval_and_empty_result_keep_schema(self):
        project, station, _, sensor, data = scene()
        analysis = project.prepare(data=data)
        result = analysis.access(station, sensor, search_step=u.s(10),
                                 first=of.Epoch(3), last=of.Epoch(10_000_000_007))
        self.assertEqual(len(result), 1)
        self.assertEqual(result[0].duration, u.ns(10_000_000_004))
        self.assertTrue(result[0].start_clipped and result[0].end_clipped)
        empty = analysis.access(station, sensor, search_step=u.s(10), first=of.Epoch(0), last=of.Epoch(0))
        self.assertEqual(len(empty), 0)
        self.assertEqual(empty.input_sha256, result.input_sha256)
        self.assertEqual(empty.data_sha256, result.data_sha256)
        with tempfile.TemporaryDirectory() as directory:
            path = Path(directory) / "empty.csv"
            empty.to_csv(path)
            with path.open(encoding="utf-8-sig", newline="") as stream:
                reader = csv.DictReader(stream)
                self.assertIn("duration_ns", reader.fieldnames)
                self.assertEqual(list(reader), [])

    def test_rejects_wrong_roles_scope_parameters_and_missing_data(self):
        project, station, satellite, sensor, data = scene()
        analysis = project.prepare(data=data)
        for a, b in ((satellite, station), (station, station), (station.id, sensor)):
            with self.assertRaises(TypeError):
                analysis.access(a, b, search_step=u.s(10))
        for options in ({}, {"search_step": 1}, {"search_step": u.s(1), "boundary_tolerance": 1},
                        {"search_step": u.s(1), "first": 0}, {"search_step": u.s(1), "cancelled": True}):
            with self.assertRaises(TypeError):
                analysis.access(station, sensor, **options)
        for options in ({"search_step": u.s(0)}, {"search_step": u.s(-1)},
                        {"search_step": u.s(1), "boundary_tolerance": u.s(2)},
                        {"search_step": u.s(1), "boundary_tolerance": u.ns(0)},
                        {"search_step": u.ns(1), "boundary_tolerance": u.ns(1)},
                        {"search_step": u.s(1), "first": of.Epoch(1), "last": of.Epoch(0)}):
            with self.assertRaises(ValueError):
                analysis.access(station, sensor, **options)
        with self.assertRaises(IndexError):
            analysis.access(station, sensor, search_step=u.s(10), first=of.Epoch(-1))
        with self.assertRaises(of.AnalysisCapabilityError):
            project.prepare().access(station, sensor, search_step=u.s(10))
        other = of.Project.from_data(project.to_data())
        with self.assertRaises(of.ProjectError):
            analysis.access(other.objects[station.id], sensor, search_step=u.s(10))
        added = project.objects.duplicate(station, name="Later")
        with self.assertRaises(of.ObjectNotFoundError):
            analysis.access(added, sensor, search_step=u.s(10))

    def test_cancellation_callback_errors_and_repeat_query(self):
        project, station, _, sensor, data = scene()
        analysis = project.prepare(data=data)
        for count in (1, 4):
            calls = []
            def cancelled():
                calls.append(True)
                return len(calls) == count
            with self.assertRaises(of.AnalysisCancelled):
                analysis.access(station, sensor, search_step=u.s(100), cancelled=cancelled)
            self.assertEqual(len(calls), count)
        def broken():
            raise LookupError("caller failure")
        with self.assertRaisesRegex(LookupError, "caller failure"):
            analysis.access(station, sensor, search_step=u.s(10), cancelled=broken)
        with self.assertRaisesRegex(TypeError, "return bool"):
            analysis.access(station, sensor, search_step=u.s(10), cancelled=lambda: 1)
        self.assertGreater(len(analysis.access(station, sensor, search_step=u.s(10))), 0)

    def test_input_hash_and_pinned_files(self):
        with tempfile.TemporaryDirectory() as directory:
            source = Path(directory) / "eop"
            shutil.copytree(EOP.parent, source)
            data = of.DataSnapshot.load(eop_manifest=source / "manifest.json")
            project, station, satellite, sensor, _ = scene(data)
            analysis = project.prepare(data=data)
        original = analysis.access(station, sensor, search_step=u.s(10))
        sensor.name = "Renamed"
        satellite.display = of.OrbitStyle(line_color=0x123456)
        project.objects.ground_station("Unrelated", location=of.Wgs84Location(u.deg(0), u.deg(0)))
        renamed = project.prepare(data=data).access(station, sensor, search_step=u.s(10))
        self.assertEqual(original.input_sha256, renamed.input_sha256)
        sensor.field_of_view = of.sensors.Cone(half_angle=u.deg(20))
        changed = project.prepare(data=data).access(station, sensor, search_step=u.s(10))
        self.assertNotEqual(original.input_sha256, changed.input_sha256)

    def test_csv_is_independent_exact_and_protects_existing_file(self):
        project, station, _, sensor, data = scene()
        result = project.prepare(data=data).access(station, sensor, search_step=u.s(10),
                                                  first=of.Epoch(3), last=of.Epoch(10_000_000_007))
        del project, station, sensor, data
        gc.collect()
        with tempfile.TemporaryDirectory() as directory:
            path = Path(directory) / "access.csv"
            result.to_csv(path)
            before = path.read_bytes()
            with path.open(encoding="utf-8-sig", newline="") as stream:
                rows = list(csv.DictReader(stream))
            self.assertEqual(int(rows[0]["start_tai_ns"]), 3)
            self.assertEqual(int(rows[0]["end_tai_ns"]), 10_000_000_007)
            self.assertEqual(int(rows[0]["duration_ns"]), 10_000_000_004)
            self.assertEqual(rows[0]["start_clipped"], "1")
            with self.assertRaises(ValueError):
                result.to_csv(path)
            self.assertEqual(path.read_bytes(), before)
            with self.assertRaises(TypeError):
                result.to_csv(path, overwrite="yes")
            result.to_csv(path, overwrite=True)
            self.assertEqual(path.read_bytes(), before)

    def test_pandas_is_optional(self):
        project, station, _, sensor, data = scene()
        result = project.prepare(data=data).access(station, sensor, search_step=u.s(10))
        with patch("orbitforge._analysis.import_module", side_effect=ImportError):
            with self.assertRaisesRegex(ImportError, "pip install pandas"):
                result.to_pandas()

    @unittest.skipUnless(importlib.util.find_spec("pandas"), "optional pandas not installed")
    def test_pandas_preserves_types_metadata_and_empty_schema(self):
        project, station, _, sensor, data = scene()
        analysis = project.prepare(data=data)
        for end in (0, 10_000_000_003):
            result = analysis.access(station, sensor, search_step=u.s(10), last=of.Epoch(end))
            frame = result.to_pandas()
            self.assertEqual(str(frame["duration_ns"].dtype), "int64")
            self.assertEqual(str(frame["start_clipped"].dtype), "bool")
            self.assertEqual(frame.attrs["last_tai_ns"], end)
            self.assertEqual(frame.attrs["input_sha256"], result.input_sha256)
            if len(result):
                frame.loc[0, "duration_ns"] = -1
                self.assertGreater(result[0].duration.nanoseconds, 0)


if __name__ == "__main__":
    unittest.main()
