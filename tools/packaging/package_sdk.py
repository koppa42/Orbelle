"""Bundle an installed headless SDK with its CMake/link dependencies."""
import argparse
from pathlib import Path
import re
import shutil
import tarfile
import zipfile


def copy_dependencies(prefix, output):
    info = prefix.parent / "vcpkg/info"
    if not info.is_dir():
        # The Linux delivery already contains only headless dependencies.
        shutil.copytree(prefix, output, symlinks=True)
        return
    for port in ("erfa", "cspice", "sqlite3", "nlohmann-json", "picosha2"):
        lists = list(info.glob(f"{port}_*_{prefix.name}.list"))
        if len(lists) != 1:
            raise ValueError(f"Expected one installed vcpkg file list for {port}")
        for entry in lists[0].read_text(encoding="utf-8").splitlines():
            relative = Path(entry).relative_to(prefix.name)
            if ".." in relative.parts:
                raise ValueError(f"Invalid dependency path: {entry}")
            source = prefix / relative
            if source.is_file():
                target = output / relative
                target.parent.mkdir(parents=True, exist_ok=True)
                shutil.copy2(source, target)


def package(sdk, dependencies, output, version, platform):
    if not re.fullmatch(r"(?:0|[1-9][0-9]*)\.(?:0|[1-9][0-9]*)\.(?:0|[1-9][0-9]*)", version):
        raise ValueError("Invalid SDK version")
    config = (sdk / "lib/cmake/Orbelle/OrbelleConfigVersion.cmake").read_text(encoding="utf-8")
    if not re.search(r'set\(PACKAGE_VERSION "' + re.escape(version) + r'"\)', config):
        raise ValueError("Installed SDK version differs from release version")
    output.mkdir(parents=True, exist_ok=True)
    name = f"Orbelle-{version}-sdk-{platform}"
    staging = output.parent / (name + "-staging")
    root = staging / name
    archive = output / (name + (".zip" if platform == "win-x64" else ".tar.gz"))
    if staging.exists() or archive.exists():
        raise ValueError("Use fresh SDK staging and archive paths")
    shutil.copytree(sdk, root)
    copy_dependencies(dependencies, root / "dependencies")
    (root / "README.txt").write_text(
        f"Orbelle {version} C++ SDK ({platform}, Release)\n\n"
        "Requires CMake 3.28+ and a compatible C++20 compiler.\n"
        "Windows: Visual Studio 2022 x64 (v143); Linux: Ubuntu 24.04 x86_64.\n"
        "This is a headless SDK with Core, Results, Application, Runtime, ERFA/IERS, CSPICE and SQLite.\n"
        "Set CMAKE_PREFIX_PATH to both this folder and its dependencies subfolder.\n"
        "Use find_package(Orbelle CONFIG REQUIRED COMPONENTS Runtime), then link Orbelle::Runtime.\n"
        "On Windows, add dependencies/bin to PATH when running a linked executable.\n"
        "The supplied Release libraries require a compatible compiler and runtime; no stable ABI is promised.\n"
        "Keep the dependency licenses and share/Orbelle/notices when redistributing.\n"
        "Documentation: https://github.com/koppa42/OrbitForge/tree/" + f"v{version}/docs\n",
        encoding="utf-8")
    if platform == "win-x64":
        with zipfile.ZipFile(archive, "x", zipfile.ZIP_DEFLATED) as bundle:
            for path in sorted(root.rglob("*")):
                if path.is_file():
                    bundle.write(path, path.relative_to(staging).as_posix())
    else:
        with tarfile.open(archive, "x:gz") as bundle:
            bundle.add(root, arcname=name)
    return root, archive


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--sdk", type=Path, required=True)
    parser.add_argument("--dependencies", type=Path, required=True)
    parser.add_argument("--output", type=Path, required=True)
    parser.add_argument("--version", required=True)
    parser.add_argument("--platform", choices=("win-x64", "linux-x64"), required=True)
    args = parser.parse_args()
    _, archive = package(args.sdk, args.dependencies, args.output, args.version, args.platform)
    print(archive)


if __name__ == "__main__":
    main()
