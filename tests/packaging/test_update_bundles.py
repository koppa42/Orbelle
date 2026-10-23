"""Update archives are reproducible, complete, and safe to inspect without extraction."""
import hashlib
import json
import os
from pathlib import Path
import stat
import sys
import tempfile
import unittest
import zipfile

ROOT = Path(__file__).resolve().parents[2]
sys.path.insert(0, str(ROOT / "tools/packaging"))
import update_bundles as updates
sys.path.pop(0)


def portable_fixture(root, version):
    root.mkdir(parents=True)
    (root / "orbelle-layout.txt").write_bytes(b"1\n")
    (root / "active-version.txt").write_bytes((version + "\n").encode("ascii"))
    (root / "Orbelle.exe").write_bytes(b"stable launcher")
    (root / ".orbelle-running.lock").write_bytes(b"")
    directory = root / "versions" / version
    contents = {
        "OrbelleApp.exe": b"application", "OrbelleUpdater.exe": b"updater",
        "OrbelleLauncher.exe": b"launcher update",
        "qt.conf": b"[Paths]\nPlugins=plugins\n", "README.txt": b"instructions",
        "manifest.json": json.dumps({"schema": 1, "version": version}).encode("utf-8"),
        "Qt6Core.dll": b"runtime", "plugins/platforms/qwindows.dll": b"platform",
        "resources/earth/day.jpg": b"imagery", "notices/runtime/copyright": b"license",
    }
    for path, data in contents.items():
        target = directory / path
        target.parent.mkdir(parents=True, exist_ok=True)
        target.write_bytes(data)
    write_managed_files(directory)
    return directory


def write_managed_files(directory):
    lines = []
    for source in sorted(directory.rglob("*")):
        path = source.relative_to(directory).as_posix()
        if source.is_file() and path not in updates.MANAGEMENT_FILES:
            lines.append(f"{updates.digest(source)}  {path}\n")
    listing = "".join(lines).encode("utf-8")
    (directory / updates.MANAGED_LIST).write_bytes(listing)
    (directory / updates.MANAGED_MARKER).write_bytes(
        f"1\n{hashlib.sha256(listing).hexdigest()}\n".encode("ascii"))


