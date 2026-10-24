"""Select reusable vcpkg snapshots and explain which packages needed rebuilding."""

import argparse
import hashlib
import html
import json
import os
from pathlib import Path
import re
import zipfile


FAMILIES = {
    "windows": ("vcpkg-win2022-release-v3-", "vcpkg-win2022-release-v2-"),
    "linux": ("vcpkg-ubuntu24-x64-v2-", "vcpkg-ubuntu24-x64-v1-"),
    "python-windows": ("python-deps-windows-v2-", "python-deps-windows-v1-"),
    "python-linux": ("python-deps-linux-v2-", "python-deps-linux-v1-"),
}
VERSION_FIELDS = ("version", "version-semver", "version-string", "version-date", "port-version")
ABI_NAME = re.compile(r"[0-9a-f]{64}\.zip")


def fingerprint(source):
    manifest = json.loads((source / "vcpkg.json").read_text(encoding="utf-8"))
    # Only the root application's version is irrelevant. Dependency versions,
    # overrides, features and the pinned registry baseline remain significant.
    for field in VERSION_FIELDS:
        manifest.pop(field, None)
    files = {"vcpkg.json": json.dumps(manifest, sort_keys=True, separators=(",", ":")).encode()}
    configuration = source / "vcpkg-configuration.json"
    files[configuration.name] = json.dumps(json.loads(configuration.read_text(encoding="utf-8")),
                                          sort_keys=True, separators=(",", ":")).encode()
    for directory in ("cmake/ports", "cmake/triplets/ci"):
        for path in (source / directory).rglob("*"):
            if path.is_file():
                files[path.relative_to(source).as_posix()] = path.read_bytes()
    digest = hashlib.sha256()
    for name, data in sorted(files.items()):
        digest.update(name.encode() + b"\0" + hashlib.sha256(data).digest())
    return digest.hexdigest()


def cache_identity(source, profile, image, run, attempt):
    for value in (image, run, attempt):
        if not re.fullmatch(r"[A-Za-z0-9_.-]+", value):
            raise ValueError("Cache image/run identifiers must be nonempty single-line tokens")
    current, legacy = FAMILIES[profile]
    digest = fingerprint(source)
    prefix = f"{current}{image}-{digest}-"
    families = [current, legacy]
    if profile.startswith("python-"):
        families += list(FAMILIES[profile.removeprefix("python-")])
    # All same-image options precede any cross-image fallback. Keep legacy
    # families and paths so rollout can reuse the existing expensive packages.
    restore_keys = [prefix, *(f"{family}{image}-" for family in families), *families]
    return {"profile": profile, "image": image, "fingerprint": digest,
            "prefix": prefix, "key": f"{prefix}{run}-{attempt}", "restore_keys": restore_keys}


def inventory(cache):
    result = {}
    for path in sorted(cache.rglob("*.zip")):
        if not ABI_NAME.fullmatch(path.name) or not zipfile.is_zipfile(path):
            continue
        stat = path.stat()
        if stat.st_size:
            result[path.relative_to(cache).as_posix()] = [stat.st_size, stat.st_mtime_ns]
    return result


def abi_inputs(data):
    return dict(line.split(" ", 1) for line in data.decode("utf-8").splitlines() if " " in line)


def cached_abis(cache, packages):
    result = []
    for relative in packages:
        try:
            with zipfile.ZipFile(cache / relative) as archive:
                for member in archive.infolist():
                    parts = member.filename.replace("\\", "/").split("/")
                    if (len(parts) != 3 or parts[0] != "share"
                            or parts[2] != "vcpkg_abi_info.txt" or member.file_size > 256 * 1024):
                        continue
                    inputs = abi_inputs(archive.read(member))
                    result.append({"port": parts[1], "abi": Path(relative).stem,
                                   "triplet": inputs.get("triplet", ""), "inputs": inputs})
        except (OSError, ValueError, zipfile.BadZipFile, UnicodeError):
            # Diagnostics must not prevent vcpkg from rejecting/replacing a bad
            # cached archive. No archive contents are extracted or executed.
            continue
    return result


def save_needed(before, after, prefix, matched):
    return bool(after) and (before != after or not matched.startswith(prefix))


def package_report(installed, previous, log):
    durations = dict(re.findall(r"Elapsed time to handle (\S+): ([^\r\n]+)", log))
    rebuilt = {f"{port}:{triplet}" for port, triplet in re.findall(
        r"Building ([a-z0-9-]+)(?:\[[^\]]*\])?:([a-z0-9-]+)@", log)}
    rows = []
    for path in sorted(installed.glob("*/share/*/vcpkg_abi_info.txt")):
        data = path.read_bytes()
        inputs = abi_inputs(data)
        port, triplet = path.parent.name, path.parents[2].name
        abi = hashlib.sha256(data).hexdigest()
        candidates = [item for item in previous
                      if item["port"] == port and item["triplet"] == triplet]
        exact = any(item["abi"] == abi for item in candidates)
        changed = {}
        if candidates and not exact:
            def differences(item):
                old = item["inputs"]
                return {key: {"before": old.get(key), "after": inputs.get(key)}
                        for key in sorted(old.keys() | inputs.keys()) if old.get(key) != inputs.get(key)}
            changed = min((differences(item) for item in candidates), key=len)
        package = f"{port}:{triplet}"
        rows.append({"package": package, "abi": abi, "available_before": exact,
                     "rebuilt": package in rebuilt, "duration": durations.get(package, "unavailable"),
                     "changed_inputs": changed})
    return rows


