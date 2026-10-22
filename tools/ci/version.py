"""Check release versions, or update the three project manifests together."""
import argparse
import json
import os
from pathlib import Path
import re
import tomllib


VERSION = r"(?:0|[1-9][0-9]*)\.(?:0|[1-9][0-9]*)\.(?:0|[1-9][0-9]*)"
ROOT = Path(__file__).resolve().parents[2]


def python_versions(root=ROOT):
    config = tomllib.loads((root / "pyproject.toml").read_text(encoding="utf-8"))["tool"]["orbelle"]["ci"]
    versions = config["python-versions"]
    baseline = config["baseline-python"]
    if (not versions or baseline not in versions or len(set(versions)) != len(versions)
            or not all(re.fullmatch(r"3\.[1-9][0-9]*", item) for item in versions)):
        raise ValueError("Invalid Python CI matrix")
    return baseline, versions


def read_versions(root):
    cmake = (root / "CMakeLists.txt").read_text(encoding="utf-8")
    match = re.search(r"project\(Orbelle\s+VERSION\s+(\S+)", cmake)
    if not match:
        raise ValueError("Cannot find the Orbelle CMake project version")
    return {
        "CMakeLists.txt": match[1],
        "pyproject.toml": tomllib.loads((root / "pyproject.toml").read_text(encoding="utf-8"))["project"]["version"],
        "vcpkg.json": json.loads((root / "vcpkg.json").read_text(encoding="utf-8"))["version-semver"],
    }


def check(root, tag=None):
    versions = read_versions(root)
    if len(set(versions.values())) != 1:
        raise ValueError(f"Project versions disagree: {versions}")
    version = next(iter(versions.values()))
    if not re.fullmatch(VERSION, version):
        raise ValueError(f"Expected a stable X.Y.Z project version, got {version!r}")
    if tag is not None and (not re.fullmatch("v" + VERSION, tag) or tag != "v" + version):
        raise ValueError(f"Release tag must be v{version}, got {tag!r}")
    return version


def set_version(root, version):
    if not re.fullmatch(VERSION, version):
        raise ValueError("Version must be X.Y.Z without a v prefix, suffix or leading zeroes")
    old_versions = read_versions(root)
    replacements = {
        "CMakeLists.txt": (r"(project\(Orbelle\s+VERSION\s+)\S+", rf"\g<1>{version}"),
        "pyproject.toml": (r'(?m)^(version\s*=\s*)"' + re.escape(old_versions["pyproject.toml"]) + r'"', rf'\g<1>"{version}"'),
        "vcpkg.json": (r'("version-semver"\s*:\s*)"[^"]+"', rf'\g<1>"{version}"'),
    }
    # Validate every replacement before touching any manifest.
    updates = {}
    for name, (pattern, replacement) in replacements.items():
        path = root / name
        contents, count = re.subn(pattern, replacement, path.read_text(encoding="utf-8"))
        if count != 1:
            raise ValueError(f"Expected one version entry in {name}, found {count}")
        updates[path] = contents
    for path, contents in updates.items():
        path.write_text(contents, encoding="utf-8", newline="\n")
    return check(root)


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    group = parser.add_mutually_exclusive_group()
    group.add_argument("--tag", help="Require this exact vX.Y.Z release tag")
    group.add_argument("--set", dest="new_version", help="Update CMake, Python and vcpkg versions to X.Y.Z")
    parser.add_argument("--github-output", action="store_true")
    args = parser.parse_args()
    try:
        version = set_version(ROOT, args.new_version) if args.new_version else check(ROOT, args.tag)
        baseline = json.loads((ROOT / "vcpkg.json").read_text(encoding="utf-8"))["builtin-baseline"]
        if not re.fullmatch(r"[0-9a-f]{40}", baseline):
            raise ValueError("vcpkg baseline must be a full commit SHA")
    except ValueError as error:
        parser.error(str(error))
    if args.github_output:
        with open(os.environ["GITHUB_OUTPUT"], "a", encoding="utf-8") as output:
            output.write(f"version={version}\nvcpkg-baseline={baseline}\n")
            python_baseline, versions = python_versions()
            extra_versions = [item for item in versions if item != python_baseline]
            output.write(f"baseline-python={python_baseline}\npython-matrix={json.dumps(extra_versions)}\n")
    print(version)


if __name__ == "__main__":
    main()
