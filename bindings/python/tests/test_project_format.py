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
        self.assertEqual(struct.unpack_from("<I", self.raw, 8)[0], 11)
        self.path.write_bytes(self.raw)
        project = of.Project.open(self.path)
        self.assertEqual(project.objects["satellite-a"].name, "观测卫星 A")
        self.assertEqual(project.objects["sensor-a"].parent, project.objects["satellite-a"])
        self.assertEqual(project.analysis_status, of.AnalysisStatus.UNRECORDED)
        project.save(self.path, overwrite=True)
        self.assertEqual(self.path.read_bytes(), self.raw)

    def test_all_previous_and_unknown_versions_are_rejected_by_every_open_entry(self):
        for version in (*range(11), 12, 0xFFFFFFFF):
            raw = bytearray(self.raw)
            struct.pack_into("<I", raw, 8, version)
            self.path.write_bytes(raw)
            for open_project in (of.load_project_data, of.load_project,
                                 of.Project.open, of.Project.plan_open):
                with self.subTest(version=version, entry=open_project.__name__):
                    with self.assertRaisesRegex(of.ProjectError, f"version {version};.*only version 11"):
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

    def test_section_order_is_independent_and_writer_is_canonical(self):
        self.path.write_bytes(with_sections(self.raw, reversed(read_sections(self.raw))))
        project = of.Project.open(self.path)
        project.save(self.path, overwrite=True)
        self.assertEqual(self.path.read_bytes(), self.raw)


if __name__ == "__main__":
    unittest.main()
