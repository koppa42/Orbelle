import csv
import gc
import importlib.util
from itertools import repeat
from pathlib import Path
import shutil
import tempfile
import unittest
from unittest.mock import patch

import orbitforge as of
from orbitforge import units as u
from test_analysis_access import EOP, scene


class AnalysisAerTests(unittest.TestCase):
    def test_samples_preserve_exact_times_order_duplicates_and_endpoint(self):
        project, station, sat, sensor, data = scene()
        analysis = project.prepare(data=data)
        epochs = [of.Epoch(10_000_000_007), of.Epoch(3), of.Epoch(10_000_000_007)]
        result = analysis.aer(station, sat, times=iter(epochs))
        self.assertIsInstance(result, of.AerSeries)
        self.assertIsInstance(result[0], of.AerSample)
        self.assertEqual(result.epoch.tai_nanoseconds, tuple(t.tai_nanoseconds for t in epochs))
        self.assertEqual(result[0].range_m, result[-1].range_m)
        self.assertEqual(len(result[::-1]), 3)
        self.assertEqual(result.off_boresight.values, (None, None, None))
        self.assertEqual(result.range.unit, "m")
        self.assertEqual(result.range_rate.unit, "m/s")
        self.assertEqual(result.azimuth.unit, "rad")
        self.assertEqual(result.visible, tuple(s.visible for s in result))
        self.assertEqual(result.line_of_sight, tuple(s.line_of_sight for s in result))
        sampled = analysis.aer(station, sensor, step=u.s(10), first=of.Epoch(3), last=epochs[0])
        self.assertEqual(sampled.epoch.tai_nanoseconds, (3, 10_000_000_003, 10_000_000_007))
        self.assertTrue(all(s.off_boresight_rad is not None for s in sampled))
        self.assertEqual(sampled.input_sha256,
                         analysis.access(station, sensor, search_step=u.s(10)).input_sha256)
        self.assertEqual(sampled.provenance.input_revision, analysis.revision)
        for index in (len(result), -len(result) - 1):
            with self.assertRaises(IndexError):
                _ = result[index]
        with self.assertRaises(TypeError):
            _ = result[1.5]
        with self.assertRaises(AttributeError):
            result[0].range_m = 0
        self.assertEqual(len(analysis.aer(station, sensor, times=[])), 0)
        self.assertEqual(len(analysis.aer(station, sensor, step=u.s(10), last=of.Epoch(0))), 1)

    def test_visibility_and_snapshot_survive_object_changes_and_data_file_removal(self):
        with tempfile.TemporaryDirectory() as directory:
            snapshot = Path(directory) / "eop"
            shutil.copytree(EOP.parent, snapshot)
            data = of.DataSnapshot.load(eop_manifest=snapshot / "manifest.json")
            project, station, sat, sensor, data = scene(data)
            analysis = project.prepare(data=data)
            before = analysis.aer(station, sensor, step=u.s(10))
            sensor.pointing = of.sensors.Pointing.track(station)
            tracked = project.prepare(data=data).aer(station, sensor, step=u.s(10))
            wide = analysis.aer(station, sat, step=u.s(10))
            self.assertGreater(sum(tracked.visible), sum(before.visible))
            self.assertEqual(tracked.visible, wide.visible)
            project.objects.remove(sat)
            shutil.rmtree(snapshot)
            self.assertEqual(analysis.aer(station, sensor, step=u.s(10)).range.values, before.range.values)
            report = analysis.report(of.ReportKind.AER, station=station, target=sensor, step=u.s(10))
            self.assertEqual(report.table.row_count, len(before))
            del analysis, project, station, sat, sensor, data
            gc.collect()
            self.assertEqual(before.provenance.target_name, "Camera")
            self.assertTrue(before[0].visible)
            self.assertGreater(report.numeric_value(0, 5), 0)

    def test_query_validation_scope_cancellation_and_callback_errors(self):
        project, station, sat, sensor, data = scene()
        analysis = project.prepare(data=data)
        for options in ({}, {"step": 1}, {"step": u.s(10), "times": []}, {"times": [3]},
                        {"times": [], "first": of.Epoch(0)}, {"step": u.s(10), "last": 0},
                        {"times": [], "cancelled": True}, {"times": [], "cancelled": lambda: 1}):
            with self.assertRaises(TypeError):
                analysis.aer(station, sensor, **options)
        for options in ({"step": u.s(0)}, {"step": u.s(-1)}, {"step": u.ns(1)},
                        {"times": repeat(of.Epoch(0))},
                        {"step": u.s(1), "first": of.Epoch(1), "last": of.Epoch(0)}):
            with self.assertRaises(ValueError):
                analysis.aer(station, sensor, **options)
        with self.assertRaises(IndexError):
            analysis.aer(station, sensor, times=[of.Epoch(-1)])
        with self.assertRaises(TypeError):
            analysis.aer(sat, station, step=u.s(10))
        with self.assertRaises(of.AnalysisCapabilityError):
            project.prepare().aer(station, sensor, times=[])
        other = of.Project("Other", project.first, project.last)
        foreign = other.objects.ground_station("Foreign", location=station.location)
        with self.assertRaises(of.ProjectError):
            analysis.aer(foreign, sensor, step=u.s(10))
        calls = 0

        def stop():
            nonlocal calls
            calls += 1
            return calls == 6

        with self.assertRaises(of.AnalysisCancelled):
            analysis.aer(station, sensor, step=u.s(10), cancelled=stop)
        self.assertEqual(calls, 6)
        with self.assertRaisesRegex(RuntimeError, "callback failed"):
            analysis.aer(station, sensor, times=[], cancelled=lambda: (_ for _ in ()).throw(RuntimeError("callback failed")))

    def test_csv_keeps_exact_epochs_blanks_and_atomic_overwrite_policy(self):
        project, station, sat, _, data = scene()
        result = project.prepare(data=data).aer(station, sat, times=[of.Epoch(3)])
        with tempfile.TemporaryDirectory() as directory:
            path = Path(directory) / "aer.csv"
            result.to_csv(path)
            before = path.read_bytes()
            with path.open(encoding="utf-8-sig", newline="") as stream:
                row = next(csv.DictReader(stream))
            self.assertEqual(row["epoch_tai_ns"], "3")
            self.assertEqual(row["off_boresight (rad)"], "")
            self.assertEqual(row["visible"], "1")
            self.assertEqual(float(row["range (m)"]), result[0].range_m)
            with self.assertRaises(ValueError):
                result.to_csv(path)
            self.assertEqual(path.read_bytes(), before)
            result.to_csv(path, overwrite=True)
            self.assertEqual(path.read_bytes(), before)
            empty = project.prepare(data=data).aer(station, sat, times=[])
            empty.to_csv(path, overwrite=True)
            with path.open(encoding="utf-8-sig", newline="") as stream:
                reader = csv.DictReader(stream)
                self.assertIn("azimuth (rad)", reader.fieldnames)
                self.assertEqual(list(reader), [])

    def test_optional_dependencies_are_lazy(self):
        project, station, sat, _, data = scene()
        result = project.prepare(data=data).aer(station, sat, times=[])
        with patch("orbitforge._analysis.import_module", side_effect=ImportError):
            with self.assertRaisesRegex(ImportError, "pip install numpy"):
                result.range.to_numpy()
            with self.assertRaisesRegex(ImportError, "pip install pandas"):
                result.to_pandas()

    @unittest.skipUnless(importlib.util.find_spec("numpy"), "optional numpy not installed")
    def test_scalar_numpy_conversions_shape_units_missing_values_and_copy(self):
        import numpy as np
        column = of.ScalarColumn((None, 0.0, 3.141592653589793), "rad")
        converted = column.to_numpy(unit="deg")
        np.testing.assert_allclose(converted, [np.nan, 0, 180], equal_nan=True)
        converted[1] = 50
        self.assertEqual(column.values[1], 0)
        self.assertEqual(of.ScalarColumn((), "m").to_numpy().shape, (0,))
        self.assertEqual(of.ScalarColumn((1000,), "m").to_numpy(unit="km").tolist(), [1])
        self.assertEqual(of.ScalarColumn((2000,), "m/s").to_numpy(unit="km/s").tolist(), [2])
        with self.assertRaises(ValueError):
            column.to_numpy(unit="km")

    @unittest.skipUnless(importlib.util.find_spec("pandas"), "optional pandas not installed")
    def test_dataframe_dtypes_missing_values_and_independent_storage(self):
        project, station, sat, _, data = scene()
        analysis = project.prepare(data=data)
        for times in ([], [of.Epoch(3)], [of.Epoch(3), of.Epoch(7)]):
            result = analysis.aer(station, sat, times=times)
            table = result.to_pandas()
            self.assertEqual(str(table.epoch_tai_ns.dtype), "int64")
            self.assertEqual(str(table.visible.dtype), "bool")
            self.assertEqual(str(table.azimuth_rad.dtype), "float64")
            self.assertTrue(table.off_boresight_rad.isna().all())
            self.assertEqual(table.attrs["input_revision"], analysis.revision)
            self.assertEqual(table.attrs["data_sha256"], data.data_lock.sha256)
            if times:
                table.loc[0, "range_m"] = 0
                self.assertGreater(result[0].range_m, 0)


