"""Select CI work without weakening the final required check."""

import argparse
import json
import os
from pathlib import Path
import re
import subprocess


ROOT = Path(__file__).resolve().parents[2]
JOBS = {"metadata", "linux", "sanitizers", "linux-consumer", "windows", "python-linux", "python-windows"}
REQUIRED = {
    "full": JOBS - {"linux-consumer"},
    "docs": {"metadata"},
    "baseline": {"metadata", "linux", "windows"},
    "release": JOBS,
    "sanitizers": {"metadata", "sanitizers"},
}
DESCRIPTIONS = {
    "full": "Native and desktop tests plus every supported Python version; no release packages.",
    "docs": "Documentation-only change: workflow, version and tooling checks; native builds are skipped.",
    "baseline": "Main integration tests: native/desktop and baseline Python; no matrix or release packages.",
    "release": "Full numerical tests, routine sanitizers, release builds and isolated installation checks.",
    "sanitizers": "Manual full ASan/UBSan run; native Release and Python jobs are not repeated.",
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


def push_paths(source, before, after):
    # Compare the entire push, not just the last commit. Missing history, new
    # branches and non-fast-forward updates must keep baseline integration tests.
    if any(not isinstance(sha, str) or not re.fullmatch(r"[0-9a-f]{40}|[0-9a-f]{64}", sha) or not sha.strip("0")
           for sha in (before, after)) or before == after:
        raise ValueError("Expected distinct, nonzero push commit IDs")
    head = subprocess.check_output(["git", "rev-parse", "HEAD"], cwd=source).decode().strip()
    if head != after:
        raise ValueError("Checkout does not match the push's after commit")
    subprocess.run(["git", "merge-base", "--is-ancestor", before, after], cwd=source, check=True)
    paths = subprocess.check_output(
        ["git", "diff", "--name-only", "--no-renames", "-z", before, after, "--"], cwd=source)
    return [path.decode("utf-8") for path in paths.split(b"\0") if path]


def select_scope(event, ref, source=ROOT, release_tag="", before="", after="", manual_mode="all"):
    if release_tag:
        return "release"
    if event == "workflow_dispatch":
        if manual_mode not in {"all", "sanitizers"}:
            raise ValueError(f"Unknown manual CI mode: {manual_mode!r}")
        return "sanitizers" if manual_mode == "sanitizers" else "full"
    if event == "push" and ref == "refs/heads/main":
        try:
            if documentation_only(push_paths(source, before, after)):
                return "docs"
        except (OSError, ValueError, subprocess.CalledProcessError) as error:
            print(f"Cannot classify the push; keeping baseline checks: {error}")
        return "baseline"
    if event == "pull_request":
        try:
            if documentation_only(pull_request_paths(source)):
                return "docs"
        except (OSError, ValueError, subprocess.CalledProcessError) as error:
            print(f"Cannot classify the PR; keeping full checks: {error}")
    # Unfamiliar events always retain full numerical and routine sanitizer checks.
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

    event = {}
    if os.environ.get("GITHUB_EVENT_NAME") == "push":
        try:
            event = json.loads(Path(os.environ["GITHUB_EVENT_PATH"]).read_text(encoding="utf-8"))
            if not isinstance(event, dict):
                event = {}
        except (KeyError, OSError, ValueError) as error:
            print(f"Cannot read push event; keeping baseline checks: {error}")
    scope = select_scope(os.environ.get("GITHUB_EVENT_NAME", ""),
                         os.environ.get("GITHUB_REF", ""),
                         release_tag=os.environ.get("RELEASE_TAG", ""),
                         before=event.get("before", ""), after=event.get("after", ""),
                         manual_mode=os.environ.get("MANUAL_MODE", "all"))
    sanitizer_profile = "full" if os.environ.get("GITHUB_EVENT_NAME") == "workflow_dispatch" else "routine"
    print(f"CI scope: {scope}. {DESCRIPTIONS[scope]}")
    if output := os.environ.get("GITHUB_OUTPUT"):
        with Path(output).open("a", encoding="utf-8") as stream:
            stream.write(f"scope={scope}\nsanitizer-profile={sanitizer_profile}\n")
    if summary := os.environ.get("GITHUB_STEP_SUMMARY"):
        with Path(summary).open("a", encoding="utf-8") as stream:
            stream.write(f"## CI scope: {scope}\n\n{DESCRIPTIONS[scope]}\n")
            if "sanitizers" in REQUIRED[scope]:
                stream.write(f"\nASan/UBSan profile: **{sanitizer_profile}**.\n")


if __name__ == "__main__":
    main()
