from pathlib import Path
import math
import struct
import tempfile
import unittest
import zlib
# Native value/format compatibility tests; public editing is in test_objects.py.
from orbitforge import _native as of
from project_fixtures import legacy_objects, legacy_stations


class TrackStyleTests(unittest.TestCase):
    @staticmethod
    def project():
        project = of.Project('Styles', of.Epoch(0), of.Epoch(7200000000000))
        orbit = of.OrbitInput()
        display = of.OrbitDisplay()
        display.line_color = 0xE9A2BA
        display.line_style = of.LineStyle.DASH_DOT
        display.line_width = 3.25
        project.add_satellite(orbit, display)
        return project

    @staticmethod
    def rewrite(raw, mutate, version=10):
        sections, offset = [], 24
        while offset < len(raw):
            tag, schema, size, _ = struct.unpack_from('<4sIQI', raw, offset)
            body = raw[offset + 20:offset + 20 + size]
            offset += 20 + size
            if version < 10 and tag == b'SNSR':
                continue
            if version < 10 and tag == b'GSTN':
                body, schema = legacy_stations(body), 1
            if version < 9 and tag == b'OBJS':
                body, schema = legacy_objects(body), 2
            if tag == b'STYL':
                body = mutate(body)
            if body is not None:
                sections.append(struct.pack('<4sIQI', tag, schema, len(body), zlib.crc32(body)) + body)
        payload = b''.join(sections)
        return raw[:8] + struct.pack('<IQI', version, len(payload), zlib.crc32(payload)) + payload

    def test_styles_roundtrip_copy_and_do_not_change_numerical_results_or_report_hash(self):
        project = self.project()
        states = project.to_scenario().sample([project.first, project.last])[0].states
        digest = of.report_input_sha256(project)
        revised = project.with_satellite_copy('satellite-1', 'copy', 'Copy')
        self.assertEqual(revised.satellites[1].display.line_color, 0xE9A2BA)
        satellites = revised.satellites
        satellites[1].display.line_color = 0x123456
        satellites[1].display.line_style = of.LineStyle.DOTTED
        revised.satellites = satellites
        with tempfile.TemporaryDirectory() as directory:
            path = Path(directory) / 'styles.orbitforge'
            revised.save(path)
            restored = of.load_project(path)
            self.assertEqual(restored.satellites[0].display.line_color, 0xE9A2BA)
            self.assertEqual(restored.satellites[1].display.line_color, 0x123456)
            self.assertEqual(restored.satellites[1].display.line_style, of.LineStyle.DOTTED)
            self.assertEqual(restored.satellites[1].display.line_width, 3.25)
            original_bytes = path.read_bytes()
            restored.save(path, overwrite=True)
            self.assertEqual(path.read_bytes(), original_bytes)
        satellites = project.satellites
        satellites[0].display = of.OrbitDisplay()
        project.satellites = satellites
        self.assertEqual(of.report_input_sha256(project), digest)
        after = project.to_scenario().sample([project.first, project.last])[0].states
        for a, b in zip(states, after):
            self.assertEqual(a.position_m, b.position_m)
            self.assertEqual(a.velocity_mps, b.velocity_mps)

    def test_legacy_defaults_and_style_integrity_even_after_repairing_crcs(self):
        with tempfile.TemporaryDirectory() as directory:
            path = Path(directory) / 'styles.orbitforge'
            self.project().save(path)
            raw = path.read_bytes()
            # v7 had the same object records but no STYL section.
            path.write_bytes(self.rewrite(raw, lambda body: None, version=7))
            display = of.load_project(path).satellites[0].display
            self.assertEqual(display.line_color, 0x73CDEE)
            self.assertEqual(display.line_width, 1.5)
            self.assertEqual(display.line_style, of.LineStyle.SOLID)
            for mutate in (
                lambda b: None,
                lambda b: struct.pack('<I', 2) + b[4:],
                lambda b: b[:-16] + struct.pack('<I', 0x1000000) + b[-12:],
                lambda b: b[:-12] + struct.pack('<I', 4) + b[-8:],
                lambda b: b[:-8] + struct.pack('<d', math.nan),
                lambda b: b[:-8] + struct.pack('<d', 0.49),
                lambda b: b[:-8] + struct.pack('<d', 8.01),
                lambda b: b.replace(b'satellite-1', b'unknown-id1'),
                lambda b: b + b'\0',
            ):
                path.write_bytes(self.rewrite(raw, mutate))
                with self.assertRaises(of.ProjectError):
                    of.load_project(path)

    def test_invalid_style_is_rejected_before_replacing_existing_project(self):
        with tempfile.TemporaryDirectory() as directory:
            project = self.project()
            path = Path(directory) / 'styles.orbitforge'
            project.save(path)
            raw = path.read_bytes()
            for value in [0, 8.1, math.inf, math.nan]:
                satellites = project.satellites
                satellites[0].display.line_width = value
                project.satellites = satellites
                with self.assertRaises(of.ProjectError):
                    project.save(path, overwrite=True)
                self.assertEqual(path.read_bytes(), raw)
