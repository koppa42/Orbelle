import csv
import gc
import importlib.util
import inspect
from itertools import repeat
from pathlib import Path
import shutil
import tempfile
import unittest
from unittest.mock import patch

import orbelle as ob
from orbelle import units as u

ROOT = Path(__file__).resolve().parents[3]
EOP = ROOT / "data/samples/eop/iers_c04_2000_01_01_2000_01_02/manifest.json"
TIME = ROOT / "data/samples/time/iers_leap_seconds_2026_07.dat"
TLE = (
    "1 00005U 58002B   00179.78495062  .00000023  00000-0  28098-4 0  4753",
    "2 00005  34.2682 348.7242 1859667 331.7664  19.3264 10.82419157413667",
)


def make_project(first=0, last=100):
    project = ob.Project("Analysis", ob.Epoch(first), ob.Epoch(last))
    station = project.objects.ground_station("Equator", location=ob.Wgs84Location(u.deg(0), u.deg(0)))
    return project, station


class AnalysisTests(unittest.TestCase):
    def test_exact_duration_units_and_rejection(self):
        self.assertEqual(u.s(0.1), u.ns(100_000_000))
        self.assertEqual(u.ms(0.000001), u.ns(1))
        self.assertEqual(u.s(9_007_199), u.ns(9_007_199_000_000_000))
        for value in (True, "1", None):
            with self.assertRaises(TypeError):
                u.s(value)
        for value in (float("nan"), float("inf"), 0.0000000001, 10**12):
            with self.assertRaises((ValueError, OverflowError)):
                u.s(value)
        with self.assertRaises(TypeError):
            u.Duration(1.0)

    def test_sampling_is_exact_and_includes_non_divisible_endpoint(self):
        base = 8_000_000_000_000_000_003
        project, station = make_project(base, base + 100)
        analysis = project.prepare()
        result = analysis.states(station, step=u.ns(30), frame=ob.FrameId.ITRF)
        self.assertEqual(result.epoch.tai_nanoseconds, tuple(base + v for v in (0, 30, 60, 90, 100)))
        self.assertEqual(len(result), 5)
        self.assertEqual(result.position.values[0], (6378137, 0, 0))
        self.assertEqual(result.provenance.input_revision, project.revision)
        single = analysis.states(station, step=u.s(1), frame=ob.FrameId.ITRF,
                                 first=ob.Epoch(base + 3), last=ob.Epoch(base + 3))
        self.assertEqual(single.epoch.tai_nanoseconds, (base + 3,))

    def test_explicit_times_preserve_order_duplicates_and_empty_schema(self):
        project, station = make_project()
        analysis = project.prepare()
        result = analysis.states(station, times=(ob.Epoch(t) for t in (90, 1, 90)), frame=ob.FrameId.ITRF)
        self.assertEqual(result.epoch.tai_nanoseconds, (90, 1, 90))
        empty = analysis.states(station, times=[], frame=ob.FrameId.ITRF)
        self.assertEqual(len(empty), 0)
        self.assertEqual(empty.position.values, ())
        self.assertEqual(empty.frame, ob.FrameId.ITRF)
        self.assertEqual(empty.origin, ob.OriginId.EARTH)
        self.assertEqual(empty.provenance.object_id, station.id)
        self.assertEqual(len(tuple(result)), 3)

    def test_query_validation_and_fixed_signatures(self):
        project, station = make_project()
        analysis = project.prepare()
        for options in ({}, {"step": u.ns(1), "times": []}, {"step": 30},
                        {"times": [0]}, {"times": [], "first": ob.Epoch(0)}):
            with self.assertRaises(TypeError):
                analysis.states(station, frame=ob.FrameId.ITRF, **options)
        for options in ({"step": u.ns(0)}, {"step": u.ns(-1)}, {"times": repeat(ob.Epoch(0))}):
            with self.assertRaises(ValueError):
                analysis.states(station, frame=ob.FrameId.ITRF, **options)
        with self.assertRaises(IndexError):
            analysis.state(station, at=ob.Epoch(101), frame=ob.FrameId.ITRF)
        with self.assertRaises(ob.AnalysisCapabilityError):
            analysis.states(station, times=[], frame=ob.FrameId.GCRF)
        with self.assertRaises(TypeError):
            analysis.state(station, ob.Epoch(0))
        self.assertIn("step", inspect.signature(analysis.states).parameters)
        with self.assertRaises(AttributeError):
            analysis.revision = 100

    def test_snapshot_survives_edits_deletion_and_project_lifetime(self):
        project, station = make_project()
        analysis = project.prepare()
        before = analysis.state(station, at=ob.Epoch(0), frame=ob.FrameId.ITRF)
        station.location = ob.Wgs84Location(u.deg(90), u.deg(0))
        self.assertNotEqual(project.prepare().state(station, at=ob.Epoch(0), frame=ob.FrameId.ITRF).position_m,
                            before.position_m)
        project.objects.remove(station)
        with self.assertRaises(ob.ObjectNotFoundError):
            _ = station.name
        self.assertEqual(analysis.state(station, at=ob.Epoch(0), frame=ob.FrameId.ITRF).position_m, before.position_m)
        result = analysis.states(station, step=u.ns(100), frame=ob.FrameId.ITRF)
        del project, station, analysis
        gc.collect()
        self.assertEqual(result.position.values[0], (6378137, 0, 0))
        self.assertEqual(result.object_name, "Equator")

    def test_document_scope_and_objects_created_after_prepare(self):
        project, station = make_project()
        other = ob.Project.from_data(project.to_data())
        analysis = project.prepare()
        with self.assertRaises(ob.ProjectError):
            analysis.state(other.objects[station.id], at=ob.Epoch(0), frame=ob.FrameId.ITRF)
        new = project.objects.duplicate(station, name="Later")
        with self.assertRaises(ob.ObjectNotFoundError):
            analysis.state(new, at=ob.Epoch(0), frame=ob.FrameId.ITRF)

    def test_plan_rejects_stale_revision_and_uncommitted_edits(self):
        project, station = make_project()
        plan = project.plan_prepare()
        with project.edit():
            station.name = "Draft"
            for operation in (project.prepare, project.plan_prepare, plan.prepare):
                with self.assertRaises(RuntimeError):
                    operation()
        with self.assertRaises(ob.RevisionConflictError):
            plan.prepare(accept_data_changes=True)
        self.assertEqual(project.prepare().revision, project.revision)

    def test_data_consent_is_pinned_and_not_sticky(self):
        strict = ob.DataSnapshot.load(eop_manifest=EOP)
        project, station = make_project()
        project.earth_orientation = strict.eop_info
        selected = ob.DataSnapshot.load(eop_manifest=EOP, extrapolation_days=2)
        with self.assertRaises(ob.DataChangedError) as caught:
            project.prepare(data=selected)
        plan = caught.exception.plan
        self.assertEqual(tuple(v.role for v in plan.data_changes), ("earth-orientation",))
        # Replacing the caller's selected resource does not change the existing plan.
        selected = ob.DataSnapshot.load(eop_manifest=EOP, extrapolation_days=3)
        accepted = plan.prepare(accept_data_changes=True)
        self.assertEqual(accepted.data.eop_info.extrapolation_days, 2)
        self.assertNotEqual(accepted.data.data_lock.sha256, selected.data_lock.sha256)
        self.assertEqual(accepted.state(station, at=ob.Epoch(0)).frame, ob.FrameId.GCRF)
        self.assertEqual(project.earth_orientation.extrapolation_days, 0)
        with self.assertRaises(ob.DataChangedError):
            plan.prepare()
        with self.assertRaises(TypeError):
            plan.prepare(accept_data_changes="yes")

    def test_coverage_extrapolation_and_utc_validity(self):
        strict = ob.DataSnapshot.load(eop_manifest=EOP)
        end = strict.eop_info.last_epoch.tai_nanoseconds
        project, station = make_project(end + 1, end + 1_000_000_000)
        with self.assertRaises(ob.EopCoverageError):
            project.plan_prepare(data=strict)
        limited = ob.DataSnapshot.load(eop_manifest=EOP, extrapolation_days=1)
        self.assertEqual(project.prepare(data=limited).state(station, at=project.first).frame, ob.FrameId.GCRF)
        future, _ = make_project(8_000_000_000_000_000_000, 8_000_000_000_000_000_100)
        with self.assertRaises(ob.TimeDataRangeError):
            future.prepare(data=ob.DataSnapshot.builtin_time())

    def test_selected_files_are_fully_owned_and_never_reopened(self):
        project, station = make_project()
        with tempfile.TemporaryDirectory() as temporary:
            source = Path(temporary) / "eop"
            shutil.copytree(EOP.parent, source)
            time_file = Path(temporary) / TIME.name
            shutil.copyfile(TIME, time_file)
            data = ob.DataSnapshot.load(eop_manifest=source / "manifest.json", time_file=time_file)
            plan = project.plan_prepare(data=data)
        del data
        analysis = plan.prepare()
        self.assertEqual(analysis.state(station, at=ob.Epoch(50)).frame, ob.FrameId.GCRF)

    def test_satellite_models_and_frame_conversion_reuse_native_algorithms(self):
        project, station = make_project(0, 60_000_000_000)
        for orbit in (ob.orbits.TwoBody(initial=ob.orbits.Cartesian(ob.Epoch(0), (7e6, 0, 0), (0, 7500, 1000))),
                      ob.orbits.J2(initial=ob.J2MeanElements(ob.Epoch(0), 7e6, .01, .5, .1, .2, .3)),
                      ob.orbits.J4(initial=ob.J4MeanElements(ob.Epoch(0), 7e6, .01, .5, .1, .2, .3))):
            satellite = project.objects.satellite("Observer", orbit=orbit)
            at = ob.Epoch(42_000_000_000)
            expected = next(s.states[0] for s in project.to_scenario().sample([at])
                            if s.provenance.object_id == satellite.id)
            analysis = project.prepare(data=ob.DataSnapshot.load(eop_manifest=EOP))
            actual = analysis.state(satellite, at=at)
            self.assertEqual(actual.position_m, expected.position_m)
            frame = ob.make_frame_provider()
            eop = ob.load_eop_snapshot(EOP)
            transformed = ob.transform_state(expected, ob.FrameId.ITRF, frame, eop.at(at))
            actual = analysis.state(satellite, at=at, frame=ob.FrameId.ITRF)
            self.assertEqual(actual.velocity_mps, transformed.velocity_mps)
            result = analysis.states(satellite, step=u.s(30))
            satellite.name = "Renamed"
            station.minimum_elevation = u.deg(10)
            self.assertEqual(project.prepare().states(satellite, step=u.s(30)).input_sha256, result.input_sha256)
            self.assertEqual(result.data_sha256, ob.DataLock().sha256)

    def test_sgp4_requires_explicit_time_and_preserves_teme(self):
        tle = ob.Tle(*TLE)
        time = ob.make_builtin_time_provider()
        epoch = time.from_utc(tle.epoch_utc)
        project = ob.Project("SGP4", epoch, ob.Epoch(epoch.tai_nanoseconds + 60_000_000_000))
        satellite = project.objects.satellite("Vanguard", orbit=ob.orbits.Sgp4(tle=tle))
        with self.assertRaises(ob.AnalysisCapabilityError):
            project.prepare()
        analysis = project.prepare(data=ob.DataSnapshot.builtin_time())
        expected = ob.make_sgp4_session(tle, time, "reference").sample([epoch]).states[0]
        self.assertEqual(analysis.state(satellite, at=epoch, frame=ob.FrameId.TEME).position_m, expected.position_m)
        with self.assertRaises(ob.AnalysisCapabilityError):
            analysis.state(satellite, at=epoch, frame=ob.FrameId.GCRF)

    def test_csv_preserves_integer_epochs_and_requires_explicit_overwrite(self):
        base = 8_000_000_000_000_000_003
        project, station = make_project(base, base + 10)
        result = project.prepare().states(station, step=u.ns(10), frame=ob.FrameId.ITRF)
        with tempfile.TemporaryDirectory() as temporary:
            path = Path(temporary) / "states.csv"
            result.to_csv(path)
            original = path.read_bytes()
            with path.open(encoding="utf-8-sig", newline="") as stream:
                rows = list(csv.DictReader(stream))
            self.assertEqual([int(r["epoch_tai_ns"]) for r in rows], [base, base + 10])
            self.assertEqual(float(rows[0]["position.x (m)"]), 6378137)
            self.assertEqual(rows[0]["frame"], "ITRF")
            with self.assertRaises(ValueError):
                result.to_csv(path)
            self.assertEqual(path.read_bytes(), original)
            result.to_csv(path, overwrite=True)
            self.assertEqual(path.read_bytes(), original)

    def test_numpy_and_pandas_are_optional(self):
        project, station = make_project()
        result = project.prepare().states(station, times=[], frame=ob.FrameId.ITRF)
        with patch("orbelle._analysis.import_module", side_effect=ImportError):
            with self.assertRaisesRegex(ImportError, "pip install numpy"):
                result.position.to_numpy()
            with self.assertRaisesRegex(ImportError, "pip install pandas"):
                result.to_pandas()
            with tempfile.TemporaryDirectory() as temporary:
                result.to_csv(Path(temporary) / "empty.csv")
        with self.assertRaises(ValueError):
            result.position.to_numpy(unit="m/s")

    @unittest.skipUnless(importlib.util.find_spec("numpy"), "Optional numpy is not installed")
    def test_numpy_empty_single_shapes_and_copy_isolation(self):
        project, station = make_project()
        analysis = project.prepare()
        for ticks in ([], [0], [1, 2, 1]):
            result = analysis.states(station, times=map(ob.Epoch, ticks), frame=ob.FrameId.ITRF)
            array = result.position.to_numpy(unit="km")
            self.assertEqual(array.shape, (len(ticks), 3))
            self.assertEqual(result.velocity.to_numpy(unit="km/s").shape, (len(ticks), 3))
            self.assertEqual(result.epoch.to_numpy().dtype.name, "int64")
            if ticks:
                self.assertAlmostEqual(array[0, 0], 6378.137, delta=1e-12)
                array[0, 0] = -1
                self.assertEqual(result.position.values[0][0], 6378137)

    @unittest.skipUnless(importlib.util.find_spec("pandas"), "Optional pandas is not installed")
    def test_pandas_keeps_exact_tai_integers_units_and_copies(self):
        base = 8_000_000_000_000_000_003
        project, station = make_project(base, base + 10)
        analysis = project.prepare()
        for ticks in ([], [base, base + 1]):
            result = analysis.states(station, times=map(ob.Epoch, ticks), frame=ob.FrameId.ITRF)
            table = result.to_pandas()
            self.assertEqual(table["epoch_tai_ns"].tolist(), ticks)
            self.assertEqual(str(table["epoch_tai_ns"].dtype), "int64")
            self.assertEqual(table.attrs["frame"], "ITRF")
            self.assertEqual(table.attrs["units"]["position"], "m")
            if ticks:
                table.loc[0, "position.x_m"] = 0
                self.assertEqual(result.position.values[0][0], 6378137)


if __name__ == "__main__":
    unittest.main()