def write_json(path, value):
    path.parent.mkdir(parents=True, exist_ok=True)
    path.write_text(json.dumps(value, indent=2, ensure_ascii=False) + "\n", encoding="utf-8")


def outputs(values):
    if destination := os.environ.get("GITHUB_OUTPUT"):
        with Path(destination).open("a", encoding="utf-8") as output:
            for name, value in values.items():
                output.write(f"{name}<<CACHE_OUTPUT_END\n{value}\nCACHE_OUTPUT_END\n")


def summarize(state, report):
    def cell(value):
        return html.escape(str(value)).replace("|", "&#124;").replace("\r", " ").replace("\n", " ")
    lines = ["## Dependency cache", "",
             f"Runner image: `{state['image']}`; dependency fingerprint: `{state['fingerprint']}`.", "",
             f"Restored snapshot: `{state['matched'] or 'none'}`.", "",
             f"Complete archives: {report['before_count']} before, {report['after_count']} after. "
             f"Save a snapshot: {report['save']}.", "",
             "| Package | Matching ABI before install | Rebuilt | Install duration | Changed ABI inputs |",
             "| --- | --- | --- | --- | --- |"]
    for row in report["packages"]:
        lines.append("| " + " | ".join(cell(value) for value in (
            row["package"], row["available_before"], row["rebuilt"], row["duration"],
            ", ".join(row["changed_inputs"]) or "—")) + " |")
    if not report["packages"]:
        lines += ["", "No installed package metadata yet; completed archives can still be saved."]
    lines += ["", "ABI differences compare the closest cached variant; full values are in the diagnostics.", ""]
    summary = "\n".join(lines)
    print(summary)
    if destination := os.environ.get("GITHUB_STEP_SUMMARY"):
        with Path(destination).open("a", encoding="utf-8") as output:
            output.write(summary)


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    sub = parser.add_subparsers(dest="command", required=True)
    identify = sub.add_parser("identify")
    identify.add_argument("--source", type=Path, default=Path.cwd())
    identify.add_argument("--profile", choices=FAMILIES, required=True)
    identify.add_argument("--cache", type=Path, required=True)
    identify.add_argument("--state", type=Path, required=True)
    begin = sub.add_parser("begin")
    begin.add_argument("--state", type=Path, required=True)
    finish = sub.add_parser("finish")
    finish.add_argument("--state", type=Path, required=True)
    finish.add_argument("--installed", type=Path, required=True)
    finish.add_argument("--log", type=Path, required=True)
    args = parser.parse_args()
    identity_path = args.state / "identity.json"
    snapshot_path = args.state / "before.json"
    if args.command == "identify":
        state = cache_identity(args.source, args.profile, os.environ.get("ImageVersion", ""),
                               os.environ["GITHUB_RUN_ID"], os.environ["GITHUB_RUN_ATTEMPT"])
        state["cache"] = str(args.cache.resolve())
        write_json(identity_path, state)
        outputs({"key": state["key"], "restore-keys": "\n".join(state["restore_keys"])})
    elif args.command == "begin":
        state = json.loads(identity_path.read_text(encoding="utf-8"))
        cache = Path(state["cache"])
        packages = inventory(cache)
        state.update(matched=os.environ.get("CACHE_MATCHED_KEY", ""), packages=packages,
                     abis=cached_abis(cache, packages))
        write_json(snapshot_path, state)
    else:
        # Checkout/restore may have failed or been cancelled before inventory.
        if not snapshot_path.exists():
            outputs({"save": "false"})
            print("No initial cache inventory; nothing to save.")
            return
        state = json.loads(snapshot_path.read_text(encoding="utf-8"))
        after = inventory(Path(state["cache"]))
        save = save_needed(state["packages"], after, state["prefix"], state["matched"])
        # Decide saving before producing optional diagnostics, so a report
        # failure cannot discard packages completed during a failed build.
        outputs({"save": str(save).lower(), "key": state["key"], "cache": state["cache"]})
        log = args.log.read_text(encoding="utf-8", errors="replace") if args.log.is_file() else ""
        report = {"before_count": len(state["packages"]), "after_count": len(after), "save": save,
                  "packages": package_report(args.installed, state["abis"], log)}
        write_json(args.state / "report.json", report)
        summarize(state, report)


if __name__ == "__main__":
    main()
