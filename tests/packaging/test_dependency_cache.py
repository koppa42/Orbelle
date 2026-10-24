"""Dependency snapshots survive interrupted installs without weakening ABI checks."""

from contextlib import redirect_stdout
import hashlib
import importlib.util
import io
import json
import os
from pathlib import Path
import sys
import tempfile
import unittest
from unittest.mock import patch
import zipfile


ROOT = Path(__file__).resolve().parents[2]
spec = importlib.util.spec_from_file_location("dependency_cache", ROOT / "tools/ci/dependency_cache.py")
cache = importlib.util.module_from_spec(spec)
spec.loader.exec_module(cache)


class DependencyCacheTests(unittest.TestCase):
    def setUp(self):
        self.temporary = tempfile.TemporaryDirectory()
        self.addCleanup(self.temporary.cleanup)
        self.root = Path(self.temporary.name)
        self.source = self.root / "source"
        self.source.mkdir()
        self.manifest = {"name": "app", "version-semver": "0.4.0", "builtin-baseline": "baseline",
                         "dependencies": [{"name": "qtbase", "version>=": "6.11.0"}],
                         "features": {"graphics": {"dependencies": ["vsgqt"]}}}
        self.write_manifest()
        (self.source / "vcpkg-configuration.json").write_text('{"overlay-ports":["cmake/ports"]}')
        self.port = self.source / "cmake/ports/test/portfile.cmake"
        self.port.parent.mkdir(parents=True)
        self.port.write_text("original port\n")
        self.triplet = self.source / "cmake/triplets/ci/x64-windows-release.cmake"
        self.triplet.parent.mkdir(parents=True)
        self.triplet.write_text("set(VCPKG_PLATFORM_TOOLSET v143)\n")
        self.binaries = self.root / "binaries"
        self.state = self.root / "report"
        self.installed = self.root / "installed"
        self.buildtrees = self.root / "buildtrees"

    def write_manifest(self):
        (self.source / "vcpkg.json").write_text(json.dumps(self.manifest), encoding="utf-8")

    def identity(self, profile="windows", run="42"):
        return cache.cache_identity(self.source, profile, "20261012.333.1", run, "1")

    def archive(self, *, port="qtbase", tool="7.6.4"):
        data = (f"features core;widgets\nportfile.cmake {port}\npowershell {tool}\n"
                "triplet x64-windows-release\n").encode()
        abi = hashlib.sha256(data).hexdigest()
        path = self.binaries / abi[:2] / f"{abi}.zip"
        path.parent.mkdir(parents=True, exist_ok=True)
        with zipfile.ZipFile(path, "w") as archive:
            archive.writestr(f"share/{port}/vcpkg_abi_info.txt", data)
        return path, data

    def metadata(self, port, data, *, planned=False):
        path = (self.buildtrees / port / "x64-windows-release.vcpkg_abi_info.txt" if planned else
                self.installed / "x64-windows-release/share" / port / "vcpkg_abi_info.txt")
        path.parent.mkdir(parents=True, exist_ok=True)
        path.write_bytes(data)

    def invoke(self, *args, env=None):
        with patch.object(sys, "argv", ["dependency_cache.py", *map(str, args)]), \
                patch.dict(os.environ, env or {}, clear=True), redirect_stdout(io.StringIO()):
            cache.main()

    def test_application_version_and_json_format_do_not_invalidate_dependencies(self):
        initial = cache.fingerprint(self.source)
        self.manifest["version-semver"] = "0.5.0"
        self.write_manifest()
        self.assertEqual(initial, cache.fingerprint(self.source))
        (self.source / "vcpkg.json").write_text(json.dumps(self.manifest, indent=4, sort_keys=True))
        (self.source / "vcpkg-configuration.json").write_text('{\n "overlay-ports": ["cmake/ports"]\n}')
        self.assertEqual(initial, cache.fingerprint(self.source))

    def test_versions_of_dependencies_and_features_still_invalidate(self):
        initial = cache.fingerprint(self.source)
        for changed in (
                {"builtin-baseline": "new-baseline"},
                {"dependencies": [{"name": "qtbase", "version>=": "6.12.0"}]},
                {"features": {"graphics": {"dependencies": ["vsgqt", "rocky"]}}},
                {"overrides": [{"name": "qtbase", "version": "6.11.1"}]}):
            original = dict(self.manifest)
            with self.subTest(changed=changed):
                self.manifest.update(changed)
                self.write_manifest()
                self.assertNotEqual(initial, cache.fingerprint(self.source))
            self.manifest = original
            self.write_manifest()

    def test_overlay_triplet_registry_and_deleted_files_invalidate(self):
        initial = cache.fingerprint(self.source)
        for path in (self.port, self.triplet, self.source / "vcpkg-configuration.json"):
            original = path.read_bytes()
            with self.subTest(path=path):
                path.write_text('{"overlay-ports":[]}' if path.suffix == ".json" else "changed\n")
                self.assertNotEqual(initial, cache.fingerprint(self.source))
            path.write_bytes(original)
        self.port.unlink()
        self.assertNotEqual(initial, cache.fingerprint(self.source))

    def test_current_profile_precedes_legacy_same_image_fallbacks(self):
        for profile in cache.FAMILIES:
            identity = self.identity(profile)
            keys = identity["restore_keys"]
            with self.subTest(profile=profile):
                self.assertEqual(keys[0], identity["prefix"])
                self.assertEqual(keys[1], cache.FAMILIES[profile][0])
                legacy = cache.FAMILIES[profile][1] + identity["image"] + "-"
                self.assertIn(legacy, keys[2:])

    def test_python_never_restores_a_native_snapshot(self):
        for platform in ("windows", "linux"):
            keys = self.identity(f"python-{platform}")["restore_keys"]
            self.assertTrue(all(key.startswith(f"python-deps-{platform}-") for key in keys))

    def test_runner_image_change_does_not_duplicate_a_pinned_tools_snapshot(self):
        first = self.identity()
        other = cache.cache_identity(self.source, "windows", "20261022.344.1", "43", "1")
        self.assertEqual(first["prefix"], other["prefix"])
        self.assertNotEqual(first["image"], other["image"])
        self.assertIn(first["prefix"], other["restore_keys"])

    def test_new_snapshot_has_unique_key_but_reuses_same_configuration_prefix(self):
        first, second = self.identity(run="42"), self.identity(run="43")
        self.assertEqual(first["prefix"], second["prefix"])
        self.assertNotEqual(first["key"], second["key"])
        self.assertTrue(first["key"].startswith(second["restore_keys"][0]))

    def test_empty_or_multiline_runner_identity_is_rejected(self):
        for image in ("", "image\nother=value"):
            with self.subTest(image=image), self.assertRaises(ValueError):
                cache.cache_identity(self.source, "windows", image, "42", "1")

    def test_literal_cache_path_is_preserved_for_actions_version_compatibility(self):
        literal = "D:\\a\\_temp/vcpkg-binary-cache"
        self.invoke("identify", "--source", self.source, "--profile", "windows", "--cache", literal,
                    "--state", self.state,
                    env={"ImageVersion": "20261012.333.1", "GITHUB_RUN_ID": "42", "GITHUB_RUN_ATTEMPT": "1"})
        state = json.loads((self.state / "identity.json").read_text())
        self.assertEqual(state["cache"], literal)

    def test_inventory_ignores_incomplete_non_package_and_empty_files(self):
        valid, _ = self.archive()
        (self.binaries / ("a" * 64 + ".zip")).write_bytes(b"interrupted upload")
        (self.binaries / ("b" * 64 + ".zip")).touch()
        (self.binaries / "temporary.zip").write_bytes(valid.read_bytes())
        (self.binaries / ("c" * 64 + ".zip.tmp")).write_bytes(valid.read_bytes())
        self.assertEqual(list(cache.inventory(self.binaries)), [valid.relative_to(self.binaries).as_posix()])

    def test_unchanged_snapshot_is_not_uploaded_again(self):
        self.archive()
        packages = cache.inventory(self.binaries)
        identity = self.identity()
        self.assertFalse(cache.save_needed(packages, packages, identity["prefix"], identity["key"]))

    def test_rebuild_after_a_matching_restore_extends_the_immutable_snapshot(self):
        self.archive()
        before = cache.inventory(self.binaries)
        self.archive(tool="7.6.5")
        identity = self.identity()
        self.assertTrue(cache.save_needed(before, cache.inventory(self.binaries),
                                         identity["prefix"], identity["key"]))

    def test_legacy_or_other_image_cache_is_promoted_once_and_empty_cache_is_not_saved(self):
        self.archive()
        packages = cache.inventory(self.binaries)
        identity = self.identity()
        self.assertTrue(cache.save_needed(packages, packages, identity["prefix"], "legacy-key"))
        self.assertFalse(cache.save_needed({}, {}, identity["prefix"], ""))

    def test_diagnostics_identify_the_changed_tool_and_install_time(self):
        self.archive()
        previous = cache.cached_abis(self.binaries, cache.inventory(self.binaries))
        _, data = self.archive(tool="7.6.5")
        installed = self.root / "installed"
        path = installed / "x64-windows-release/share/qtbase/vcpkg_abi_info.txt"
        path.parent.mkdir(parents=True)
        path.write_bytes(data)
        log = ("Building qtbase[core,widgets]:x64-windows-release@6.11.0#1...\n"
               "Elapsed time to handle qtbase:x64-windows-release: 22 min\n")
        row, = cache.package_report(installed, previous, log)
        self.assertFalse(row["available_before"])
        self.assertTrue(row["rebuilt"])
        self.assertEqual(row["duration"], "22 min")
        self.assertEqual(row["changed_inputs"], {"powershell": {"before": "7.6.4", "after": "7.6.5"}})
        all_cached = cache.cached_abis(self.binaries, cache.inventory(self.binaries))
        row, = cache.package_report(installed, all_cached, "")
        self.assertTrue(row["available_before"])
        self.assertFalse(row["rebuilt"])
        self.assertEqual(row["changed_inputs"], {})

    def test_cancelled_install_can_save_completed_archive_without_installed_metadata(self):
        output = self.root / "output"
        env = {"ImageVersion": "20261012.333.1", "GITHUB_RUN_ID": "42", "GITHUB_RUN_ATTEMPT": "1",
               "GITHUB_OUTPUT": str(output)}
        self.invoke("identify", "--source", self.source, "--profile", "windows", "--cache", self.binaries,
                    "--state", self.state, env=env)
        self.invoke("begin", "--state", self.state)
        self.archive()
        self.invoke("finish", "--state", self.state, "--installed", self.root / "not-installed",
                    "--buildtrees", self.buildtrees,
                    "--log", self.root / "missing.log", env=env)
        self.assertIn("save<<CACHE_OUTPUT_END\ntrue\n", output.read_text())
        report = json.loads((self.state / "report.json").read_text())
        self.assertTrue(report["save"])
        self.assertEqual(report["after_count"], 1)
        self.assertEqual(report["packages"], [])

    def test_compaction_removes_desktop_packages_and_old_variants_from_legacy_python_cache(self):
        self.archive(port="qtbase")
        self.archive(port="sqlite3", tool="old")
        needed, data = self.archive(port="sqlite3", tool="pinned")
        original = cache.inventory(self.binaries)
        self.metadata("sqlite3", data)
        selected = cache.select_snapshot(original, original, self.installed, self.buildtrees)
        self.assertEqual(list(selected), [needed.relative_to(self.binaries).as_posix()])
        (self.binaries / "incomplete.zip.tmp").write_bytes(b"partial")
        cache.prune_snapshot(self.binaries, selected)
        self.assertEqual(cache.inventory(self.binaries), selected)
        self.assertEqual([path for path in self.binaries.rglob("*") if path.is_file()], [needed])
        identity = self.identity("python-windows")
        self.assertTrue(cache.save_needed(original, selected, identity["prefix"], identity["key"]))
        self.assertFalse(cache.save_needed(selected, selected, identity["prefix"], identity["key"]))

    def test_interrupted_install_keeps_planned_cached_and_new_completed_packages(self):
        needed, data = self.archive()
        self.metadata("qtbase", data, planned=True)
        self.archive(tool="obsolete")
        before = cache.inventory(self.binaries)
        completed, _ = self.archive(port="sqlite3")
        after = cache.inventory(self.binaries)
        selected = cache.select_snapshot(after, before, self.installed, self.buildtrees)
        self.assertEqual(set(selected), {path.relative_to(self.binaries).as_posix()
                                         for path in (needed, completed)})

    def test_early_failure_does_not_replace_existing_snapshot_with_empty_selection(self):
        self.archive()
        before = cache.inventory(self.binaries)
        selected = cache.select_snapshot(before, before, self.installed, self.buildtrees)
        self.assertEqual(selected, {})
        identity = self.identity()
        self.assertFalse(cache.save_needed(before, selected, identity["prefix"], "legacy-key"))
        self.assertEqual(cache.inventory(self.binaries), before)

    def test_pruning_rejects_links_before_removing_any_file(self):
        original, _ = self.archive()
        outside = self.root / "outside"
        outside.write_text("keep")
        try:
            (self.binaries / "linked").symlink_to(outside)
        except OSError:
            self.skipTest("Creating symlinks is not permitted on this host")
        with self.assertRaises(ValueError):
            cache.prune_snapshot(self.binaries, {})
        self.assertEqual(outside.read_text(), "keep")
        self.assertTrue(original.exists())

    def test_finish_reports_compact_snapshot_bytes_and_preserves_literal_save_path(self):
        wanted, data = self.archive(port="sqlite3")
        self.archive(port="qtbase")
        env = {"ImageVersion": "20261012.333.1", "GITHUB_RUN_ID": "42", "GITHUB_RUN_ATTEMPT": "1"}
        self.invoke("identify", "--source", self.source, "--profile", "python-windows",
                    "--cache", self.binaries, "--state", self.state, env=env)
        self.invoke("begin", "--state", self.state, env={"CACHE_MATCHED_KEY": "legacy-key"})
        self.metadata("sqlite3", data)
        output = self.root / "output"
        self.invoke("finish", "--state", self.state, "--installed", self.installed,
                    "--buildtrees", self.buildtrees, "--log", self.root / "missing.log",
                    env={"GITHUB_OUTPUT": str(output)})
        report = json.loads((self.state / "report.json").read_text())
        self.assertTrue(report["save"])
        self.assertEqual((report["before_count"], report["after_count"], report["retained_count"]), (2, 2, 1))
        self.assertEqual(report["retained_bytes"], wanted.stat().st_size)
        self.assertGreater(report["after_bytes"], report["retained_bytes"])
        self.assertIn(str(self.binaries), output.read_text())
        self.assertEqual(len(cache.inventory(self.binaries)), 1)

    def test_early_checkout_failure_has_nothing_to_save(self):
        output = self.root / "output"
        self.invoke("finish", "--state", self.state, "--installed", self.root / "missing",
                    "--buildtrees", self.buildtrees,
                    "--log", self.root / "missing.log", env={"GITHUB_OUTPUT": str(output)})
        self.assertIn("save<<CACHE_OUTPUT_END\nfalse\n", output.read_text())


if __name__ == "__main__":
    unittest.main()
