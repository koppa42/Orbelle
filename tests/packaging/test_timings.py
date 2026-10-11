"""Timing summaries retain failures and distinguish nested command reports."""
import importlib.util
import json
from pathlib import Path
import tempfile
import unittest

ROOT = Path(__file__).resolve().parents[2]
spec = importlib.util.spec_from_file_location("timings", ROOT / "tools/ci/summarize_timings.py")
timings = importlib.util.module_from_spec(spec)
spec.loader.exec_module(timings)


class TimingTests(unittest.TestCase):
    def test_nested_reports_preserve_failure_duration_and_escape_table_cells(self):
        with tempfile.TemporaryDirectory() as directory:
            root = Path(directory)
            (root / "python").mkdir()
            (root / "commands.json").write_text(json.dumps([
                {"name": "python-api", "elapsed_seconds": 4.5, "exit_code": 1}]))
            (root / "python/commands.json").write_text(json.dumps([
                {"name": "compile|<test>", "elapsed_seconds": 3.25, "exit_code": 1},
                {"name": "old-format", "exit_code": 0}]))
            result = timings.summarize(root)
            self.assertIn("| python-api | 4.50 | 1 |", result)
            self.assertIn("### python", result)
            self.assertIn("| compile&#124;&lt;test&gt; | 3.25 | 1 |", result)
            self.assertIn("| old-format | unavailable | 0 |", result)
            self.assertIn("must not be added", result)

    def test_setup_failure_without_reports_is_reported(self):
        with tempfile.TemporaryDirectory() as directory:
            self.assertIn("No command timing reports", timings.summarize(Path(directory) / "missing"))


if __name__ == "__main__":
    unittest.main()
