"""Keep numerical coverage intact while separating instrumented test costs."""

import argparse
import importlib.util
import json
from pathlib import Path
import re
import shutil
import subprocess
import sys
import tempfile
import unittest
from unittest.mock import patch


ROOT = Path(__file__).resolve().parents[2]


def load_tool(name):
    spec = importlib.util.spec_from_file_location(name, ROOT / f"tools/ci/{name}.py")
    module = importlib.util.module_from_spec(spec)
    with patch.object(sys, "path", [str(ROOT / "tools/ci"), *sys.path]):
        spec.loader.exec_module(module)
    return module


selector = load_tool("select_sanitizers")
linux = load_tool("check_linux")

# These existing large-allocation, polar, parser and derivative checks must
# remain in the daily tier even when long reference sweeps are deferred.
BOUNDARIES = [
    "core.Gravity.FullDegreeZonalFieldAtPoleMatchesClosedForm",
    "core.GravityDifferential.FullDegreeExactPolesMatchHighPrecisionLegendreIdentities",
    "core.GravityDifferential.CorrectionsExtendBaseFieldAndCanBeCancelledWithoutMutation",
    "core.GravityDifferential.ConcurrentEpochsDoNotShareMutableDerivativeState",
]


class SelectionTests(unittest.TestCase):
    def names(self):
        return [*selector.FULL_ONLY, *BOUNDARIES, "integration.FutureParser.NewBoundary"]

    def test_new_cases_and_high_degree_boundaries_run_by_default(self):
        names = self.names()
        routine = selector.select_tests(names, "routine")
        self.assertTrue(set(BOUNDARIES) <= set(routine["selected"]))
        self.assertIn("integration.FutureParser.NewBoundary", routine["selected"])
        self.assertEqual(set(routine["selected"]) | set(routine["deferred"]), set(names))
        self.assertFalse(set(routine["selected"]) & set(routine["deferred"]))
        self.assertTrue(all(routine["deferred"].values()))

    def test_full_profile_has_every_case_and_no_ctest_filter(self):
        full = selector.select_tests(self.names(), "full")
        self.assertEqual(full["selected"], self.names())
        self.assertEqual(full["selected_count"], len(self.names()))
        self.assertEqual(full["ctest_args"], [])
        self.assertEqual(full["deferred"], {})

    def test_stale_empty_duplicate_or_invalid_inventory_fails_closed(self):
        for names in ([], self.names()[1:], [*self.names(), self.names()[0]],
                      [*self.names(), ""], [*self.names(), None]):
            for profile in ("routine", "full"):
                with self.subTest(names=names[-1:] if names else [], profile=profile), \
                     self.assertRaises(ValueError):
                    selector.select_tests(names, profile)
        with self.assertRaises(ValueError):
            selector.select_tests(self.names(), "skip")

    def test_exclusions_cannot_match_a_whole_suite_or_nearby_parameter(self):
        pattern = selector.select_tests(self.names(), "routine")["ctest_args"][1]
        for name in selector.FULL_ONLY:
            self.assertIsNotNone(re.search(pattern, name))
            for other in (name + "0", name + "Extra", "prefix" + name, name.replace(".", "X")):
                self.assertIsNone(re.search(pattern, other), other)

    @unittest.skipUnless(shutil.which("ctest") and shutil.which("cmake"), "CTest is required")
    def test_real_ctest_applies_exact_selection_and_returns_failures(self):
        with tempfile.TemporaryDirectory() as directory:
            root = Path(directory)
            # Use a tiny synthetic CTest project to exercise CTest's regex engine,
            # JSON discovery and JUnit, without depending on a native build.
            names = [*self.names(), next(iter(selector.FULL_ONLY)) + "Extra"]
            cmake = Path(shutil.which("cmake")).as_posix()
            lines = [f'add_test("{name}" "{cmake}" -E true)' for name in names]
            (root / "CTestTestfile.cmake").write_text("\n".join(lines) + "\n", encoding="utf-8")
            for profile in ("routine", "full"):
                output = root / f"{profile}.json"
                subprocess.run([sys.executable, str(ROOT / "tools/ci/select_sanitizers.py"),
                                "--build", str(root), "--profile", profile, "--output", str(output)],
                               check=True, capture_output=True, text=True)
                selection = json.loads(output.read_text())
                command = ["ctest", "--test-dir", str(root), *selection["ctest_args"]]
                inventory = json.loads(subprocess.check_output([*command, "--show-only=json-v1"], text=True))
                self.assertEqual([test["name"] for test in inventory["tests"]], selection["selected"])
                result = subprocess.run([*command, "--no-tests=error", "--output-junit",
                                         str(root / f"{profile}.xml")], capture_output=True, text=True)
                self.assertEqual(result.returncode, 0, result.stdout + result.stderr)
            # A newly added failure remains selected and cannot become a green run.
            with (root / "CTestTestfile.cmake").open("a") as stream:
                stream.write(f'add_test("integration.NewFailure" "{cmake}" -E false)\n')
            args = selector.select_tests(names, "routine")["ctest_args"]
            result = subprocess.run(["ctest", "--test-dir", str(root), *args, "--output-on-failure"],
                                    capture_output=True, text=True)
            self.assertNotEqual(result.returncode, 0)
            self.assertIn("integration.NewFailure", result.stdout)


class LinuxStagesTests(unittest.TestCase):
    def test_delivery_and_native_stages_do_not_repeat_the_separate_sanitizer_job(self):
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

    def test_release_never_gets_the_routine_test_filter(self):
        for sanitizer, profile in ((False, "routine"), (True, "routine"), (True, "full")):
            with self.subTest(sanitizer=sanitizer, profile=profile), tempfile.TemporaryDirectory() as directory:
                args = argparse.Namespace(source=ROOT, work=Path(directory) / "work",
                                          reports=Path(directory) / "reports", delivery=None, jobs=1)
                check = linux.Check(args)
                name = "sanitizers" if sanitizer else "release"
                build = check.work / "build" / name
                build.mkdir(parents=True)
                for file in ("CMakeCache.txt", "compile_commands.json"):
                    (build / file).write_text("fixture")
                def run(command, *args, **kwargs):
                    if command == "sanitizers-selection":
                        selected = selector.select_tests([*selector.FULL_ONLY, *BOUNDARIES], profile)
                        (check.reports / "sanitizers-selection.json").write_text(json.dumps(selected))
                with patch.object(check, "run", side_effect=run) as run_mock:
                    check.build(sanitizer=sanitizer, install=False, sanitizer_profile=profile)
                calls = [call.args for call in run_mock.call_args_list]
                tests = next(call for call in calls if call[0] == f"{name}-tests")
                self.assertIn("--no-tests=error", tests)
                self.assertEqual("-E" in tests, sanitizer and profile == "routine")
                self.assertEqual(any(call[0] == "sanitizers-selection" for call in calls), sanitizer)


if __name__ == "__main__":
    unittest.main()
