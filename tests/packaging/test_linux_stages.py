"""Regular CI preserves full numerical coverage; sanitizer runs stay explicit."""

import argparse
from contextlib import redirect_stderr
import importlib.util
import io
import json
import os
from pathlib import Path
import subprocess
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
    def test_dependency_build_pins_tools_and_does_not_pollute_project_compiler_cache(self):
        with tempfile.TemporaryDirectory() as directory:
            args = argparse.Namespace(source=ROOT, work=Path(directory) / "work",
                                      reports=Path(directory) / "reports", delivery=None, jobs=1,
                                      vcpkg_root=Path(directory) / "vcpkg")
            with patch.dict(os.environ, CMAKE_C_COMPILER_LAUNCHER="sccache",
                            CMAKE_CXX_COMPILER_LAUNCHER="sccache"):
                check = linux.Check(args)
            baseline = json.loads((ROOT / "vcpkg.json").read_text())["builtin-baseline"]
            with patch.object(linux.subprocess, "check_output", return_value=baseline), \
                    patch.object(check, "run") as run, patch.object(linux.shutil, "copy2"):
                check.dependencies()
            call = next(item for item in run.call_args_list if item.args[0] == "dependencies")
            self.assertIn("--x-abi-tools-use-exact-versions", call.args)
            for name in ("CMAKE_C_COMPILER_LAUNCHER", "CMAKE_CXX_COMPILER_LAUNCHER"):
                self.assertEqual(call.kwargs["extra_env"][name], "")
                self.assertEqual(check.env[name], "sccache")

    def test_regular_stages_and_explicit_sanitizer_stage_remain_separate(self):
        self.assertEqual(linux.stages_for("package"), ("dependencies", "release", "wheel", "export"))
        self.assertEqual(linux.stages_for("sanitizers"), ("dependencies", "sanitizers"))
        self.assertIn("sanitizers", linux.stages_for("all"))
        with tempfile.TemporaryDirectory() as directory:
            args = argparse.Namespace(source=ROOT, work=Path(directory) / "work",
                                      reports=Path(directory) / "reports", delivery=None, jobs=1,
                                      skip_python=False)
            check = linux.Check(args)
            with patch.object(check, "dependencies") as deps, \
                 patch.object(check, "build") as build, patch.object(check, "run") as run:
                check.tests()
                deps.assert_called_once()
                build.assert_called_once_with(install=False)
                self.assertEqual(run.call_args.args[0], "python-api")

    def test_split_native_job_still_builds_and_runs_the_complete_native_stage(self):
        with tempfile.TemporaryDirectory() as directory:
            args = argparse.Namespace(source=ROOT, work=Path(directory) / "work",
                                      reports=Path(directory) / "reports", delivery=None, jobs=1,
                                      skip_python=True)
            check = linux.Check(args)
            with patch.object(check, "dependencies") as deps, \
                 patch.object(check, "build") as build, patch.object(check, "run") as run:
                check.tests()
                deps.assert_called_once()
                build.assert_called_once_with(install=False)
                run.assert_not_called()
            with patch.object(check, "dependencies"), \
                 patch.object(check, "build", side_effect=subprocess.CalledProcessError(1, "ctest")), \
                 patch.object(check, "run") as run:
                with self.assertRaises(subprocess.CalledProcessError):
                    check.tests()
                run.assert_not_called()

    def test_skip_python_cannot_change_release_or_other_stage_coverage(self):
        for stage in ("all", "dependencies", "release", "sanitizers", "wheel", "export",
                      "consumer", "compute", "package"):
            with self.subTest(stage=stage), tempfile.TemporaryDirectory() as directory:
                root = Path(directory)
                argv = ["check_linux.py", "--stage", stage, "--skip-python", "--source", str(ROOT),
                        "--work", str(root / "work"), "--reports", str(root / "reports")]
                with patch.object(sys, "argv", argv), patch.object(linux, "Check") as check, \
                     redirect_stderr(io.StringIO()):
                    with self.assertRaises(SystemExit) as error:
                        linux.main()
                    self.assertEqual(error.exception.code, 2)
                    check.assert_not_called()
                self.assertFalse((root / "work").exists())

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
