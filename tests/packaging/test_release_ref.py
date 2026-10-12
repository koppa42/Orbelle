"""Exercise release ancestry using real annotated tags and divergent branches."""
from pathlib import Path
import subprocess
import sys
import tempfile
import unittest

ROOT = Path(__file__).resolve().parents[2]
sys.path.insert(0, str(ROOT / "tools/ci"))
import verify_release
sys.path.pop(0)


class ReleaseRefTests(unittest.TestCase):
    def setUp(self):
        directory = tempfile.TemporaryDirectory()
        self.addCleanup(directory.cleanup)
        self.root = Path(directory.name)
        self.git("init", "--quiet", "--initial-branch=main")
        self.git("config", "user.name", "CI test")
        self.git("config", "user.email", "ci@example.invalid")
        self.git("config", "commit.gpgsign", "false")
        self.git("config", "tag.gpgsign", "false")
        self.git("commit", "--quiet", "--allow-empty", "-m", "Initial")
        self.git("update-ref", "refs/remotes/origin/main", "HEAD")

    def git(self, *args):
        return subprocess.check_output(["git", *args], cwd=self.root, stderr=subprocess.STDOUT)

    def verify(self):
        return verify_release.verify_tag("v0.1.0", self.root)

    def test_annotated_tag_on_main_and_older_main_commit_are_allowed(self):
        self.git("tag", "-a", "v0.1.0", "-m", "Release")
        tagged = self.git("rev-parse", "HEAD").decode().strip()
        self.assertEqual(self.verify(), tagged)
        self.git("commit", "--quiet", "--allow-empty", "-m", "Next main commit")
        self.git("update-ref", "refs/remotes/origin/main", "HEAD")
        self.git("checkout", "--quiet", "v0.1.0")
        self.assertEqual(self.verify(), tagged)

    def test_tag_on_unmerged_topic_is_rejected(self):
        self.git("switch", "--quiet", "-c", "topic")
        self.git("commit", "--quiet", "--allow-empty", "-m", "Unmerged change")
        self.git("tag", "v0.1.0")
        with self.assertRaisesRegex(ValueError, "not been merged"):
            self.verify()

    def test_wrong_checkout_is_rejected_even_when_tag_is_on_main(self):
        self.git("tag", "v0.1.0")
        self.git("commit", "--quiet", "--allow-empty", "-m", "Different commit")
        self.git("update-ref", "refs/remotes/origin/main", "HEAD")
        with self.assertRaisesRegex(ValueError, "Checkout"):
            self.verify()

    def test_missing_remote_main_or_tag_fails_closed(self):
        with self.assertRaises(subprocess.CalledProcessError):
            self.verify()
        self.git("tag", "v0.1.0")
        self.git("update-ref", "-d", "refs/remotes/origin/main")
        with self.assertRaises(subprocess.CalledProcessError):
            self.verify()


if __name__ == "__main__":
    unittest.main()