class UpdateBundleTests(unittest.TestCase):
    def setUp(self):
        self.temporary = tempfile.TemporaryDirectory()
        self.addCleanup(self.temporary.cleanup)
        self.root = Path(self.temporary.name)
        self.version = "0.3.2"
        self.portable = self.root / "portable"
        self.files = portable_fixture(self.portable, self.version)
        self.assets = self.root / "assets"
        self.paths = updates.generate_update_bundles(self.portable, self.version, self.assets)
        self.manifest_path = self.paths[-1]
        self.manifest = json.loads(self.manifest_path.read_text(encoding="utf-8"))

    def save_manifest(self):
        self.manifest_path.write_text(json.dumps(self.manifest), encoding="utf-8")

    def rewrite_bundle(self, bundle, transform):
        record = next(item for item in self.manifest["bundles"] if item["id"] == bundle)
        path = self.assets / record["name"]
        with zipfile.ZipFile(path) as archive:
            entries = [(item, archive.read(item)) for item in archive.infolist()]
        entries = transform(entries)
        with zipfile.ZipFile(path, "w") as archive:
            for info, data in entries:
                archive.writestr(info, data)
        record.update(size=path.stat().st_size, sha256=updates.digest(path))
        self.save_manifest()

    def test_round_trip_keeps_complete_relative_inventory_and_three_groups(self):
        self.assertEqual(updates.validate_update_assets(self.assets, self.version), self.paths)
        files = {item["path"]: item for item in self.manifest["files"]}
        self.assertEqual(set(files), {path.relative_to(self.files).as_posix()
                                     for path in self.files.rglob("*") if path.is_file()})
        self.assertNotIn("Orbelle.exe", files)
        self.assertEqual(files["Qt6Core.dll"]["bundle"], "runtime")
        self.assertEqual(files["plugins/platforms/qwindows.dll"]["bundle"], "runtime")
        self.assertEqual(files["resources/earth/day.jpg"]["bundle"], "resources")
        self.assertEqual(files["notices/runtime/copyright"]["bundle"], "resources")
        for name in ("OrbelleApp.exe", "OrbelleUpdater.exe", "OrbelleLauncher.exe", "manifest.json",
                     *updates.MANAGEMENT_FILES):
            self.assertEqual(files[name]["bundle"], "app")

    def test_reproducible_archives_ignore_source_timestamps(self):
        for path in self.files.rglob("*"):
            if path.is_file():
                os.utime(path, (1700000000, 1700000000))
        repeated = updates.generate_update_bundles(self.portable, self.version, self.root / "again")
        self.assertEqual([updates.digest(path) for path in self.paths],
                         [updates.digest(path) for path in repeated])
        for path in repeated[:-1]:
            with zipfile.ZipFile(path) as archive:
                self.assertTrue(all(item.date_time == updates.FIXED_ZIP_TIME for item in archive.infolist()))

    def test_new_application_release_reuses_unchanged_runtime_and_resources_bytes(self):
        newer = self.root / "new-portable"
        new_files = portable_fixture(newer, "0.3.3")
        (new_files / "OrbelleApp.exe").write_bytes(b"new application")
        write_managed_files(new_files)
        generated = updates.generate_update_bundles(newer, "0.3.3", self.root / "new-assets")
        self.assertNotEqual(updates.digest(self.paths[0]), updates.digest(generated[0]))
        self.assertEqual([updates.digest(path) for path in self.paths[1:3]],
                         [updates.digest(path) for path in generated[1:3]])

    def test_refuses_overwrite_and_mismatched_active_version(self):
        with self.assertRaises(FileExistsError):
            updates.generate_update_bundles(self.portable, self.version, self.assets)
        (self.portable / "active-version.txt").write_bytes(b"0.3.1\n")
        with self.assertRaisesRegex(ValueError, "active version"):
            updates.generate_update_bundles(self.portable, self.version, self.root / "other")

    def test_source_inventory_must_match_native_management_list(self):
        (self.files / "forgotten.dll").write_bytes(b"unmanaged runtime")
        with self.assertRaisesRegex(ValueError, "complete update file inventory"):
            updates.generate_update_bundles(self.portable, self.version, self.root / "other")

    def test_source_portable_requires_launcher_run_lock(self):
        (self.portable / ".orbelle-running.lock").unlink()
        with self.assertRaisesRegex(ValueError, "run-lock"):
            updates.generate_update_bundles(self.portable, self.version, self.root / "other")

    def test_generation_and_manifest_require_the_versioned_launcher(self):
        (self.files / "OrbelleLauncher.exe").unlink()
        write_managed_files(self.files)
        with self.assertRaisesRegex(ValueError, "launcher"):
            updates.generate_update_bundles(self.portable, self.version, self.root / "other")
        self.manifest["files"] = [entry for entry in self.manifest["files"]
                                  if entry["path"] != "OrbelleLauncher.exe"]
        self.save_manifest()
        with self.assertRaisesRegex(ValueError, "Missing required update files"):
            updates.validate_update_assets(self.assets, self.version)

    def test_modified_bundle_is_rejected_before_inspection(self):
        self.paths[0].write_bytes(self.paths[0].read_bytes() + b"altered")
        with self.assertRaisesRegex(ValueError, "size/hash mismatch"):
            updates.validate_update_assets(self.assets, self.version)

    def test_generation_and_manifest_require_package_inventory_for_native_cleanup(self):
        (self.files / "manifest.json").unlink()
        write_managed_files(self.files)
        with self.assertRaisesRegex(ValueError, r"manifest\.json"):
            updates.generate_update_bundles(self.portable, self.version, self.root / "other")
        self.manifest["files"] = [entry for entry in self.manifest["files"]
                                  if entry["path"] != "manifest.json"]
        self.save_manifest()
        with self.assertRaisesRegex(ValueError, "Missing required update files"):
            updates.validate_update_assets(self.assets, self.version)

    def test_version_platform_layout_and_schema_are_exact(self):
        for key, value in (("version", "9.9.9"), ("platform", "linux-x64"),
                           ("layout", 2), ("schema", True)):
            original = self.manifest[key]
            with self.subTest(key=key):
                self.manifest[key] = value
                self.save_manifest()
                with self.assertRaises(ValueError):
                    updates.validate_update_assets(self.assets, self.version)
            self.manifest[key] = original

    def test_manifest_rejects_unsafe_windows_paths(self):
        original = self.manifest["files"][0]["path"]
        for path in ("../escape", "/absolute", "C:/escape", "resources\\escape", "file:stream",
                     "resources/CON.txt", "COM¹.log", "CONOUT$", "CLOCK$", "name.", "dir//file",
                     "dir/../file", "file\x00name", "file\x7fname", "cafe\u0301.txt",
                     "a" * 256, "/".join(["x" * 250] * 5)):
            with self.subTest(path=path):
                self.manifest["files"][0]["path"] = path
                self.save_manifest()
                with self.assertRaises(ValueError):
                    updates.validate_update_assets(self.assets, self.version)
        self.manifest["files"][0]["path"] = original

    def test_manifest_rejects_case_aliases_and_wrong_bundle_assignment(self):
        extra = dict(self.manifest["files"][0])
        extra["path"] = extra["path"].swapcase()
        self.manifest["files"].append(extra)
        self.save_manifest()
        with self.assertRaisesRegex(ValueError, "Duplicate Windows"):
            updates.validate_update_assets(self.assets, self.version)
        self.manifest["files"].pop()
        self.manifest["files"][0]["bundle"] = "runtime"
        self.save_manifest()
        with self.assertRaisesRegex(ValueError, "wrong update bundle"):
            updates.validate_update_assets(self.assets, self.version)

    def test_manifest_cannot_redirect_bundle_downloads(self):
        self.manifest["bundles"][0]["name"] = "https://example.invalid/program.zip"
        self.save_manifest()
        with self.assertRaisesRegex(ValueError, "bundle name"):
            updates.validate_update_assets(self.assets, self.version)

    def test_manifest_rejects_empty_bundle(self):
        self.manifest["files"] = [entry for entry in self.manifest["files"]
                                  if entry["bundle"] != "runtime"]
        self.save_manifest()
        with self.assertRaisesRegex(ValueError, "Empty update bundle"):
            updates.validate_update_assets(self.assets, self.version)

    def test_manifest_rejects_case_aliases_in_parent_directories(self):
        extra = {"path": "Resources/another.txt", "size": 1,
                 "sha256": hashlib.sha256(b"X").hexdigest(), "bundle": "resources"}
        self.manifest["files"].append(extra)
        self.save_manifest()
        with self.assertRaisesRegex(ValueError, "Case-colliding update directory"):
            updates.validate_update_assets(self.assets, self.version)

    def test_extra_zip_member_is_rejected_even_when_bundle_hash_matches(self):
        self.rewrite_bundle("app", lambda entries: entries + [(zipfile.ZipInfo("extra.exe"), b"extra")])
        with self.assertRaisesRegex(ValueError, "inventory differs"):
            updates.validate_update_assets(self.assets, self.version)

    def test_zip_symlink_is_rejected_even_when_bytes_and_hash_match(self):
        def link(entries):
            entries[0][0].create_system = 3
            entries[0][0].external_attr = (stat.S_IFLNK | 0o777) << 16
            return entries
        self.rewrite_bundle("app", link)
        with self.assertRaisesRegex(ValueError, "Invalid update ZIP member"):
            updates.validate_update_assets(self.assets, self.version)

    def test_file_hash_is_checked_after_the_bundle_hash(self):
        self.rewrite_bundle("app", lambda entries: [(info, b"X" * len(data))
                                                    for info, data in entries])
        with self.assertRaisesRegex(ValueError, "file hash mismatch"):
            updates.validate_update_assets(self.assets, self.version)

    def test_management_marker_is_verified_independently(self):
        marker = next(item for item in self.manifest["files"] if item["path"] == updates.MANAGED_MARKER)
        altered = b"1\n" + b"0" * 64 + b"\n"
        marker["sha256"] = hashlib.sha256(altered).hexdigest()
        self.rewrite_bundle("app", lambda entries: [(info, altered if info.filename == updates.MANAGED_MARKER else data)
                                                    for info, data in entries])
        with self.assertRaisesRegex(ValueError, "marker does not match"):
            updates.validate_update_assets(self.assets, self.version)

    def test_duplicate_json_keys_are_not_silently_accepted(self):
        self.manifest_path.write_text('{"schema":1,"schema":2}', encoding="utf-8")
        with self.assertRaisesRegex(ValueError, "Duplicate update manifest key"):
            updates.validate_update_assets(self.assets, self.version)


if __name__ == "__main__":
    unittest.main()
