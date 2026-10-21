"""Desktop launch failures must remain observable and leave no owned child running."""
import os
from pathlib import Path
import subprocess
import sys
import tempfile
import unittest

sys.path.insert(0, str(Path(__file__).resolve().parents[1]))
from desktop_test_support import (desktop_environment, logged_process, run_logged,
                                  timeout_message, wait_until)
from run_graphics_smoke import expected_failure


class DesktopLaunchTests(unittest.TestCase):
    def setUp(self):
        self.temporary = tempfile.TemporaryDirectory()
        self.addCleanup(self.temporary.cleanup)
        self.directory = Path(self.temporary.name)

    def command(self, source):
        return [sys.executable, "-u", "-c", source]

    def launch(self, source, timeout=5):
        return run_logged(self.command(source), cwd=self.directory, env=os.environ.copy(),
                          directory=self.directory, timeout=timeout)

    def test_environment_is_child_only_and_modes_do_not_leak_validation(self):
        original = {"PATH": "existing-path", "QT_QPA_PLATFORM": "offscreen",
                    "VK_INSTANCE_LAYERS": "inherited-layer", "VK_LAYER_ENABLES": "old",
                    "VK_LOADER_LAYERS_DISABLE": "*", "VK_LAYER_VALIDATE_SYNC": "0"}
        saved = original.copy()
        correctness = desktop_environment(self.directory, scale=1.5, validation=True,
                                          environ=original)
        timing = desktop_environment(self.directory, environ=correctness)
        self.assertEqual(original, saved)
        self.assertEqual(correctness["QT_SCALE_FACTOR"], "1.5")
        self.assertEqual(correctness["QT_QPA_PLATFORM"], "windows")
        self.assertEqual(correctness["VK_LAYER_VALIDATE_SYNC"], "1")
        self.assertNotIn("VK_LOADER_LAYERS_DISABLE", correctness)
        for key in ("VK_INSTANCE_LAYERS", "VK_LAYER_ENABLES", "VK_LAYER_VALIDATE_SYNC"):
            self.assertNotIn(key, timing)
        self.assertEqual(timing["PROJ_NETWORK"], "OFF")
        self.assertTrue(correctness["PATH"].endswith(os.pathsep + "existing-path"))

    def test_invalid_scale_is_rejected_before_launch(self):
        for scale in (0, 4.1, float("nan")):
            with self.subTest(scale=scale), self.assertRaises(ValueError):
                desktop_environment(self.directory, scale=scale)

    def test_success_and_nonzero_exit_preserve_both_logs(self):
        for code in (0, 7):
            result = self.launch(f"import sys; print('output'); print('diagnostic', file=sys.stderr); sys.exit({code})")
            self.assertEqual(result.returncode, code)
            self.assertIn("output", (self.directory / "stdout.log").read_text())
            self.assertIn("diagnostic", (self.directory / "stderr.log").read_text())

    def test_timeout_preserves_partial_logs_and_reports_actual_limit(self):
        with self.assertRaises(subprocess.TimeoutExpired) as raised:
            self.launch("import time; print('started'); time.sleep(30)", timeout=1)
        self.assertEqual(timeout_message(raised.exception), "External 1-second timeout")
        self.assertIn("started", (self.directory / "stdout.log").read_text())
        self.assertTrue((self.directory / "stderr.log").exists())

    def test_exception_reaps_only_the_owned_child(self):
        with self.assertRaisesRegex(RuntimeError, "abort capture"):
            with logged_process(self.command("import time; time.sleep(30)"),
                                cwd=self.directory, env=os.environ.copy(),
                                directory=self.directory) as child:
                raise RuntimeError("abort capture")
        self.assertIsNotNone(child.poll())
        # All log handles must also be closed after an exceptional exit (Windows).
        (self.directory / "stdout.log").rename(self.directory / "closed.log")

    def test_readiness_detects_early_exit(self):
        with logged_process(self.command("raise SystemExit(9)"), cwd=self.directory,
                            env=os.environ.copy(), directory=self.directory) as child:
            child.wait(timeout=5)
            with self.assertRaisesRegex(RuntimeError, "exited early with 9"):
                wait_until(lambda: False, child, 5)

    def test_expected_failure_requires_both_exit_code_and_diagnostic(self):
        for code, text, passed in ((1, "expected", True), (0, "expected", False),
                                   (1, "unrelated crash", False)):
            result = expected_failure(
                self.command(f"import sys; print({text!r}, file=sys.stderr); sys.exit({code})"),
                os.environ.copy(), self.directory, b"expected")
            self.assertEqual(result["passed"], passed)


if __name__ == "__main__":
    unittest.main()
