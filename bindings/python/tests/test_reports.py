import csv
import io
import math
import struct
import tempfile
import unittest
from pathlib import Path

# Native value/format tests; public editing is in test_objects.py.
from orbitforge import _native as of
from project_format import rewrite_section


class ReportsTest(unittest.TestCase):
    def inputs(self):
        project = of.Project('报告', of.Epoch(0), of.Epoch(2_000_000_001))
        station = of.ProjectGroundStation()
        station.object_id = 'station'
        station.display_name = '海岸,"一号"\n站'
        project.add_ground_station(station)
        definition = of.ReportDefinition()
        definition.id = 'report-a'
        definition.name = '位置报告'
        definition.frame = of.FrameId.ITRF
        definition.step_nanoseconds = 1_000_000_000
        definition.time_scale = of.ReportTimeScale.TAI
        return project, definition

    def test_typed_samples_csv_and_immutable_result(self):
        project, definition = self.inputs()
        result = of.generate_report(project, definition)
        self.assertEqual(of.estimate_report_rows(project, definition), 4)
        self.assertEqual(result.table.row_count, 4)
        self.assertEqual([row[2].tai_nanoseconds for row in result.table.rows],
                         [0, 1_000_000_000, 2_000_000_000, 2_000_000_001])
        self.assertEqual(result.table.rows[0][4], 6378137)
        self.assertEqual(result.table.columns[4].unit, 'm')
        self.assertEqual(result.column_title(4), 'X (km)')
        self.assertAlmostEqual(float(result.cell_text(0, 4)), 6378.137)
        copy = result.definition
        copy.name = 'changed'
        definition.name = 'also changed'
        self.assertEqual(result.definition.name, '位置报告')
        with tempfile.TemporaryDirectory() as directory:
            path = Path(directory) / 'report.csv'
            result.export_csv(path)
            original = path.read_bytes()
            rows = list(csv.reader(io.StringIO(original.decode('utf-8-sig'), newline='')))
            self.assertEqual(rows[1][1], '海岸,"一号"\n站')
            self.assertEqual(rows[-1][2], '2000-01-01T12:00:02.000000001')
            with self.assertRaises(ValueError):
                result.export_csv(path)
            self.assertEqual(path.read_bytes(), original)
            result.export_csv(path, overwrite=True)
        project.current = of.Epoch(1)
        project.reports = [definition]
        self.assertEqual(result.input_sha256, of.report_input_sha256(project))
        project = project.renamed_ground_station('station', 'New name')
        self.assertNotEqual(result.input_sha256, of.report_input_sha256(project))
        self.assertEqual(result.table.rows[0][1], '海岸,"一号"\n站')

    def test_all_four_templates_and_explicit_object_selection(self):
        project, definition = self.inputs()
        for kind, count in ((of.ReportKind.SCENARIO_SUMMARY, 7),
                            (of.ReportKind.OBJECT_SUMMARY, 1),
                            (of.ReportKind.GROUND_STATIONS, 1)):
            definition.kind = kind
            self.assertEqual(of.generate_report(project, definition).table.row_count, count)
        definition.object_ids = ['missing']
        with self.assertRaisesRegex(ValueError, 'missing'):
            of.generate_report(project, definition)
        project.reports = [definition]
        with tempfile.TemporaryDirectory() as directory:
            path = Path(directory) / 'report.orbitforge'
            project.save(path)
            restored = of.load_project(path)
            self.assertEqual(restored.reports[0].object_ids, ['missing'])

    def test_frame_conversion_uses_explicit_matching_eop(self):
        project, definition = self.inputs()
        manifest = Path(of.__file__).parent / 'data/eop/iers_c04_2000_01_01_2000_01_02/manifest.json'
        eop = of.load_eop_snapshot(manifest)
        project.earth_orientation = eop.data_info
        definition.frame = of.FrameId.GCRF
        with self.assertRaisesRegex(ValueError, 'requires confirmed EOP'):
            of.generate_report(project, definition)
        result = of.generate_report(project, definition, eop_manifest=manifest)
        fixed = project.to_scenario().sample([project.first])[0].states[0]
        expected = of.make_frame_provider().gcrf_to_itrf_state(project.first, eop.at(project.first)).inverse(fixed)
        for i in range(3):
            self.assertAlmostEqual(result.table.rows[0][4 + i], expected.position_m[i], places=9)
            self.assertAlmostEqual(result.table.rows[0][7 + i], expected.velocity_mps[i], places=9)
        self.assertEqual(result.data_sha256, project.data_lock.sha256)
        self.assertEqual(result.time_sha256, eop.data_info.time_data.sha256)

    def test_roundtrip_and_corruption_with_repaired_crcs(self):
        project, definition = self.inputs()
        definition.object_ids = ['station']
        definition.follow_scenario = False
        definition.first, definition.last = of.Epoch(17), of.Epoch(19)
        definition.step_nanoseconds = 1
        definition.time_scale = of.ReportTimeScale.TT
        project.reports = [definition]
        with tempfile.TemporaryDirectory() as directory:
            path = Path(directory) / 'reports.orbitforge'
            project.save(path)
            raw = path.read_bytes()
            restored = of.load_project(path)
            self.assertEqual(restored.reports[0].step_nanoseconds, 1)
            self.assertEqual(restored.reports[0].first.tai_nanoseconds, 17)
            self.assertEqual(restored.reports[0].time_scale, of.ReportTimeScale.TT)
            restored.save(path, overwrite=True)
            self.assertEqual(path.read_bytes(), raw)
            for mutation in (lambda b: None, lambda b: struct.pack('<I', 129) + b[4:],
                             lambda b: b + b'\x00', lambda b: b[:-4] + struct.pack('<I', 3)):
                path.write_bytes(rewrite_section(raw, b"RPTS", mutation))
                with self.assertRaises(of.ProjectError):
                    of.load_project(path)

    def test_limits_fail_before_sampling(self):
        project, definition = self.inputs()
        definition.step_nanoseconds = 1
        with self.assertRaisesRegex(ValueError, '200000'):
            of.generate_report(project, definition)
        definition.step_nanoseconds = 0
        with self.assertRaises(ValueError):
            of.estimate_report_rows(project, definition)

    def test_derived_templates_units_and_persistence(self):
        project, definition = self.inputs()
        orbit = of.OrbitInput()
        orbit.first, orbit.last = project.first, project.last
        orbit.initial = of.to_cartesian(of.KeplerianElements(project.first, 8e6, .1, .5, .6, .7, 0), 3.986004418e14)
        project.add_satellite(orbit)
        definition.kind = of.ReportKind.CLASSICAL_ELEMENTS
        result = of.generate_report(project, definition)
        self.assertEqual(result.table.row_count, 4)
        self.assertEqual(result.table.rows[0][3], 'GCRF')
        self.assertAlmostEqual(result.numeric_value(0, 4), 8000, delta=1e-7)
        self.assertAlmostEqual(result.numeric_value(0, 6), math.degrees(.5), delta=1e-10)
        self.assertEqual(result.column_unit(6), 'deg')
        self.assertIsNone(result.numeric_value(0, 2))
        definition.kind = of.ReportKind.ORBITAL_MOTION
        result = of.generate_report(project, definition)
        self.assertAlmostEqual(result.table.rows[0][4], 7.2e6, delta=1e-6)
        self.assertAlmostEqual(result.numeric_value(0, 6), 0, delta=1e-9)
        definition.kind = of.ReportKind.GEODETIC_POSITION
        definition.object_ids = ['station']
        result = of.generate_report(project, definition)
        self.assertEqual(result.table.rows[0][3], 'ITRF')
        self.assertAlmostEqual(result.numeric_value(0, 6), 0, delta=1e-8)
        project.reports = [definition]
        with tempfile.TemporaryDirectory() as directory:
            path = Path(directory) / 'derived.orbitforge'
            project.save(path)
            raw = path.read_bytes()
            self.assertEqual(struct.unpack_from('<I', raw, 8)[0], 11)
            self.assertEqual(of.load_project(path).reports[0].kind, of.ReportKind.GEODETIC_POSITION)


if __name__ == '__main__':
    unittest.main()
