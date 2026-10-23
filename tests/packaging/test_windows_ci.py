"""Changing build generators must preserve full tests and the Release layout."""

import importlib.util
from contextlib import redirect_stderr
import io
import os
from pathlib import Path
import subprocess
import sys
import tempfile
from types import SimpleNamespace
import unittest
from unittest.mock import patch

ROOT = Path(__file__).resolve().parents[2]
spec = importlib.util.spec_from_file_location("windows_ci", ROOT / "tools/ci/check_windows.py")
windows = importlib.util.module_from_spec(spec)
with patch.object(sys, "path", [str(ROOT / "tools/ci"), *sys.path]):
    spec.loader.exec_module(windows)


class WindowsGeneratorTests(unittest.TestCase):
    def test_split_native_job_retains_tests_and_propagates_failure(self):
        for exit_code in (0, 1):
            with self.subTest(exit_code=exit_code), tempfile.TemporaryDirectory() as directory:
                root = Path(directory)
                prefix = root / "dependencies"
                prefix.mkdir()
                args = ["check_windows.py", "--dependency-prefix", str(prefix), "--skip-python",
                        "--work", str(root / "work"), "--reports", str(root / "reports"),
                        "--version", "0.3.1", "--generator", "Ninja Multi-Config"]
                platform = SimpleNamespace(name="nt", environ=dict(os.environ),
                                           path=os.path, pathsep=os.pathsep)
                results = [subprocess.CompletedProcess([], code) for code in (0, 0, exit_code)]
                with patch.object(windows, "os", platform), patch.object(sys, "argv", args), \
                     patch.object(windows, "check"), \
                     patch.object(windows.subprocess, "run", side_effect=results) as run:
                    if exit_code:
                        with self.assertRaises(subprocess.CalledProcessError):
                            windows.main()
                    else:
                        windows.main()
                self.assertEqual(len(run.call_args_list), 3)
                tests = run.call_args_list[-1].args[0]
                self.assertEqual(tests, ["ctest", "--test-dir", str(root / "work/desktop"),
                    "-C", "Release", "--output-on-failure", "--no-tests=error",
                    "--output-junit", str(root / "reports/desktop-tests.xml")])
                self.assertEqual((root / "reports/passed.json").exists(), exit_code == 0)

    def test_release_rejects_skipping_python_before_creating_work(self):
        with tempfile.TemporaryDirectory() as directory:
            root = Path(directory)
            args = ["check_windows.py", "--dependency-prefix", str(root / "dependencies"),
                    "--work", str(root / "work"), "--reports", str(root / "reports"),
                    "--version", "0.3.1", "--release", "--output", str(root / "output"),
                    "--skip-python"]
            with patch.object(sys, "argv", args), patch.object(windows.subprocess, "run") as run, \
                 redirect_stderr(io.StringIO()):
                with self.assertRaises(SystemExit) as error:
                    windows.main()
                self.assertEqual(error.exception.code, 2)
                run.assert_not_called()
            self.assertFalse((root / "work").exists())

    def test_release_stages_full_portable_and_all_update_assets(self):
        with tempfile.TemporaryDirectory() as directory:
            root = Path(directory)
            output = root / "release"
            output.mkdir()
            expected = {f"Orbelle-0.3.2-win-x64{suffix}" for suffix in
                        (".zip", "-app.zip", "-runtime.zip", "-resources.zip", "-update.json")}
            for name in expected:
                (root / name).write_bytes(name.encode("ascii"))
            windows.copy_desktop_assets(root, output, "0.3.2")
            self.assertEqual({path.name for path in output.iterdir()}, expected)
            for path in output.iterdir():
                self.assertEqual(path.read_bytes(), path.name.encode("ascii"))

    def test_cached_ninja_and_visual_studio_keep_full_tests_and_python_parity(self):
        for generator in ("Ninja Multi-Config", "Visual Studio 17 2022"):
            with self.subTest(generator=generator), tempfile.TemporaryDirectory() as directory:
                root = Path(directory)
                prefix = root / "dependencies"
                prefix.mkdir()
                environment = dict(os.environ, CMAKE_GENERATOR=generator,
                                   CMAKE_GENERATOR_PLATFORM="x64",
                                   CMAKE_CXX_COMPILER_LAUNCHER="sccache")
                platform = SimpleNamespace(name="nt", environ=environment, path=os.path, pathsep=os.pathsep)
                args = ["check_windows.py", "--dependency-prefix", str(prefix),
                        "--work", str(root / "work"), "--reports", str(root / "reports"),
                        "--version", "0.2.1"]
                with patch.object(windows, "os", platform), patch.object(sys, "argv", args), \
                     patch.object(windows, "check"), \
                     patch.object(windows.subprocess, "run", return_value=subprocess.CompletedProcess([], 0)) as run:
                    windows.main()
                commands = [call.args[0] for call in run.call_args_list]
                configure, build, tests, python = commands
                self.assertIn(generator, configure)
                # Compiler probes can use Debug; Release must not gain debug symbols.
                self.assertIn("-DCMAKE_MSVC_DEBUG_INFORMATION_FORMAT=$<$<CONFIG:Debug,RelWithDebInfo>:Embedded>", configure)
                self.assertEqual("-A" in configure, generator.startswith("Visual Studio "))
                env = run.call_args_list[0].kwargs["env"]
                self.assertEqual(env["CMAKE_CXX_COMPILER_LAUNCHER"], "sccache")
                self.assertEqual("CMAKE_GENERATOR_PLATFORM" in env, generator.startswith("Visual Studio "))
                self.assertEqual(build[-2:], ["--config", "Release"])
                self.assertEqual(tests, ["ctest", "--test-dir", str(root / "work/desktop"),
                    "-C", "Release", "--output-on-failure", "--no-tests=error",
                    "--output-junit", str(root / "reports/desktop-tests.xml")])
                self.assertIn("--cpp-example", python)
                self.assertEqual(Path(python[-1]), root / "work/desktop/examples/cpp/Release/orbelle_example.exe")

    def test_test_failure_stops_before_python_and_cannot_report_success(self):
        with tempfile.TemporaryDirectory() as directory:
            root = Path(directory)
            prefix = root / "dependencies"
            prefix.mkdir()
            args = ["check_windows.py", "--dependency-prefix", str(prefix),
                    "--work", str(root / "work"), "--reports", str(root / "reports"),
                    "--version", "0.2.1", "--generator", "Ninja Multi-Config"]
            platform = SimpleNamespace(name="nt", environ=dict(os.environ), path=os.path, pathsep=os.pathsep)
            results = [subprocess.CompletedProcess([], code) for code in (0, 0, 1)]
            with patch.object(windows, "os", platform), patch.object(sys, "argv", args), \
                 patch.object(windows, "check"), \
                 patch.object(windows.subprocess, "run", side_effect=results):
                with self.assertRaises(subprocess.CalledProcessError):
                    windows.main()
            self.assertFalse((root / "reports/passed.json").exists())


if __name__ == "__main__":
    unittest.main()
