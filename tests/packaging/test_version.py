"""Release tags must describe the version actually built by every entry point."""
import importlib.util
from pathlib import Path
import tempfile
import unittest


ROOT = Path(__file__).resolve().parents[2]
spec = importlib.util.spec_from_file_location("release_version", ROOT / "tools/ci/version.py")
version = importlib.util.module_from_spec(spec)
spec.loader.exec_module(version)


class VersionTests(unittest.TestCase):
    def setUp(self):
        self.directory = tempfile.TemporaryDirectory()
        self.addCleanup(self.directory.cleanup)
        self.root = Path(self.directory.name)
        for name in ("CMakeLists.txt", "pyproject.toml", "vcpkg.json"):
            (self.root / name).write_bytes((ROOT / name).read_bytes())

    def test_bump_updates_all_build_entry_points_and_tag_check(self):
        self.assertEqual(version.set_version(self.root, "1.2.3"), "1.2.3")
        self.assertEqual(set(version.read_versions(self.root).values()), {"1.2.3"})
        self.assertEqual(version.check(self.root, "v1.2.3"), "1.2.3")
        with self.assertRaises(ValueError):
            version.check(self.root, "v1.2.4")

    def test_invalid_versions_and_tags_cannot_publish(self):
        before = {p.name: p.read_bytes() for p in self.root.iterdir()}
        for invalid in ("v1.2.3", "1.2", "1.2.3-rc1", "01.2.3", "1.2.3\n", "1.2.3;echo bad"):
            with self.subTest(invalid=invalid), self.assertRaises(ValueError):
                version.set_version(self.root, invalid)
        self.assertEqual(before, {p.name: p.read_bytes() for p in self.root.iterdir()})
        current = version.check(self.root)
        for tag in (current, "v" + current + "-rc1", "v" + current + "\n"):
            with self.subTest(tag=tag), self.assertRaises(ValueError):
                version.check(self.root, tag)

    def test_mismatched_manifests_fail_before_a_build(self):
        path = self.root / "vcpkg.json"
        path.write_text(path.read_text().replace(version.check(self.root), "9.8.7"))
        with self.assertRaisesRegex(ValueError, "disagree"):
            version.check(self.root)

    def test_python_baseline_must_be_in_the_release_matrix(self):
        path = self.root / "pyproject.toml"
        path.write_text(path.read_text().replace('baseline-python = "3.13"', 'baseline-python = "3.99"'))
        with self.assertRaisesRegex(ValueError, "Python CI matrix"):
            version.python_versions(self.root)


if __name__ == "__main__":
    unittest.main()
