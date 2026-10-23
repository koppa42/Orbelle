"""Create and verify the three reproducible Windows update bundles."""
from __future__ import annotations

import hashlib
import json
from pathlib import Path
import re
import shutil
import stat
import unicodedata
import zipfile

VERSION = r"(?:0|[1-9][0-9]*)\.(?:0|[1-9][0-9]*)\.(?:0|[1-9][0-9]*)"
BUNDLES = ("app", "runtime", "resources")
MANAGED_LIST = "orbelle-managed-files.txt"
MANAGED_MARKER = "orbelle-managed.txt"
MANAGEMENT_FILES = {MANAGED_LIST, MANAGED_MARKER}
APPLICATION_FILES = {
    "OrbelleApp.exe", "OrbelleUpdater.exe", "OrbelleLauncher.exe", "qt.conf", "manifest.json",
}
FIXED_ZIP_TIME = (1980, 1, 1, 0, 0, 0)
MAX_FILES = 50000
MAX_TOTAL_SIZE = 8 * 1024 * 1024 * 1024


def digest(path: Path) -> str:
    with path.open("rb") as stream:
        return hashlib.file_digest(stream, "sha256").hexdigest()


def asset_names(version: str) -> list[str]:
    if not re.fullmatch(VERSION, version):
        raise ValueError("Update version must be stable X.Y.Z")
    prefix = f"Orbelle-{version}-win-x64"
    return [f"{prefix}-{bundle}.zip" for bundle in BUNDLES] + [f"{prefix}-update.json"]


def checked_path(value: str, max_length=1024) -> str:
    """Accept only ordinary relative file names with unambiguous Windows semantics."""
    if (not isinstance(value, str) or not value or "\\" in value
            or len(value.encode("utf-16-le")) // 2 > max_length
            or unicodedata.normalize("NFC", value) != value):
        raise ValueError(f"Invalid update path: {value!r}")
    for component in value.split("/"):
        if (component in ("", ".", "..") or component[-1:] in (".", " ")
                or len(component.encode("utf-16-le")) // 2 > 255
                or any(ord(char) < 32 or ord(char) == 127 or char in '<>:"|?*' for char in component)
                or re.fullmatch(r"(?:CON|PRN|AUX|NUL|CLOCK\$|CONIN\$|CONOUT\$|COM[1-9¹²³]|LPT[1-9¹²³])(?:\..*)?",
                                component, re.I)):
            raise ValueError(f"Unsafe update path: {value!r}")
    return value


def bundle_for(path: str) -> str:
    parts = path.split("/")
    if parts[0].lower() in ("resources", "notices") and len(parts) > 1:
        return "resources"
    if ((parts[0].lower() == "plugins" and len(parts) > 1)
            or (len(parts) == 1 and path.lower().endswith(".dll"))):
        return "runtime"
    return "app"


def _unique_paths(paths, max_length=1024):
    folded = set()
    aliases = {}
    for path in paths:
        checked_path(path, max_length)
        if path.casefold() in folded:
            raise ValueError(f"Duplicate Windows update path: {path}")
        folded.add(path.casefold())
        pieces = path.split("/")
        for end in range(1, len(pieces) + 1):
            prefix = "/".join(pieces[:end])
            key = prefix.casefold()
            if key in aliases and aliases[key] != prefix:
                raise ValueError(f"Case-colliding update directory: {path}")
            aliases[key] = prefix
    for path in folded:
        pieces = path.split("/")
        if any("/".join(pieces[:end]) in folded for end in range(1, len(pieces))):
            raise ValueError(f"Update path is both a file and a directory: {path}")


def _managed_files(files, contents):
    if not MANAGEMENT_FILES.issubset(files):
        raise ValueError("Update is missing managed-file markers")
    listing = contents[MANAGED_LIST]
    expected_marker = f"1\n{hashlib.sha256(listing).hexdigest()}\n".encode("ascii")
    if contents[MANAGED_MARKER] != expected_marker:
        raise ValueError("Managed-file marker does not match its list")
    expected = {path: entry["sha256"] for path, entry in files.items() if path not in MANAGEMENT_FILES}
    actual = {}
    for line in listing.decode("utf-8").splitlines():
        match = re.fullmatch(r"([0-9a-f]{64})  (.+)", line)
        if not match or match[2] in actual:
            raise ValueError("Invalid managed-file list")
        actual[checked_path(match[2])] = match[1]
    if actual != expected:
        raise ValueError("Managed-file list differs from the complete update file inventory")


