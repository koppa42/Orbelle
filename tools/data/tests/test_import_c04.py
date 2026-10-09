from datetime import date
import hashlib
import importlib.util
import json
from pathlib import Path
import tempfile
import unittest


ROOT = Path(__file__).resolve().parents[3]
spec = importlib.util.spec_from_file_location("import_c04", ROOT / "tools/data/import_c04.py")
import_c04 = importlib.util.module_from_spec(spec)
spec.loader.exec_module(import_c04)
SAMPLE = ROOT / "data/samples/eop/iers_c04_2016_12_28_2017_01_04/eop.c04"


class ImportC04Tests(unittest.TestCase):
    def setUp(self):
        parent = ROOT / "out/tests/import-c04"
        parent.mkdir(parents=True, exist_ok=True)
        self.sandbox = tempfile.TemporaryDirectory(dir=parent)
        self.addCleanup(self.sandbox.cleanup)
        self.directory = Path(self.sandbox.name)
        self.source = self.directory / "input.c04"
        self.source.write_bytes(SAMPLE.read_bytes())
        self.output = self.directory / "snapshot"

    def make(self):
        return import_c04.make_snapshot(self.source, self.output, date(2016, 12, 28),
                                       date(2017, 1, 4), "test-snapshot", "test-source", "2026-10-09")

    def test_preserves_bytes_hash_and_existing_snapshots(self):
        manifest = self.make()
        self.assertEqual((self.output / "eop.c04").read_bytes(), SAMPLE.read_bytes())
        self.assertEqual(manifest["sha256"], hashlib.sha256(SAMPLE.read_bytes()).hexdigest())
        self.assertEqual(json.loads((self.output / "manifest.json").read_text()), manifest)
        with self.assertRaises(FileExistsError):
            self.make()

    def test_invalid_fields_are_rejected_before_creating_output(self):
        original = SAMPLE.read_bytes().splitlines(keepends=True)
        index = next(i for i, line in enumerate(original) if line.strip() and not line.startswith(b"#"))
        # Full-row indices: xp error=13, LOD=12, DUT1=7, xp=5.
        for column, value in ((13, b"-0.5"), (12, b"1"), (7, b"2"), (5, b"nan"), (5, b"10")):
            with self.subTest(column=column, value=value):
                lines = original.copy()
                fields = lines[index].split()
                fields[column] = value
                lines[index] = b" ".join(fields) + b"\n"
                self.source.write_bytes(b"".join(lines))
                with self.assertRaises(ValueError):
                    self.make()
                self.assertFalse(self.output.exists())

    def test_missing_day_is_rejected(self):
        lines = SAMPLE.read_bytes().splitlines(keepends=True)
        self.source.write_bytes(b"".join(line for line in lines if not line.startswith(b"2016  12  31")))
        with self.assertRaises(ValueError):
            self.make()
        self.assertFalse(self.output.exists())


if __name__ == "__main__":
    unittest.main()