class AnalysisReportTests(unittest.TestCase):
    def test_all_templates_and_explicit_display_units(self):
        project, station, sat, sensor, data = scene()
        covariance = of.orbits.CovarianceSettings(
            enabled=True,
            state=tuple(tuple((10000.0 if i < 3 else 1.0) if i == j else 0.0
                              for j in range(6)) for i in range(6)),
        )
        uncertain = project.objects.satellite("Uncertain", orbit=of.orbits.Hpop(
            initial=sat.orbit.initial, gravity_degree=0, covariance=covariance))
        analysis = project.prepare(data=data)
        for kind in of.ReportKind.__members__.values():
            with self.subTest(kind=kind):
                options = {}
                if kind in (of.ReportKind.AER, of.ReportKind.ACCESS_INTERVALS):
                    options.update(station=station, target=sensor, step=u.s(10))
                elif kind == of.ReportKind.STATE_COVARIANCE:
                    options.update(objects=uncertain, step=u.s(60), last=project.first)
                elif kind != of.ReportKind.SCENARIO_SUMMARY:
                    options["objects"] = station if kind == of.ReportKind.GROUND_STATIONS else sat
                    if kind not in (of.ReportKind.OBJECT_SUMMARY, of.ReportKind.GROUND_STATIONS):
                        options["step"] = u.s(60)
                if kind in (of.ReportKind.LIGHTING_INTERVALS, of.ReportKind.LIGHTING_STATE):
                    # This EOP-only snapshot deliberately has no solar ephemeris.
                    # Successful lighting queries are covered by test_lighting.py.
                    with self.assertRaisesRegex(ValueError, "prepared solar ephemeris"):
                        analysis.report(kind, **options)
                    continue
                report = analysis.report(kind, **options)
                self.assertIsInstance(report, of.ReportResult)
                self.assertGreater(report.table.row_count, 0)
                self.assertEqual(report.input_revision, analysis.revision)
                self.assertEqual(report.data_sha256, data.data_lock.sha256)
                if kind == of.ReportKind.STATE_COVARIANCE:
                    self.assertEqual(report.table.row_count, 1)
                    self.assertEqual(report.numeric_value(0, 4), 0.01)
        report = analysis.report(of.ReportKind.CARTESIAN_STATE, objects=[sat], step=u.s(30), last=of.Epoch(3))
        self.assertEqual(report.table.row_count, 2)
        self.assertEqual(report.numeric_value(0, 4), 7000)
        self.assertEqual(report.column_unit(4), "km")
        self.assertEqual(report.table.rows[-1][2].tai_nanoseconds, 3)
        self.assertEqual(report.definition.query.object_ids, [sat.id])
        with tempfile.TemporaryDirectory() as directory:
            report.export_csv(Path(directory) / "report.csv")

    def test_selection_rejects_empty_foreign_and_mismatched_inputs(self):
        project, station, sat, sensor, data = scene()
        analysis = project.prepare(data=data)
        for options in ({}, {"objects": []}, {"objects": [sat.id]}, {"objects": [sat, sat]},
                        {"objects": repeat(sat)}, {"objects": sat, "station": station},
                        {"objects": sat, "step": u.s(1)}):
            with self.assertRaises((TypeError, ValueError)):
                analysis.report(of.ReportKind.OBJECT_SUMMARY, **options)
        with self.assertRaisesRegex(ValueError, "incompatible"):
            analysis.report(of.ReportKind.STATE_COVARIANCE, objects=sat, step=u.s(60))
        with self.assertRaises(TypeError):
            analysis.report(of.ReportKind.CARTESIAN_STATE, objects=sat)
        with self.assertRaises(TypeError):
            analysis.report(of.ReportKind.SCENARIO_SUMMARY, objects=[])
        with self.assertRaises(TypeError):
            analysis.report(of.ReportKind.AER, objects=[station, sensor], step=u.s(10))
        other = of.Project("Other", project.first, project.last)
        foreign = other.objects.ground_station("Station", location=station.location)
        with self.assertRaises(of.ProjectError):
            analysis.report(of.ReportKind.GROUND_STATIONS, objects=foreign)
        late = project.objects.ground_station("Late", location=station.location)
        with self.assertRaises(of.ObjectNotFoundError):
            analysis.report(of.ReportKind.GROUND_STATIONS, objects=late)

    def test_native_reports_do_not_implicitly_select_time_or_eop(self):
        project, station, sat, _, _ = scene()
        analysis = project.prepare()
        report = analysis.report(of.ReportKind.CARTESIAN_STATE, objects=sat, step=u.s(60))
        self.assertEqual(report.definition.format.time_scale, of.ReportTimeScale.TAI)
        with self.assertRaises(of.AnalysisCapabilityError):
            analysis.report(of.ReportKind.CARTESIAN_STATE, objects=sat, step=u.s(60), time_scale=of.ReportTimeScale.UTC)
        with self.assertRaises(of.AnalysisCapabilityError):
            analysis.report(of.ReportKind.CARTESIAN_STATE, objects=sat, step=u.s(60), frame=of.FrameId.ITRF)
        self.assertEqual(analysis.report(of.ReportKind.GROUND_STATIONS, objects=station).table.row_count, 1)

    def test_progress_cancellation_callback_exception_and_deleted_handles(self):
        project, _, sat, _, _ = scene()
        analysis = project.prepare()
        project.objects.remove(sat)
        progress = []
        report = analysis.report(of.ReportKind.CARTESIAN_STATE, objects=sat, step=u.s(60),
                                 progress=lambda done, total: progress.append((done, total)))
        self.assertEqual(progress[-1], (report.table.row_count, report.table.row_count))
        self.assertEqual(progress, sorted(progress))
        self.assertEqual(report.table.rows[0][1], "Observer")
        cancelled = False

        def done(current, total):
            nonlocal cancelled
            cancelled = current == total

        with self.assertRaises(of.AnalysisCancelled):
            analysis.report(of.ReportKind.CARTESIAN_STATE, objects=sat, step=u.s(60),
                            cancelled=lambda: cancelled, progress=done)
        with self.assertRaisesRegex(RuntimeError, "progress failed"):
            analysis.report(of.ReportKind.OBJECT_SUMMARY, objects=sat,
                            progress=lambda *_: (_ for _ in ()).throw(RuntimeError("progress failed")))
        for options in ({"cancelled": True}, {"cancelled": lambda: 1}, {"progress": True}):
            with self.assertRaises(TypeError):
                analysis.report(of.ReportKind.OBJECT_SUMMARY, objects=sat, **options)


if __name__ == "__main__":
    unittest.main()
