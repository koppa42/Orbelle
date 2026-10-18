"""Regular CI preserves full numerical coverage; sanitizer runs stay explicit."""

import argparse
import importlib.util
from pathlib import Path
import sys
import tempfile
import unittest
from unittest.mock import patch


ROOT = Path(__file__).resolve().parents[2]
spec = importlib.util.spec_from_file_location("check_linux", ROOT / "tools/ci/check_linux.py")
linux = importlib.util.module_from_spec(spec)
with patch.object(sys, "path", [str(ROOT / "tools/ci"), *sys.path]):
    spec.loader.exec_module(linux)


class LinuxStagesTests(unittest.TestCase):
    def test_regular_stages_and_explicit_sanitizer_stage_remain_separate(self):
        self.assertEqual(linux.stages_for("package"), ("dependencies", "release", "wheel", "export"))
        self.assertEqual(linux.stages_for("sanitizers"), ("dependencies", "sanitizers"))
        self.assertIn("sanitizers", linux.stages_for("all"))
        with tempfile.TemporaryDirectory() as directory:
            args = argparse.Namespace(source=ROOT, work=Path(directory) / "work",
                                      reports=Path(directory) / "reports", delivery=None, jobs=1)
            check = linux.Check(args)
            with patch.object(check, "dependencies") as deps, \
                 patch.object(check, "build") as build, patch.object(check, "run") as run:
                check.tests()
                deps.assert_called_once()
                build.assert_called_once_with(install=False)
                self.assertEqual(run.call_args.args[0], "python-api")

    def test_release_and_sanitizers_run_unfiltered_with_failure_detection(self):
        for sanitizer in (False, True):
            with self.subTest(sanitizer=sanitizer), tempfile.TemporaryDirectory() as directory:
                args = argparse.Namespace(source=ROOT, work=Path(directory) / "work",
                                          reports=Path(directory) / "reports", delivery=None, jobs=1)
                check = linux.Check(args)
                name = "sanitizers" if sanitizer else "release"
                build = check.work / "build" / name
                build.mkdir(parents=True)
                for file in ("CMakeCache.txt", "compile_commands.json"):
                    (build / file).write_text("fixture")
                with patch.object(check, "run") as run:
                    check.build(sanitizer=sanitizer, install=False)
                tests = next(call for call in run.call_args_list if call.args[0] == f"{name}-tests")
                self.assertEqual(tests.args, (f"{name}-tests", "ctest", "--test-dir", build,
                                              "--output-on-failure", "--no-tests=error",
                                              "--output-junit", check.reports / f"{name}-tests.xml"))
                if sanitizer:
                    self.assertEqual(tests.kwargs["extra_env"], {
                        "ASAN_OPTIONS": "detect_leaks=1:halt_on_error=1",
                        "UBSAN_OPTIONS": "halt_on_error=1:print_stacktrace=1",
                    })
                else:
                    self.assertEqual(tests.kwargs["extra_env"], {})


if __name__ == "__main__":
    unittest.main()
