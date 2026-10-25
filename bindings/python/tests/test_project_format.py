"""Current file schema, strict version rejection, and the public open boundary."""
from pathlib import Path
import struct
import tempfile
import unittest

import orbelle as ob
from project_format import read_sections, with_sections


FIXTURE = Path(__file__).resolve().parents[3] / "tests/integration/fixtures/project-current.orbelle"


class ProjectFormatTests(unittest.TestCase):
    def setUp(self):
        self.temp = tempfile.TemporaryDirectory()
        self.addCleanup(self.temp.cleanup)
        self.path = Path(self.temp.name) / "project.orbelle"
        self.raw = FIXTURE.read_bytes()

    def test_fixed_current_fixture_opens_with_preserved_inputs_and_relationships(self):
        self.assertEqual(struct.unpack_from("<I", self.raw, 8)[0], 54)
        self.path.write_bytes(self.raw)
        project = ob.Project.open(self.path)
        self.assertEqual(project.objects["satellite-a"].name, "观测卫星 A")
        low = ob.load_project_data(self.path)
        self.assertEqual(low.reports[4].query.kind, ob.ReportKind.COVERAGE_GRID)
        self.assertEqual(low.reports[4].query.coverage.east_rad, -2.9)
        self.assertFalse(low.reports[4].query.coverage.include_end_gaps)
        self.assertEqual(low.reports[4].query.region_id, "region-a")
        self.assertEqual([r.object_id for r in low.regions], ["region-a", "region-b"])
        self.assertEqual(low.regions[0].boundary.west_rad, 2.9)
        self.assertFalse(low.regions[0].display.show_3d)
        self.assertEqual(low.regions[1].boundary.vertices[1].longitude_rad, -3.0)
        self.assertEqual(low.regions[1].description, "Great-circle triangle")
        self.assertEqual(low.regions[0].grid_resolution_rad, .1)
        self.assertEqual(low.regions[1].grid_resolution_rad, .05)
        self.assertTrue(low.regions[0].display.fill)
        self.assertEqual(low.regions[0].display.opacity, .12)
        self.assertFalse(low.regions[1].display.fill)
        self.assertEqual(low.regions[1].display.opacity, .35)
        self.assertEqual(project.objects["satellite-b"].orbit.hpop.max_step_seconds, 45)
        self.assertEqual(project.objects["satellite-b"].orbit.hpop.solid_tides,
                         ob.orbits.SolidTideSettings("full", True, True))
        self.assertEqual(project.objects["satellite-b"].orbit.hpop.srp.sun_position_method, "apparent")
        hpop = project.objects["satellite-b"].orbit.hpop
        self.assertEqual(hpop.ocean_tides, ob.orbits.OceanTideSettings(False, "fixture-ocean", 30, 12))
        self.assertTrue(hpop.srp.boundary_mitigation)
        self.assertTrue(hpop.include_secular_variations)
        self.assertEqual(hpop.satellite_mass_kg, 1250)
        self.assertEqual(hpop.srp.additional_eclipsing_bodies, (
            ob.orbits.SrpEclipsingBody("mercury", 2440000.125),
            ob.orbits.SrpEclipsingBody("venus", 6051800)))
        self.assertEqual(hpop.integrator, "gauss_jackson")
        self.assertTrue(hpop.vop)
        self.assertFalse(hpop._native().uses_vop())
        self.assertEqual(hpop.correction, "pseudo")
        self.assertEqual(hpop.step_control, "fixed")
        self.assertEqual(hpop.interpolation, "hermite")
        self.assertEqual(hpop.interpolation_order, 5)
        self.assertEqual(hpop.interpolation_mu, 4.1e14)
        self.assertTrue(hpop.report_ephemeris_on_fixed_step)
        self.assertTrue(hpop.time_regularization)
        self.assertEqual(hpop.regularization_exponent, 1.25)
        self.assertEqual(hpop.steps_per_orbit, 240)
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
        self.assertEqual(project.objects["sensor-a"].orientation.x.radians, .125)
        self.assertEqual(project.objects["sensor-a"].orientation.y.radians, -.25)
        self.assertTrue(project.to_data().sensors[0].display.show_space_extension)
        self.assertEqual(project.analysis_status, ob.AnalysisStatus.UNRECORDED)
        self.assertEqual(project.to_data().reports[0].query.lighting_ephemeris, "fixture-sun")
        self.assertEqual(project.to_data().reports[1].query.kind, ob.ReportKind.ORBITAL_EVENTS)
        self.assertEqual(project.to_data().reports[1].query.event_tolerance_nanoseconds, 1000)
        self.assertEqual(project.to_data().reports[2].query.kind, ob.ReportKind.RELATIVE_MOTION)
        self.assertEqual(project.to_data().reports[3].query.kind, ob.ReportKind.INTERSATELLITE_ACCESS)
        self.assertEqual(project.to_data().reports[2].query.object_ids, ["satellite-a", "satellite-b"])
        project.save(self.path, overwrite=True)
        self.assertEqual(self.path.read_bytes(), self.raw)

    def test_coverage_records_reject_invalid_geometry_and_boolean_after_crc_repair(self):
        marker = struct.pack("<6dB", -.2, .3, 2.9, -2.9, .1, .2, 0)
        for offset, fmt, value in ((0, "d", float("nan")), (16, "d", 4),
                                   (32, "d", 0), (48, "B", 2)):
            sections = []
            for tag, version, body in read_sections(self.raw):
                if tag == b"RPTS":
                    body = bytearray(body)
                    self.assertEqual(body.count(marker), 2)
                    struct.pack_into("<" + fmt, body, body.index(marker) + offset, value)
                sections.append((tag, version, body))
            raw = with_sections(self.raw, sections)
            self.path.write_bytes(raw)
            with self.subTest(offset=offset), self.assertRaises(ob.ProjectError):
                ob.Project.open(self.path)
            self.assertEqual(self.path.read_bytes(), raw)

    def test_region_section_rejects_invalid_geometry_counts_and_flags(self):
        marker = struct.pack("<4dI", -.2, .3, 2.9, -2.9, 0)
        spacing_offset = 42 + 4 + len("Date line region")
        for offset, fmt, value in ((0, "d", float("nan")), (32, "I", 257), (36, "B", 2),
                                  (spacing_offset, "d", 0), (spacing_offset, "d", float("nan")),
                                  (spacing_offset+8, "B", 2), (spacing_offset+9, "d", -0.1),
                                  (spacing_offset+9, "d", 1.1), (spacing_offset+9, "d", float("nan"))):
            sections = []
            for tag, version, body in read_sections(self.raw):
                if tag == b"REGN":
                    body = bytearray(body)
                    struct.pack_into("<" + fmt, body, body.index(marker) + offset, value)
                sections.append((tag, version, body))
            raw = with_sections(self.raw, sections)
            self.path.write_bytes(raw)
            with self.subTest(offset=offset), self.assertRaises(ob.ProjectError):
                ob.Project.open(self.path)
            self.assertEqual(self.path.read_bytes(), raw)

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
            with self.assertRaisesRegex(ob.ProjectError,"weather update/source"):
                ob.Project.open(self.path)
            self.assertEqual(self.path.read_bytes(),raw)

    def test_invalid_sensor_space_extension_boolean_is_rejected(self):
        sections = []
        for tag, version, body in read_sections(self.raw):
            if tag == b"SNSR":
                self.assertEqual(version, 4)
                self.assertEqual(body[-1], 1)
                body = body[:-1] + b"\x02"
            sections.append((tag, version, body))
        raw = with_sections(self.raw, sections)
        self.path.write_bytes(raw)
        with self.assertRaises(ob.ProjectError):
            ob.Project.open(self.path)
        self.assertEqual(self.path.read_bytes(), raw)

    def test_invalid_shadow_boundary_flag_preserves_source(self):
        marker=struct.pack("<8d",1.25,.03,23000,4.56e-6,149597870700,696000000,6378137,1737400)
        sections=[]
        for tag,version,body in read_sections(self.raw):
            if tag==b"OBJS":
                body=bytearray(body)
                self.assertEqual(body.count(marker),1)
                self.assertEqual(body[body.index(marker)-1],1)
                body[body.index(marker)-1]=2
            sections.append((tag,version,body))
        raw=with_sections(self.raw,sections)
        self.path.write_bytes(raw)
        with self.assertRaises(ob.ProjectError):
            ob.Project.open(self.path)
        self.assertEqual(self.path.read_bytes(),raw)

    def test_invalid_additional_occultors_preserve_source_after_crc_repair(self):
        marker=struct.pack("<8d",1.25,.03,23000,4.56e-6,149597870700,696000000,6378137,1737400)
        for offset,fmt,value in ((0,"I",9),(0,"I",0xFFFFFFFF),(4,"I",0),(4,"I",1),
                (4,"I",10),(8,"d",-1),(8,"d",float("nan")),(8,"d",float("inf")),(16,"I",6)):
            sections=[]
            for tag,version,body in read_sections(self.raw):
                if tag==b"OBJS":
                    body=bytearray(body)
                    self.assertEqual(body.count(marker),1)
                    struct.pack_into("<"+fmt,body,body.index(marker)+len(marker)+offset,value)
                sections.append((tag,version,body))
            altered=with_sections(self.raw,sections)
            self.path.write_bytes(altered)
            with self.subTest(offset=offset,value=value), self.assertRaises(ob.ProjectError):
                ob.Project.open(self.path)
            self.assertEqual(self.path.read_bytes(),altered)

    def test_all_previous_and_unknown_versions_are_rejected_by_every_open_entry(self):
        for version in (*range(54), 55, 0xFFFFFFFF):
            raw = bytearray(self.raw)
            struct.pack_into("<I", raw, 8, version)
            self.path.write_bytes(raw)
            for open_project in (ob.load_project_data, ob.load_project,
                                 ob.Project.open, ob.Project.plan_open):
                with self.subTest(version=version, entry=open_project.__name__):
                    with self.assertRaisesRegex(ob.ProjectError, f"version {version};.*only version 54"):
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
                    with self.assertRaises(ob.ProjectError):
                        ob.load_project_data(self.path)

    def test_unknown_tide_mode_and_nonboolean_tides_or_relativity_rejected_with_valid_crcs(self):
        marker = struct.pack("<IIBIBBBI", 0, 0, 1, 2, 1, 1, 0, 13) + b"fixture-ocean" + struct.pack("<IIBdIBII7d", 30, 12, 0, 1250, 4, 1, 1, 1, 2e-12, .002, 2e-6, .005, 45, 12, 1000)
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
            with self.assertRaises(ob.ProjectError):
                ob.Project.open(self.path)
            self.assertEqual(self.path.read_bytes(), corrupted)

    def test_unknown_integrator_is_rejected_with_valid_crcs(self):
        marker = struct.pack("<IIBdIBII7d", 30, 12, 0, 1250, 4, 1, 1, 1, 2e-12, .002, 2e-6, .005, 45, 12, 1000)
        for value in (5, 0xFFFFFFFF):
            sections = []
            for tag, version, body in read_sections(self.raw):
                if tag == b"OBJS":
                    self.assertEqual(body.count(marker), 1)
                    body = bytearray(body)
                    struct.pack_into("<I", body, body.index(marker) + 17, value)
                sections.append((tag, version, body))
            corrupted = with_sections(self.raw, sections)
            self.path.write_bytes(corrupted)
            with self.assertRaisesRegex(ob.ProjectError, "Unsupported HPOP integrator"):
                ob.Project.open(self.path)
            self.assertEqual(self.path.read_bytes(), corrupted)

    def test_invalid_vop_boolean_is_rejected_with_valid_crcs(self):
        marker = struct.pack("<IIBdIBII7d", 30, 12, 0, 1250, 4, 1, 1, 1, 2e-12, .002, 2e-6, .005, 45, 12, 1000)
        for value in (2, 255):
            sections = []
            for tag, version, body in read_sections(self.raw):
                if tag == b"OBJS":
                    self.assertEqual(body.count(marker), 1)
                    body = bytearray(body)
                    body[body.index(marker) + 21] = value
                sections.append((tag, version, body))
            corrupted = with_sections(self.raw, sections)
            self.path.write_bytes(corrupted)
            with self.assertRaises(ob.ProjectError):
                ob.Project.open(self.path)
            self.assertEqual(self.path.read_bytes(), corrupted)

    def test_unknown_correction_is_rejected_with_valid_crcs(self):
        marker = struct.pack("<IIBdIBII7d", 30, 12, 0, 1250, 4, 1, 1, 1, 2e-12, .002, 2e-6, .005, 45, 12, 1000)
        for value in (2, 0xFFFFFFFF):
            sections = []
            for tag, version, body in read_sections(self.raw):
                if tag == b"OBJS":
                    self.assertEqual(body.count(marker), 1)
                    body = bytearray(body)
                    struct.pack_into("<I", body, body.index(marker) + 22, value)
                sections.append((tag, version, body))
            corrupted = with_sections(self.raw, sections)
            self.path.write_bytes(corrupted)
            with self.assertRaisesRegex(ob.ProjectError, "Unsupported HPOP predictor corrector scheme"):
                ob.Project.open(self.path)
            self.assertEqual(self.path.read_bytes(), corrupted)

    def test_unknown_step_control_is_rejected_with_valid_crcs(self):
        marker = struct.pack("<IIBdIBII7d", 30, 12, 0, 1250, 4, 1, 1, 1, 2e-12, .002, 2e-6, .005, 45, 12, 1000)
        for value in (2, 0xFFFFFFFF):
            sections = []
            for tag, version, body in read_sections(self.raw):
                if tag == b"OBJS":
                    self.assertEqual(body.count(marker), 1)
                    body = bytearray(body)
                    struct.pack_into("<I", body, body.index(marker) + 26, value)
                sections.append((tag, version, body))
            corrupted = with_sections(self.raw, sections)
            self.path.write_bytes(corrupted)
            with self.assertRaisesRegex(ob.ProjectError, "Unsupported HPOP step size control"):
                ob.Project.open(self.path)
            self.assertEqual(self.path.read_bytes(), corrupted)

    def test_invalid_mass_is_rejected_even_with_valid_crcs_and_disabled_forces(self):
        marker = struct.pack("<IIBIBBBI", 0, 0, 1, 2, 1, 1, 0, 13) + b"fixture-ocean" + struct.pack("<IIBdIBII7d", 30, 12, 0, 1250, 4, 1, 1, 1, 2e-12, .002, 2e-6, .005, 45, 12, 1000)
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
            with self.subTest(mass=mass), self.assertRaises(ob.ProjectError):
                ob.Project.open(self.path)
            self.assertEqual(self.path.read_bytes(), corrupted)

    def test_invalid_srp_sun_method_is_rejected_with_valid_crcs(self):
        sections = read_sections(self.raw)
        marker = struct.pack("<BIIBBB8d", 0, 2, 2, 1, 0, 1, 1.25, .03, 23000,
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
        with self.assertRaisesRegex(ob.ProjectError, "SRP Sun position method"):
            ob.Project.open(self.path)
        self.assertEqual(self.path.read_bytes(), corrupted)

    def test_section_order_is_independent_and_writer_is_canonical(self):
        self.path.write_bytes(with_sections(self.raw, reversed(read_sections(self.raw))))
        project = ob.Project.open(self.path)
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
                with self.assertRaises(ob.ProjectError):
                    ob.load_project_data(self.path)

    def test_invalid_interpolation_fields_with_valid_crcs_are_rejected(self):
        prefix = struct.pack("<IIBdIBII7d", 30, 12, 0, 1250, 4, 1, 1, 1, 2e-12, .002, 2e-6, .005, 45, 12, 1000)
        for offset, encoding, value in ((0, "I", 3), (4, "I", 4), (4, "I", 30), (4, "I", 0xFFFFFFFF),
                (8, "d", 0), (8, "d", -1), (8, "d", float("nan")), (8, "d", float("inf")), (16, "B", 2)):
            with self.subTest(offset=offset, value=value):
                sections = []
                for tag, version, original in read_sections(self.raw):
                    body = bytearray(original)
                    if tag == b"OBJS":
                        self.assertEqual(body.count(prefix), 1)
                        struct.pack_into("<" + encoding, body, body.index(prefix) + len(prefix) + offset, value)
                    sections.append((tag, version, body))
                self.path.write_bytes(with_sections(self.raw, sections))
                with self.assertRaises(ob.ProjectError):
                    ob.load_project_data(self.path)


    def test_invalid_regularization_fields_with_valid_crcs_preserve_source(self):
        prefix = struct.pack("<IIBdIBII7dIIdB", 30, 12, 0, 1250, 4, 1, 1, 1, 2e-12, .002, 2e-6,
                             .005, 45, 12, 1000, 1, 5, 4.1e14, 1)
        for enabled in (0, 1):
            for offset, encoding, value in ((0, "B", 2), (1, "d", 0), (1, "d", -1),
                    (1, "d", 10.001), (1, "d", float("nan")), (1, "d", float("inf")),
                    (9, "I", 0), (9, "I", 0x80000000), (9, "I", 0xFFFFFFFF)):
                with self.subTest(enabled=enabled, offset=offset, value=value):
                    sections = []
                    for tag, version, original in read_sections(self.raw):
                        body = bytearray(original)
                        if tag == b"OBJS":
                            self.assertEqual(body.count(prefix), 1)
                            start = body.index(prefix) + len(prefix)
                            struct.pack_into("<B", body, start, enabled)
                            struct.pack_into("<" + encoding, body, start + offset, value)
                        sections.append((tag, version, body))
                    altered = with_sections(self.raw, sections)
                    self.path.write_bytes(altered)
                    with self.assertRaises(ob.ProjectError):
                        ob.Project.open(self.path)
                    self.assertEqual(self.path.read_bytes(), altered)


if __name__ == "__main__":
    unittest.main()
