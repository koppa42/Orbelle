"""Split Python jobs retain all API/example/parity checks without duplicate native builds."""

from contextlib import ExitStack, redirect_stderr
import importlib.util
import io
import json
import os
from pathlib import Path
import subprocess
import sys
import tempfile
from types import SimpleNamespace
import unittest
from unittest.mock import patch

ROOT = Path(__file__).resolve().parents[2]
spec = importlib.util.spec_from_file_location("python_ci", ROOT / "tools/ci/check_python.py")
python_ci = importlib.util.module_from_spec(spec)
with patch.object(sys, "path", [str(ROOT / "tools/ci"), *sys.path]):
    spec.loader.exec_module(python_ci)


class PythonCiTests(unittest.TestCase):
    def setUp(self):
        temporary = tempfile.TemporaryDirectory()
        self.addCleanup(temporary.cleanup)
        self.root = Path(temporary.name)
        self.source = self.root / "source"
        package = self.source / "bindings/python/src/orbelle"
        package.mkdir(parents=True)
        (package / "__init__.py").write_text("# isolated package fixture\n")
        (self.source / "pyproject.toml").write_bytes((ROOT / "pyproject.toml").read_bytes())
        self.prefix = self.root / "dependencies"
        self.prefix.mkdir()
        self.work = self.root / "work"
        self.reports = self.root / "reports"
        self.requests = []

    def run_check(self, options=(), *, platform="posix", generator="Ninja", fail=None,
                  example_output="one"):
        environment = dict(os.environ, CMAKE_GENERATOR=generator, PYTHONPATH="untrusted-python",
                           PYTHONHOME="untrusted-home", LD_LIBRARY_PATH="untrusted-libraries",
                           CMAKE_CXX_COMPILER_LAUNCHER="sccache")
        system = SimpleNamespace(name=platform, environ=environment)
        argv = ["check_python.py", "--dependency-prefix", str(self.prefix),
                "--work", str(self.work), "--reports", str(self.reports), *options]
        filename = "orbelle_example.exe" if platform == "nt" else "orbelle_example"
        base = self.work / "build/examples/cpp"
        multi = "Multi-Config" in generator or generator.startswith("Visual Studio ")
        expected_example = (base / "Release" if multi else base) / filename

        def run(command, **kwargs):
            self.requests.append((command, kwargs["env"]))
            if command[:2] == ["cmake", "--build"] and "orbelle_example" in command:
                if example_output != "missing":
                    expected_example.parent.mkdir(parents=True, exist_ok=True)
                    expected_example.write_bytes(b"compiled example")
                if example_output == "ambiguous":
                    alternate = (base if multi else base / "Release") / filename
                    alternate.parent.mkdir(parents=True, exist_ok=True)
                    alternate.write_bytes(b"unexpected duplicate")
            failed = (fail == "build" and command[:2] == ["cmake", "--build"] or
                      fail == "parity" and any(item.endswith("compare_examples.py") for item in command))
            return subprocess.CompletedProcess(command, 1 if failed else 0)

        def output(command, **kwargs):
            if "--cmakedir" in command:
                return str(self.root / "pybind11")
            if "sysconfig.get_path('purelib')" in command[-1]:
                return str(self.work / "consumer/site-packages")
            self.fail(f"Unexpected subprocess query: {command}")

        with ExitStack() as stack:
            stack.enter_context(patch.object(sys, "argv", argv))
            stack.enter_context(patch.object(python_ci, "os", system))
            stack.enter_context(patch.object(python_ci, "ROOT", self.source))
            stack.enter_context(patch.object(python_ci, "check", return_value="0.3.1"))
            stack.enter_context(patch.object(python_ci, "stage_validation_inputs"))
            stack.enter_context(patch.object(python_ci.subprocess, "run", side_effect=run))
            stack.enter_context(patch.object(python_ci.subprocess, "check_output", side_effect=output))
            python_ci.main()
        return expected_example

    def test_default_still_builds_only_extension_and_runs_every_api_and_example(self):
        self.run_check()
        commands = [command for command, _ in self.requests]
        build = next(command for command in commands if command[:2] == ["cmake", "--build"])
        self.assertEqual(build[-2:], ["--target", "_native"])
        configure = next(command for command in commands if command[:2] == ["cmake", "-S"])
        self.assertIn("-DORBELLE_BUILD_EXAMPLES=OFF", configure)
        records = json.loads((self.reports / "commands.json").read_text())
        names = {record["name"] for record in records}
        self.assertIn("python-tests", names)
        self.assertTrue({"example-" + name for name in python_ci.PYTHON_EXAMPLES} <= names)
        self.assertNotIn("cpp-python-parity", names)
        self.assertTrue((self.reports / "passed.json").exists())

    def test_built_example_uses_same_build_and_is_compared_from_clean_consumer(self):
        for platform, generator in (("posix", "Ninja"), ("nt", "Ninja Multi-Config"),
                                    ("nt", "Visual Studio 17 2022")):
            with self.subTest(platform=platform, generator=generator):
                self.work = self.root / f"work-{platform}-{generator.replace(' ', '-')}"
                self.reports = self.root / f"reports-{platform}-{generator.replace(' ', '-')}"
                self.requests.clear()
                example = self.run_check(["--build-cpp-example"], platform=platform, generator=generator)
                commands = [command for command, _ in self.requests]
                builds = [command for command in commands if command[:2] == ["cmake", "--build"]]
                self.assertEqual(len(builds), 1)
                self.assertEqual(builds[0][-3:], ["--target", "_native", "orbelle_example"])
                configure = next(command for command in commands if command[:2] == ["cmake", "-S"])
                self.assertIn("-DORBELLE_BUILD_EXAMPLES=ON", configure)
                self.assertNotIn("-DORBELLE_BUILD_EXAMPLES=OFF", configure)
                parity, env = self.requests[-1]
                self.assertEqual(Path(parity[-1]), example)
                self.assertIn("-I", parity)
                self.assertIn("consumer", Path(parity[0]).parts)
                self.assertTrue(parity[-3].endswith("compare_examples.py"))
                for key in ("PYTHONPATH", "PYTHONHOME", "LD_LIBRARY_PATH", "CMAKE_PREFIX_PATH"):
                    self.assertNotIn(key, env)
                records = json.loads((self.reports / "commands.json").read_text())
                names = {record["name"] for record in records}
                self.assertTrue({"python-tests", "cpp-python-parity"} <= names)
                self.assertTrue({"example-" + name for name in python_ci.PYTHON_EXAMPLES} <= names)

    def test_existing_external_cpp_example_is_still_supported(self):
        example = self.root / "external-example"
        example.write_bytes(b"external C++ example")
        self.run_check(["--cpp-example", str(example)])
        self.assertEqual(Path(self.requests[-1][0][-1]), example)
        build = next(command for command, _ in self.requests if command[:2] == ["cmake", "--build"])
        self.assertEqual(build[-2:], ["--target", "_native"])

    def test_invalid_argument_combinations_fail_before_work_or_commands(self):
        for options in (["--build-cpp-example", "--cpp-example", "existing"],
                        ["--build-cpp-example", "--release", "--output", str(self.root / "assets")]):
            with self.subTest(options=options), redirect_stderr(io.StringIO()):
                with self.assertRaises(SystemExit) as error:
                    self.run_check(options)
                self.assertEqual(error.exception.code, 2)
                self.assertFalse(self.work.exists())
                self.assertEqual(self.requests, [])

    def test_build_and_parity_failures_never_report_success(self):
        for failure in ("build", "parity"):
            with self.subTest(failure=failure):
                self.work = self.root / ("work-" + failure)
                self.reports = self.root / ("reports-" + failure)
                self.requests.clear()
                with self.assertRaises(subprocess.CalledProcessError):
                    self.run_check(["--build-cpp-example"], fail=failure)
                self.assertFalse((self.reports / "passed.json").exists())
                records = json.loads((self.reports / "commands.json").read_text())
                self.assertEqual(records[-1]["exit_code"], 1)
                self.assertEqual(records[-1]["name"],
                                 "python-build" if failure == "build" else "cpp-python-parity")

    def test_missing_or_ambiguous_example_stops_before_consumer_tests(self):
        for outcome in ("missing", "ambiguous"):
            with self.subTest(outcome=outcome):
                self.work = self.root / ("work-" + outcome)
                self.reports = self.root / ("reports-" + outcome)
                self.requests.clear()
                with self.assertRaisesRegex(RuntimeError, "exactly one freshly built C\\+\\+ example"):
                    self.run_check(["--build-cpp-example"], example_output=outcome)
                self.assertFalse((self.reports / "passed.json").exists())
                records = json.loads((self.reports / "commands.json").read_text())
                self.assertEqual(records[-1]["name"], "python-build")


if __name__ == "__main__":
    unittest.main()
