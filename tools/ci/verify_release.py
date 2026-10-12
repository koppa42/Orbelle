"""Reject release tags whose checked-out commit has not reached main."""
import argparse
from pathlib import Path
import subprocess

from version import ROOT, check


def verify_tag(tag, source=ROOT, main_ref="refs/remotes/origin/main"):
    def commit(ref):
        return subprocess.check_output(
            ["git", "rev-parse", "--verify", ref + "^{commit}"], cwd=source,
            stderr=subprocess.PIPE, text=True).strip()

    tagged = commit("refs/tags/" + tag)
    if commit("HEAD") != tagged:
        raise ValueError("Checkout must match the release tag's commit")
    target = commit(main_ref)
    result = subprocess.run(["git", "merge-base", "--is-ancestor", tagged, target], cwd=source)
    if result.returncode == 1:
        raise ValueError("Release tag's commit has not been merged into main")
    result.check_returncode()
    return tagged


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--tag", required=True)
    parser.add_argument("--source", type=Path, default=ROOT)
    args = parser.parse_args()
    check(args.source, args.tag)
    print(f"Verified release commit: {verify_tag(args.tag, args.source)}")


if __name__ == "__main__":
    main()
