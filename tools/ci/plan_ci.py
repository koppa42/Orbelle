"""Select CI work without weakening the final required check."""

import argparse
import json
import os
from pathlib import Path
import subprocess


ROOT = Path(__file__).resolve().parents[2]
JOBS = {"metadata", "linux", "linux-consumer", "windows", "python-matrix"}
REQUIRED = {
    "full": JOBS,
    "docs": {"metadata"},
    "cache": {"metadata", "linux", "windows"},
}
DESCRIPTIONS = {
    "full": "Full build, tests, SDK/desktop packages and every supported Python version.",
    "docs": "Documentation-only PR: version and tooling checks; native builds are skipped.",
    "cache": "Main maintenance: version/tooling checks and shared dependency caches; no repeat product build.",
}


def documentation_only(paths):
    # Keep this allowlist narrow. A Markdown fixture or a script under docs can
    # affect the product, so arbitrary .md files and docs/** are not exempt.
    return bool(paths) and all(
        path in {"README.md", "AGENTS.md"} or
        (path.startswith("docs/") and path.endswith(".md"))
        for path in paths
    )


def pull_request_paths(source):
    # checkout's default PR ref is the synthetic merge commit. Depth 2 supplies
    # both parents; compare with the base parent to include the entire PR.
    parents = subprocess.check_output(
        ["git", "show", "--no-patch", "--format=%P", "HEAD"], cwd=source
    ).decode("ascii").split()
    if len(parents) != 2:
        raise ValueError("Expected the PR merge ref with both parents available")
    paths = subprocess.check_output(
        ["git", "diff", "--name-only", "--no-renames", "-z", parents[0], "HEAD", "--"],
        cwd=source,
    )
    # Include both sides of renames: moving source into docs must still build.
    return [path.decode("utf-8") for path in paths.split(b"\0") if path]


def select_scope(event, ref, source=ROOT, release_tag=""):
    if release_tag:
        return "full"
    if event == "push" and ref == "refs/heads/main":
        return "cache"
    if event == "pull_request":
        try:
            if documentation_only(pull_request_paths(source)):
                return "docs"
        except (OSError, ValueError, subprocess.CalledProcessError) as error:
            print(f"Cannot classify the PR; keeping full checks: {error}")
    # Manual runs, release tags and unfamiliar events always retain full checks.
    return "full"


def check_results(scope, results):
    if scope not in REQUIRED:
        raise ValueError(f"Missing or unknown CI scope: {scope!r}")
    if set(results) != JOBS:
        raise ValueError(f"Unexpected CI jobs: {sorted(results)}")
    failures = {}
    for name, job in results.items():
        expected = "success" if name in REQUIRED[scope] else "skipped"
        if job.get("result") != expected:
            failures[name] = {"expected": expected, "actual": job.get("result")}
    if failures:
        raise ValueError(f"CI scope {scope!r} did not pass: {failures}")


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("command", choices=("plan", "check"))
    args = parser.parse_args()
    if args.command == "check":
        scope = os.environ.get("CI_SCOPE", "")
        check_results(scope, json.loads(os.environ["RESULTS"]))
        print(f"All checks required by scope {scope!r} passed.")
        return

    scope = select_scope(os.environ.get("GITHUB_EVENT_NAME", ""),
                         os.environ.get("GITHUB_REF", ""),
                         release_tag=os.environ.get("RELEASE_TAG", ""))
    print(f"CI scope: {scope}. {DESCRIPTIONS[scope]}")
    if output := os.environ.get("GITHUB_OUTPUT"):
        with Path(output).open("a", encoding="utf-8") as stream:
            stream.write(f"scope={scope}\n")
    if summary := os.environ.get("GITHUB_STEP_SUMMARY"):
        with Path(summary).open("a", encoding="utf-8") as stream:
            stream.write(f"## CI scope: {scope}\n\n{DESCRIPTIONS[scope]}\n")


if __name__ == "__main__":
    main()
