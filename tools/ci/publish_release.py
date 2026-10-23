"""Validate release assets, upload a complete draft, then make it public."""
import argparse
import hashlib
import json
import os
from pathlib import Path
import subprocess
import sys
import tempfile
import zipfile

from version import ROOT, check, python_versions

sys.path.insert(0, str(Path(__file__).resolve().parents[1] / "packaging"))
from update_bundles import asset_names, validate_portable_archive, validate_update_assets
sys.path.pop(0)


def validate_assets(directory, version):
    expected = {
        f"Orbelle-{version}-win-x64.zip",
        f"Orbelle-{version}-sdk-win-x64.zip",
        f"Orbelle-{version}-sdk-linux-x64.tar.gz",
    }
    expected.update(asset_names(version))
    for python in python_versions()[1]:
        tag = "cp" + python.replace(".", "")
        expected.update(f"orbelle-{version}-{tag}-{tag}-{platform}.whl"
                        for platform in ("win_amd64", "linux_x86_64"))
    actual = {p.name for p in directory.iterdir()}
    if actual != expected:
        raise ValueError(f"Release assets differ: missing={expected - actual}, unexpected={actual - expected}")
    for name in sorted(expected):
        path = directory / name
        if not path.is_file() or path.stat().st_size == 0:
            raise ValueError(f"Empty or invalid asset: {name}")
        if name.endswith(".whl"):
            with zipfile.ZipFile(path) as wheel:
                metadata = wheel.read(f"orbelle-{version}.dist-info/METADATA").decode("utf-8")
                if f"\nVersion: {version}\n" not in metadata:
                    raise ValueError(f"Wheel metadata version differs: {name}")
                if wheel.testzip() is not None:
                    raise ValueError(f"Corrupt wheel: {name}")
    validate_update_assets(directory, version)
    validate_portable_archive(directory / f"Orbelle-{version}-win-x64.zip", version,
                              directory / f"Orbelle-{version}-win-x64-update.json")
    return [directory / name for name in sorted(expected)]


def publish(tag, assets, repo, run=subprocess.run):
    version = check(ROOT, tag)
    files = validate_assets(assets, version)
    checksums = assets / "SHA256SUMS"
    lines = []
    for path in files:
        with path.open("rb") as stream:
            lines.append(f"{hashlib.file_digest(stream, 'sha256').hexdigest()}  {path.name}\n")
    checksums.write_text("".join(lines), encoding="utf-8", newline="\n")
    files.append(checksums)

    def gh(*args, **kwargs):
        return run(["gh", *args, "--repo", repo], **kwargs)

    existing = gh("release", "view", tag, "--json", "isDraft", capture_output=True, text=True)
    if existing.returncode == 0 and not json.loads(existing.stdout)["isDraft"]:
        raise ValueError(f"{tag} is already published; published releases are never overwritten")
    if existing.returncode != 0:
        notes = (
            "Windows x64 desktop: download the portable ZIP and extract the whole folder. "
            "The desktop requires a Vulkan-capable GPU and driver.\n\n"
            "The app, runtime and resources ZIPs plus update.json are consumed by the desktop updater; "
            "for a new installation, use the complete portable ZIP.\n\n"
            "C++ SDKs include headless Runtime/ERFA/IERS/CSPICE/SQLite and their link dependencies. "
            "See README.txt inside the archive.\n\n"
            "Python wheels: CPython " + ", ".join(python_versions()[1]) + " x64 on Windows and Ubuntu 24.04. "
            "The Linux wheel is a native linux_x86_64 build, not a manylinux wheel. "
            "Install the matching file with pip install <wheel>.\n\n"
            "CI checks C++, installed SDK/Python APIs and Qt offscreen tests. "
            "GPU rendering and performance require separate hardware validation. "
            "SHA256SUMS covers every uploaded package.\n"
        )
        with tempfile.TemporaryDirectory() as temporary:
            note_path = Path(temporary) / "release-notes.md"
            note_path.write_text(notes, encoding="utf-8")
            gh("release", "create", tag, "--verify-tag", "--draft", "--title", f"星弦 Orbelle {tag}",
               "--generate-notes", "--notes-file", str(note_path), check=True)
    # A failed upload leaves a draft. Reruns can replace only these known assets.
    gh("release", "upload", tag, *map(str, files), "--clobber", check=True)
    uploaded = gh("release", "view", tag, "--json", "assets", capture_output=True, text=True, check=True)
    if {asset["name"] for asset in json.loads(uploaded.stdout)["assets"]} != {path.name for path in files}:
        raise ValueError("Draft asset list does not match the tested packages; leaving it unpublished")
    gh("release", "edit", tag, "--draft=false", check=True)


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--tag", required=True)
    parser.add_argument("--assets", type=Path, required=True)
    args = parser.parse_args()
    publish(args.tag, args.assets, os.environ["GH_REPO"])


if __name__ == "__main__":
    main()
