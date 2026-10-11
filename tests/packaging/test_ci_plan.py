"""Documentation and cache-only runs must never conceal a failed required job."""

import importlib.util
from pathlib import Path
import subprocess
import tempfile
import unittest
from unittest.mock import patch


ROOT = Path(__file__).resolve().parents[2]
spec = importlib.util.spec_from_file_location("ci_plan", ROOT / "tools/ci/plan_ci.py")
plan = importlib.util.module_from_spec(spec)
spec.loader.exec_module(plan)


class ScopeTests(unittest.TestCase):
    def test_only_explicit_documentation_paths_skip_native_builds(self):
        with patch.object(plan, "pull_request_paths", return_value=[
            "README.md", "AGENTS.md", "docs/development/requirements.md",
        ]):
            self.assertEqual(plan.select_scope("pull_request", "refs/pull/3/merge"), "docs")
        for path in ("modules/core/orbit.cpp", ".github/workflows/ci.yml", "vcpkg.json",
                     "tests/fixtures/README.md", "docs/example.py", "data/input.json"):
            with self.subTest(path=path), patch.object(plan, "pull_request_paths",
                                                     return_value=["README.md", path]):
                self.assertEqual(plan.select_scope("pull_request", "refs/pull/3/merge"), "full")

    def test_unavailable_or_empty_diff_keeps_full_checks(self):
        with patch.object(plan, "pull_request_paths", side_effect=ValueError("missing parent")):
            self.assertEqual(plan.select_scope("pull_request", "refs/pull/3/merge"), "full")
        with patch.object(plan, "pull_request_paths", return_value=[]):
            self.assertEqual(plan.select_scope("pull_request", "refs/pull/3/merge"), "full")

    def test_only_main_push_uses_cache_maintenance(self):
        self.assertEqual(plan.select_scope("push", "refs/heads/main"), "cache")
        for event, ref in (("workflow_dispatch", "refs/heads/main"),
                           ("workflow_call", "refs/heads/main"),
                           ("push", "refs/tags/v0.1.0"),
                           ("merge_group", "refs/heads/main"), ("", "")):
            with self.subTest(event=event, ref=ref):
                self.assertEqual(plan.select_scope(event, ref), "full")
        self.assertEqual(plan.select_scope("push", "refs/heads/main", release_tag="v0.1.0"), "full")


class RequiredChecksTests(unittest.TestCase):
    def results(self, scope):
        return {name: {"result": "success" if name in plan.REQUIRED[scope] else "skipped"}
                for name in plan.JOBS}

    def test_expected_checks_and_intentional_skips_pass(self):
        for scope in ("full", "docs", "cache"):
            with self.subTest(scope=scope):
                plan.check_results(scope, self.results(scope))

    def test_failure_cancellation_or_skip_cannot_pass_a_required_job(self):
        for scope in ("full", "docs", "cache"):
            for name in plan.REQUIRED[scope]:
                for result in ("failure", "cancelled", "skipped", ""):
                    with self.subTest(scope=scope, job=name, result=result):
                        results = self.results(scope)
                        results[name]["result"] = result
                        with self.assertRaises(ValueError):
                            plan.check_results(scope, results)

    def test_unknown_scope_or_missing_jobs_cannot_pass(self):
        with self.assertRaises(ValueError):
            plan.check_results("", self.results("docs"))
        results = self.results("full")
        del results["windows"]
        with self.assertRaises(ValueError):
            plan.check_results("full", results)

    def test_unplanned_jobs_are_not_silently_ignored(self):
        results = self.results("docs")
        results["windows"]["result"] = "failure"
        with self.assertRaises(ValueError):
            plan.check_results("docs", results)


class MergeDiffTests(unittest.TestCase):
    def setUp(self):
        self.directory = tempfile.TemporaryDirectory()
        self.addCleanup(self.directory.cleanup)
        self.root = Path(self.directory.name)
        self.git("init", "--quiet", "--initial-branch=main")
        self.git("config", "user.name", "CI test")
        self.git("config", "user.email", "ci@example.invalid")
        self.git("config", "commit.gpgsign", "false")
        (self.root / "README.md").write_text("Initial documentation\n")
        (self.root / "source.cpp").write_text("int main() {}\n")
        self.commit("Initial files")
        self.git("switch", "--quiet", "-c", "topic")

    def git(self, *args):
        return subprocess.check_output(["git", *args], cwd=self.root, stderr=subprocess.STDOUT)

    def commit(self, message):
        self.git("add", "--all")
        self.git("commit", "--quiet", "-m", message)

    def merge(self):
        self.git("switch", "--quiet", "main")
        self.git("merge", "--quiet", "--no-ff", "topic", "-m", "Synthetic PR merge")

    def test_merge_diff_sees_every_pr_commit(self):
        (self.root / "README.md").write_text("Updated documentation\n")
        self.commit("Update docs")
        (self.root / "source.cpp").write_text("int main() { return 1; }\n")
        self.commit("Change code")
        self.merge()
        self.assertEqual(set(plan.pull_request_paths(self.root)), {"README.md", "source.cpp"})
        self.assertEqual(plan.select_scope("pull_request", "refs/pull/3/merge", self.root), "full")

    def test_rename_into_docs_still_runs_full_checks(self):
        (self.root / "docs").mkdir()
        (self.root / "source.cpp").rename(self.root / "docs/source.md")
        self.commit("Move code into docs")
        self.merge()
        self.assertIn("source.cpp", plan.pull_request_paths(self.root))
        self.assertEqual(plan.select_scope("pull_request", "refs/pull/3/merge", self.root), "full")

    def test_documentation_merge_skips_build_but_plain_head_does_not(self):
        (self.root / "README.md").write_text("Updated documentation\n")
        self.commit("Update docs")
        self.assertEqual(plan.select_scope("pull_request", "refs/pull/3/merge", self.root), "full")
        self.merge()
        self.assertEqual(plan.select_scope("pull_request", "refs/pull/3/merge", self.root), "docs")


if __name__ == "__main__":
    unittest.main()
