import csv
from dataclasses import FrozenInstanceError
import tempfile
from pathlib import Path
import unittest

import orbitforge as of
from orbitforge import units as u
from test_analysis import make_project


class ReportTemplateTests(unittest.TestCase):
    def test_reusable_template_preserves_source_and_provenance(self):
        project, station = make_project(0, 100)
        analysis = project.prepare()
        selected = [of.reports.Column("state.x", unit="km", title="距离"), "time.epoch"]
        template = of.reports.Template(selected)
        selected.clear()
        result = analysis.report(of.ReportKind.CARTESIAN_STATE, objects=station,
                                 step=u.ns(30), frame=of.FrameId.ITRF, template=template)
        self.assertEqual(result.table.row_count, 5)
        self.assertEqual([c.id for c in result.table.columns], ["state.x", "time.epoch"])
        self.assertEqual(result.table.rows[0][0], 6378137)
        self.assertEqual(result.numeric_value(0, 0), 6378.137)
        self.assertEqual(result.column_title(0), "距离 (km)")
        project.objects.remove(station)
        restored = of.reports.Template(units=of.ReportUnits.SI).apply(result)
        self.assertEqual(len(restored.table.columns), 10)
        self.assertEqual(restored.table.rows[-1][2].tai_nanoseconds, 100)
        self.assertEqual(restored.input_revision, result.input_revision)
        self.assertEqual(restored.input_sha256, result.input_sha256)
        self.assertEqual(restored.data_sha256, result.data_sha256)
        self.assertEqual(restored.column_unit(4), "m")
        self.assertEqual(result.column_unit(0), "km")
        with tempfile.TemporaryDirectory() as directory:
            path = Path(directory) / "report.csv"
            result.export_csv(path)
            with path.open(encoding="utf-8-sig", newline="") as stream:
                rows = list(csv.reader(stream))
            self.assertEqual(rows[0], ["距离 (km)", "Time (TAI)"])
            self.assertAlmostEqual(float(rows[1][0]), 6378.137)

    def test_explicit_all_keeps_prepared_scope_while_empty_is_rejected(self):
        project, station = make_project()
        analysis = project.prepare()
        with self.assertRaises(ValueError):
            analysis.report(of.ReportKind.OBJECT_SUMMARY, objects=[])
        with self.assertRaises(TypeError):
            analysis.report(of.ReportKind.OBJECT_SUMMARY)
        project.objects.ground_station("Second", location=of.Wgs84Location(u.deg(1), u.deg(0)))
        self.assertEqual(analysis.report(of.ReportKind.OBJECT_SUMMARY,
                                        objects=of.reports.ALL).table.row_count, 1)
        report = project.prepare().report(of.ReportKind.OBJECT_SUMMARY, objects=of.reports.ALL)
        self.assertEqual(report.table.row_count, 2)
        self.assertEqual(report.definition.query.scope, of.ReportScope.ALL_COMPATIBLE)

    def test_field_discovery_validation_and_template_conflicts(self):
        fields = of.reports.fields(of.ReportKind.CARTESIAN_STATE)
        self.assertEqual(next(c for c in fields if c.id == "state.x").unit, "m")
        project, station = make_project()
        analysis = project.prepare()
        for columns in ([], ["state.x", "state.x"]):
            with self.assertRaises(ValueError):
                of.reports.Template(columns)
        for columns in ("state.x", [1]):
            with self.assertRaises(TypeError):
                of.reports.Template(columns)
        for columns in (["missing"], [of.reports.Column("state.x", unit="deg")]):
            calls = []
            with self.assertRaises(ValueError):
                analysis.report(of.ReportKind.CARTESIAN_STATE, objects=station,
                                frame=of.FrameId.ITRF, step=u.ns(30),
                                template=of.reports.Template(columns), progress=lambda *a: calls.append(a))
            self.assertEqual(calls, [])
        template = of.reports.Template()
        with self.assertRaises(FrozenInstanceError):
            template.columns = ()
        with self.assertRaises(TypeError):
            analysis.report(of.ReportKind.OBJECT_SUMMARY, objects=station,
                            template=template, units=of.ReportUnits.SI)

    def test_utc_is_required_only_when_displayed(self):
        project, station = make_project()
        template = of.reports.Template(["state.x"], time_scale=of.ReportTimeScale.UTC)
        result = project.prepare().report(of.ReportKind.CARTESIAN_STATE, objects=station,
                                          step=u.ns(30), frame=of.FrameId.ITRF, template=template)
        tai = of.reports.Template([of.reports.Column("time.epoch", unit="TAI")],
                                  time_scale=of.ReportTimeScale.UTC).apply(result)
        self.assertEqual(tai.cell_text(0, 0), "2000-01-01T12:00:00.000000000")
        with self.assertRaises(ValueError):
            of.reports.Template([of.reports.Column("time.epoch", unit="UTC")]).apply(result)
        with self.assertRaises(of.AnalysisCapabilityError):
            project.prepare().report(of.ReportKind.CARTESIAN_STATE, objects=station,
                                     step=u.ns(30), frame=of.FrameId.ITRF,
                                     template=of.reports.Template([of.reports.Column("time.epoch", unit="UTC")]))

    def test_custom_template_roundtrip_and_missing_binding_remains_explicit(self):
        project, station = make_project()
        result = project.prepare().report(of.ReportKind.CARTESIAN_STATE, objects=station,
                                          step=u.ns(30), frame=of.FrameId.ITRF,
                                          template=of.reports.Template(["time.epoch", of.reports.Column("state.x", unit="m", title="X")]))
        project.set_reports([result.definition])
        project.objects.remove(station)
        with tempfile.TemporaryDirectory() as directory:
            path = Path(directory) / "report.orbitforge"
            project.save(path)
            restored = of.load_project(path)
            definition = restored.to_data().reports[0]
            self.assertEqual(definition.query.object_ids, [station.id])
            self.assertEqual(definition.query.scope, of.ReportScope.OBJECTS)
            self.assertEqual([(c.field, c.unit, c.title) for c in definition.format.columns],
                             [("time.epoch", "", ""), ("state.x", "m", "X")])
