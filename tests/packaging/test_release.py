"""Exercise publication failure boundaries without contacting GitHub."""
import json
from pathlib import Path
import subprocess
import sys
import tempfile
import unittest
import zipfile

ROOT = Path(__file__).resolve().parents[2]
sys.path.insert(0, str(ROOT / "tools/ci"))
import publish_release
import version
sys.path.pop(0)


class ReleaseTests(unittest.TestCase):
    def setUp(self):
        self.temporary = tempfile.TemporaryDirectory()
        self.addCleanup(self.temporary.cleanup)
        self.assets = Path(self.temporary.name)
        self.version = version.check(ROOT)
        self.tag = "v" + self.version
        for name in (f"Orbelle-{self.version}-win-x64.zip",
                     f"Orbelle-{self.version}-sdk-win-x64.zip",
                     f"Orbelle-{self.version}-sdk-linux-x64.tar.gz"):
            (self.assets / name).write_bytes(b"test archive")
        for python in version.python_versions()[1]:
            tag = "cp" + python.replace(".", "")
            for platform in ("win_amd64", "linux_x86_64"):
                with zipfile.ZipFile(self.assets / f"orbitforge-{self.version}-{tag}-{tag}-{platform}.whl", "w") as wheel:
                    wheel.writestr(f"orbitforge-{self.version}.dist-info/METADATA",
                                   f"Metadata-Version: 2.1\nName: orbitforge\nVersion: {self.version}\n")
        self.asset_count = 3 + 2 * len(version.python_versions()[1])

    def test_requires_every_platform_and_rejects_extra_files(self):
        self.assertEqual(len(publish_release.validate_assets(self.assets, self.version)), self.asset_count)
        extra = self.assets / "private.log"
        extra.write_text("diagnostic")
        with self.assertRaises(ValueError):
            publish_release.validate_assets(self.assets, self.version)
        extra.unlink()
        next(self.assets.glob("*.whl")).unlink()
        with self.assertRaises(ValueError):
            publish_release.validate_assets(self.assets, self.version)

    def test_upload_failure_never_publishes(self):
        calls = []

        def run(command, **kwargs):
            calls.append(command)
            if "upload" in command:
                raise subprocess.CalledProcessError(1, command)
            return subprocess.CompletedProcess(command, 1 if "view" in command else 0, stdout="")

        with self.assertRaises(subprocess.CalledProcessError):
            publish_release.publish(self.tag, self.assets, "owner/repo", run)
        self.assertTrue(any("--draft" in command for command in calls))
        self.assertFalse(any("edit" in command for command in calls))
        self.assertEqual(len((self.assets / "SHA256SUMS").read_text().splitlines()), self.asset_count)

    def test_existing_draft_resumes_and_publishes_after_inventory_check(self):
        calls = []

        def run(command, **kwargs):
            calls.append(command)
            result = {"isDraft": True} if "isDraft" in command else {
                "assets": [{"name": path.name} for path in self.assets.iterdir()]}
            return subprocess.CompletedProcess(command, 0, stdout=json.dumps(result))

        publish_release.publish(self.tag, self.assets, "owner/repo", run)
        self.assertFalse(any("create" in command for command in calls))
        self.assertIn("--draft=false", calls[-1])
        self.assertTrue(any("--clobber" in command for command in calls))

    def test_published_release_is_never_overwritten(self):
        calls = []

        def run(command, **kwargs):
            calls.append(command)
            return subprocess.CompletedProcess(command, 0, stdout='{"isDraft": false}')

        with self.assertRaisesRegex(ValueError, "already published"):
            publish_release.publish(self.tag, self.assets, "owner/repo", run)
        self.assertEqual(len(calls), 1)


if __name__ == "__main__":
    unittest.main()
