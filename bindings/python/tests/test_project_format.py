"""Current file schema, strict version rejection, and the public open boundary."""
from pathlib import Path
import struct
import tempfile
import unittest

import orbitforge as of
from project_format import read_sections, with_sections


FIXTURE = Path(__file__).resolve().parents[3] / "tests/integration/fixtures/project-current.orbitforge"


class ProjectFormatTests(unittest.TestCase):
    def setUp(self):
        self.temp = tempfile.TemporaryDirectory()
        self.addCleanup(self.temp.cleanup)
        self.path = Path(self.temp.name) / "project.orbitforge"
        self.raw = FIXTURE.read_bytes()

    def test_fixed_current_fixture_opens_with_preserved_inputs_and_relationships(self):
        self.assertEqual(struct.unpack_from("<I", self.raw, 8)[0], 32)
        self.path.write_bytes(self.raw)
        project = of.Project.open(self.path)
        self.assertEqual(project.objects["satellite-a"].name, "观测卫星 A")
        self.assertEqual(project.objects["satellite-b"].orbit.hpop.max_step_seconds, 45)
        self.assertEqual(project.objects["satellite-b"].orbit.hpop.solid_tides,
                         of.orbits.SolidTideSettings("full", True, True))
        self.assertEqual(project.objects["satellite-b"].orbit.hpop.srp.sun_position_method, "apparent")
        hpop = project.objects["satellite-b"].orbit.hpop
        self.assertEqual(hpop.ocean_tides, of.orbits.OceanTideSettings(False, "fixture-ocean", 30, 12))
        self.assertTrue(hpop.include_secular_variations)
        self.assertEqual(hpop.satellite_mass_kg, 1250)
        self.assertEqual(hpop.integrator, "bulirsch_stoer")
        self.assertEqual(hpop.drag_area_m2, 37.5)
        self.assertEqual(hpop.srp_area_m2, 37.5)
        drag = hpop.drag
        self.assertTrue(drag.use_approximate_altitude)
        self.assertEqual(drag.weather_model, "fixture-weather")
        self.assertEqual(drag.geomagnetic_update, "three_hourly_cubic_spline")
        self.assertEqual(drag.geomagnetic_source, "kp")
        plan = project.objects["satellite-b"].orbit.mission
        self.assertEqual(plan.propagation, "hpop")
        self.assertEqual(plan.segments[0].duration.nanoseconds, 200_000_000_001)
        self.assertEqual(plan.segments[1].delta_v_mps, (12, -3, .5))
        self.assertEqual(project.objects["sensor-a"].parent, project.objects["satellite-a"])
        self.assertEqual(project.analysis_status, of.AnalysisStatus.UNRECORDED)
        project.save(self.path, overwrite=True)
        self.assertEqual(self.path.read_bytes(), self.raw)

    def test_unknown_weather_enum_is_rejected_with_valid_crcs(self):
        for value in (4, 0xFFFFFFFF):
            sections=[]
            for tag, version, body in read_sections(self.raw):
                if tag == b"OBJS":
                    body=bytearray(body)
                    offset=body.index(b"fixture-weather")+len(b"fixture-weather")
                    struct.pack_into("<I",body,offset,value)
                sections.append((tag,version,body))
            raw=with_sections(self.raw,sections)
            self.path.write_bytes(raw)
            with self.assertRaisesRegex(of.ProjectError,"weather update/source"):
                of.Project.open(self.path)
            self.assertEqual(self.path.read_bytes(),raw)

    def test_all_previous_and_unknown_versions_are_rejected_by_every_open_entry(self):
        for version in (*range(32), 33, 0xFFFFFFFF):
            raw = bytearray(self.raw)
            struct.pack_into("<I", raw, 8, version)
            self.path.write_bytes(raw)
            for open_project in (of.load_project_data, of.load_project,
                                 of.Project.open, of.Project.plan_open):
                with self.subTest(version=version, entry=open_project.__name__):
                    with self.assertRaisesRegex(of.ProjectError, f"version {version};.*only version 32"):
                        open_project(self.path)
                    self.assertEqual(self.path.read_bytes(), raw)

    def test_missing_duplicate_unknown_or_misversioned_sections_fail_with_valid_crcs(self):
        sections = read_sections(self.raw)
        for index, (tag, version, body) in enumerate(sections):
            mutations = {
                "missing": sections[:index] + sections[index + 1:],
                "duplicate": sections + [(tag, version, body)],
                "unknown": sections[:index] + [(b"NOPE", version, body)] + sections[index + 1:],
            }
            for unsupported in (0, version - 1, version + 1):
                mutations[f"section version {unsupported}"] = (
                    sections[:index] + [(tag, unsupported, body)] + sections[index + 1:]
                )
            for label, mutation in mutations.items():
                with self.subTest(tag=tag, mutation=label):
                    self.path.write_bytes(with_sections(self.raw, mutation))
                    with self.assertRaises(of.ProjectError):
                        of.load_project_data(self.path)

    def test_unknown_tide_mode_and_nonboolean_tides_or_relativity_rejected_with_valid_crcs(self):
        marker = struct.pack("<IIBIBBBI", 0, 0, 1, 2, 1, 1, 0, 13) + b"fixture-ocean" + struct.pack("<IIBdI7d", 30, 12, 0, 1250, 3, 2e-12, .002, 2e-6, .005, 45, 12, 1000)
        for offset, fmt, value in ((8, "B", 2), (9, "I", 3), (13, "B", 2), (14, "B", 2), (15, "B", 2), (41, "B", 2), (33, "I", 2191), (37, "I", 31)):
            sections = []
            for tag, version, body in read_sections(self.raw):
                if tag == b"OBJS":
                    self.assertEqual(body.count(marker), 1)
                    body = bytearray(body)
                    struct.pack_into("<" + fmt, body, body.index(marker) + offset, value)
                sections.append((tag, version, body))
            corrupted = with_sections(self.raw, sections)
            self.path.write_bytes(corrupted)
            with self.assertRaises(of.ProjectError):
                of.Project.open(self.path)
            self.assertEqual(self.path.read_bytes(), corrupted)

    def test_unknown_integrator_is_rejected_with_valid_crcs(self):
        marker = struct.pack("<IIBdI7d", 30, 12, 0, 1250, 3, 2e-12, .002, 2e-6, .005, 45, 12, 1000)
        for value in (4, 0xFFFFFFFF):
            sections = []
            for tag, version, body in read_sections(self.raw):
                if tag == b"OBJS":
                    self.assertEqual(body.count(marker), 1)
                    body = bytearray(body)
                    struct.pack_into("<I", body, body.index(marker) + 17, value)
                sections.append((tag, version, body))
            corrupted = with_sections(self.raw, sections)
            self.path.write_bytes(corrupted)
            with self.assertRaisesRegex(of.ProjectError, "Unsupported HPOP integrator"):
                of.Project.open(self.path)
            self.assertEqual(self.path.read_bytes(), corrupted)

    def test_invalid_mass_is_rejected_even_with_valid_crcs_and_disabled_forces(self):
        marker = struct.pack("<IIBIBBBI", 0, 0, 1, 2, 1, 1, 0, 13) + b"fixture-ocean" + struct.pack("<IIBdI7d", 30, 12, 0, 1250, 3, 2e-12, .002, 2e-6, .005, 45, 12, 1000)
        for mass in (0, -1, float("nan"), float("inf"), 5e-324):
            sections = []
            for tag, version, body in read_sections(self.raw):
                if tag == b"OBJS":
                    self.assertEqual(body.count(marker), 1)
                    body = bytearray(body)
                    struct.pack_into("<d", body, body.index(marker) + 42, mass)
                sections.append((tag, version, body))
            corrupted = with_sections(self.raw, sections)
            self.path.write_bytes(corrupted)
            with self.subTest(mass=mass), self.assertRaises(of.ProjectError):
                of.Project.open(self.path)
            self.assertEqual(self.path.read_bytes(), corrupted)

    def test_invalid_srp_sun_method_is_rejected_with_valid_crcs(self):
        sections = read_sections(self.raw)
        marker = struct.pack("<BIIBB8d", 0, 2, 2, 1, 0, 1.25, .03, 23000,
                             4.56e-6, 149597870700, 696000000, 6378137, 1737400)
        altered = []
        for tag, version, body in sections:
            if tag == b"OBJS":
                self.assertEqual(body.count(marker), 1)
                body = bytearray(body)
                struct.pack_into("<I", body, body.index(marker) + 5, 3)
            altered.append((tag, version, body))
        corrupted = with_sections(self.raw, altered)
        self.path.write_bytes(corrupted)
        with self.assertRaisesRegex(of.ProjectError, "SRP Sun position method"):
            of.Project.open(self.path)
        self.assertEqual(self.path.read_bytes(), corrupted)

    def test_section_order_is_independent_and_writer_is_canonical(self):
        self.path.write_bytes(with_sections(self.raw, reversed(read_sections(self.raw))))
        project = of.Project.open(self.path)
        project.save(self.path, overwrite=True)
        self.assertEqual(self.path.read_bytes(), self.raw)

    def test_mission_fields_are_rejected_even_after_repairing_crcs(self):
        sections = read_sections(self.raw)
        objects = next(body for tag, _, body in sections if tag == b"OBJS")
        marker = struct.pack("<III", 1, 3, 0) + struct.pack("<I", 5) + b"Drift"
        start = objects.index(marker)
        # Offsets follow the independently assembled fixture, not the native writer.
        cases = ((0, "I", 2), (4, "I", 0), (4, "I", 257), (8, "I", 2),
                 (21, "q", -1), (21, "q", 1), (42, "I", 3), (46, "d", float("nan")))
        for offset, fmt, value in cases:
            body = bytearray(objects)
            struct.pack_into("<" + fmt, body, start + offset, value)
            altered = [(tag, version, body if tag == b"OBJS" else original)
                       for tag, version, original in sections]
            with self.subTest(offset=offset, value=value):
                self.path.write_bytes(with_sections(self.raw, altered))
                with self.assertRaises(of.ProjectError):
                    of.load_project_data(self.path)


if __name__ == "__main__":
    unittest.main()