def generate_update_bundles(output: Path, version: str, destination: Path) -> list[Path]:
    """Bundle output/versions/version; return three ZIPs followed by their JSON manifest."""
    assets = [destination / name for name in asset_names(version)]
    root = output / "versions" / version
    if (output / "orbelle-layout.txt").read_bytes() != b"1\n":
        raise ValueError("Unsupported portable layout")
    if (output / "active-version.txt").read_bytes() != (version + "\n").encode("ascii"):
        raise ValueError("Portable active version differs from the update version")
    lock = output / ".orbelle-running.lock"
    if not lock.is_file() or lock.is_symlink():
        raise ValueError("Portable installation is missing its ordinary run-lock file")
    if not root.is_dir() or root.is_symlink() or root.is_junction():
        raise ValueError("Missing or linked portable version directory")
    files = []
    sources = {}
    for source in sorted(root.rglob("*"), key=lambda path: path.relative_to(root).as_posix()):
        if source.is_symlink() or source.is_junction():
            raise ValueError(f"Linked update source: {source}")
        if source.is_dir():
            continue
        if not source.is_file():
            raise ValueError(f"Non-regular update source: {source}")
        path = checked_path(source.relative_to(root).as_posix())
        sources[path] = source
        files.append({"path": path, "size": source.stat().st_size,
                      "sha256": digest(source), "bundle": bundle_for(path)})
    _unique_paths(sources)
    _managed_files({entry["path"]: entry for entry in files},
                   {path: sources[path].read_bytes() for path in MANAGEMENT_FILES if path in sources})
    if not APPLICATION_FILES.issubset(sources):
        raise ValueError("Update is missing required application, updater, launcher, qt.conf or manifest.json files")
    destination.mkdir(parents=True, exist_ok=True)
    if any(path.exists() for path in assets):
        raise FileExistsError("Refusing to overwrite existing update assets")
    bundles = []
    created = []
    try:
        for bundle, asset in zip(BUNDLES, assets):
            with zipfile.ZipFile(asset, "x", compression=zipfile.ZIP_DEFLATED, compresslevel=6) as archive:
                created.append(asset)
                for entry in files:
                    if entry["bundle"] != bundle:
                        continue
                    info = zipfile.ZipInfo(entry["path"], date_time=FIXED_ZIP_TIME)
                    info.create_system = 3
                    info.external_attr = (stat.S_IFREG | 0o644) << 16
                    info.compress_type = zipfile.ZIP_DEFLATED
                    with sources[entry["path"]].open("rb") as source, archive.open(info, "w", force_zip64=True) as target:
                        shutil.copyfileobj(source, target, 1024 * 1024)
            bundles.append({"id": bundle, "name": asset.name, "size": asset.stat().st_size,
                            "sha256": digest(asset)})
        manifest = {"schema": 1, "version": version, "platform": "win-x64", "layout": 1,
                    "files": files, "bundles": bundles}
        with assets[-1].open("x", encoding="utf-8", newline="\n") as stream:
            created.append(assets[-1])
            stream.write(json.dumps(manifest, ensure_ascii=False, indent=2) + "\n")
        validate_update_assets(destination, version)
        return assets
    except Exception:
        for path in created:
            path.unlink(missing_ok=True)
        raise


def _json_object(pairs):
    result = {}
    for key, value in pairs:
        if key in result:
            raise ValueError(f"Duplicate update manifest key: {key}")
        result[key] = value
    return result


def _size_and_hash(entry):
    if (type(entry.get("size")) is not int or entry["size"] < 0
            or not isinstance(entry.get("sha256"), str)
            or not re.fullmatch(r"[0-9a-f]{64}", entry["sha256"])):
        raise ValueError("Invalid update size or SHA-256")


def validate_update_assets(directory: Path, version: str) -> list[Path]:
    """Validate metadata and every ZIP member without extracting untrusted paths."""
    assets = [directory / name for name in asset_names(version)]
    if any(not path.is_file() or path.is_symlink() for path in assets):
        raise ValueError("Missing or linked update assets")
    if assets[-1].stat().st_size > 16 * 1024 * 1024:
        raise ValueError("Update manifest exceeds the size limit")
    manifest = json.loads(assets[-1].read_text(encoding="utf-8"), object_pairs_hook=_json_object)
    if (not isinstance(manifest, dict)
            or set(manifest) != {"schema", "version", "platform", "layout", "files", "bundles"}
            or type(manifest["schema"]) is not int or manifest["schema"] != 1
            or type(manifest["layout"]) is not int or manifest["layout"] != 1
            or manifest["version"] != version or manifest["platform"] != "win-x64"
            or not isinstance(manifest["files"], list) or not isinstance(manifest["bundles"], list)):
        raise ValueError("Invalid update manifest schema, version, platform or layout")
    files = {}
    if not 0 < len(manifest["files"]) <= MAX_FILES:
        raise ValueError("Update file count exceeds the limit")
    for entry in manifest["files"]:
        if not isinstance(entry, dict) or set(entry) != {"path", "size", "sha256", "bundle"}:
            raise ValueError("Invalid update file entry")
        path = checked_path(entry["path"])
        _size_and_hash(entry)
        if path in files or entry["bundle"] != bundle_for(path):
            raise ValueError("Duplicate file or wrong update bundle assignment")
        files[path] = entry
    _unique_paths(files)
    if {entry["bundle"] for entry in files.values()} != set(BUNDLES):
        raise ValueError("Empty update bundle")
    if sum(entry["size"] for entry in files.values()) > MAX_TOTAL_SIZE:
        raise ValueError("Update unpacked size exceeds the limit")
    if not (APPLICATION_FILES | MANAGEMENT_FILES).issubset(files):
        raise ValueError("Missing required update files")
    bundles = {}
    expected_names = dict(zip(BUNDLES, asset_names(version)))
    for entry in manifest["bundles"]:
        if not isinstance(entry, dict) or set(entry) != {"id", "name", "size", "sha256"}:
            raise ValueError("Invalid update bundle entry")
        _size_and_hash(entry)
        bundle = entry["id"]
        if not isinstance(bundle, str) or bundle in bundles or entry["name"] != expected_names.get(bundle):
            raise ValueError("Invalid or duplicate update bundle name")
        bundles[bundle] = entry
    if set(bundles) != set(BUNDLES):
        raise ValueError("Update must contain all three bundles")
    management = {}
    for bundle, entry in bundles.items():
        asset = directory / entry["name"]
        if asset.stat().st_size != entry["size"] or digest(asset) != entry["sha256"]:
            raise ValueError(f"Update bundle size/hash mismatch: {entry['name']}")
        expected = {path for path, item in files.items() if item["bundle"] == bundle}
        with zipfile.ZipFile(asset) as archive:
            members = archive.infolist()
            _unique_paths(member.orig_filename for member in members)
            if {member.filename for member in members} != expected:
                raise ValueError(f"ZIP file inventory differs from manifest: {entry['name']}")
            for member in members:
                mode = member.external_attr >> 16
                if (member.is_dir() or member.flag_bits & 1
                        or member.compress_type not in (zipfile.ZIP_STORED, zipfile.ZIP_DEFLATED)
                        or member.external_attr & 0x400
                        or stat.S_IFMT(mode) not in (0, stat.S_IFREG)
                        or (member.file_size > 1024 * 1024
                            and member.file_size > max(1, member.compress_size) * 1000)
                        or member.file_size != files[member.filename]["size"]):
                    raise ValueError(f"Invalid update ZIP member: {member.filename}")
                with archive.open(member) as stream:
                    actual_hash = hashlib.file_digest(stream, "sha256").hexdigest()
                if actual_hash != files[member.filename]["sha256"]:
                    raise ValueError(f"Update file hash mismatch: {member.filename}")
                if member.filename in MANAGEMENT_FILES:
                    if member.file_size > 16 * 1024 * 1024:
                        raise ValueError("Managed-file marker/list exceeds the size limit")
                    management[member.filename] = archive.read(member)
    _managed_files(files, management)
    return assets


def validate_portable_archive(archive_path: Path, version: str, manifest_path: Path):
    """The complete installation and incremental assets must describe identical files."""
    manifest = json.loads(manifest_path.read_text(encoding="utf-8"), object_pairs_hook=_json_object)
    expected = {entry["path"]: entry for entry in manifest["files"]}
    top = f"Orbelle-{version}-win-x64/"
    prefix = top + f"versions/{version}/"
    with zipfile.ZipFile(archive_path) as archive:
        members = archive.infolist()
        _unique_paths((member.orig_filename for member in members), max_length=1024 + len(prefix))
        required = {top + name for name in
                    ("Orbelle.exe", "orbelle-layout.txt", "active-version.txt", ".orbelle-running.lock")}
        if not required.issubset(member.filename for member in members):
            raise ValueError("Missing portable launcher files")
        files = {}
        for member in members:
            if (not member.filename.startswith(top) or member.is_dir()
                    or stat.S_IFMT(member.external_attr >> 16) not in (0, stat.S_IFREG)
                    or member.external_attr & 0x400 or member.flag_bits & 1):
                raise ValueError("Invalid portable ZIP layout or file type")
            if member.filename.startswith(top + "versions/"):
                if not member.filename.startswith(prefix):
                    raise ValueError("Portable ZIP contains an unexpected version")
                files[member.filename[len(prefix):]] = member
        if set(files) != set(expected):
            raise ValueError("Portable and update file inventories differ")
        if (archive.read(top + "orbelle-layout.txt") != b"1\n"
                or archive.read(top + "active-version.txt") != (version + "\n").encode("ascii")
                or archive.getinfo(top + "Orbelle.exe").file_size == 0):
            raise ValueError("Portable launcher metadata differs from its release")
        for path, member in files.items():
            with archive.open(member) as stream:
                actual_hash = hashlib.file_digest(stream, "sha256").hexdigest()
            if member.file_size != expected[path]["size"] or actual_hash != expected[path]["sha256"]:
                raise ValueError(f"Portable and update file contents differ: {path}")
