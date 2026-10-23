"""Test desktop and Python; build and validate distribution packages only for releases."""
import argparse
from datetime import datetime, timezone
import json
import os
from pathlib import Path
import shutil
import subprocess
import sys
import time
import tomllib
import zipfile

from version import check
from validation_inputs import PYTHON_EXAMPLES, stage_validation_inputs


def copy_desktop_assets(work, output, version):
    """Stage both the complete installation and all incremental update assets."""
    for suffix in (".zip", "-app.zip", "-runtime.zip", "-resources.zip", "-update.json"):
        shutil.copy2(work / f"Orbelle-{version}-win-x64{suffix}", output)


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--dependency-prefix", type=Path, required=True)
    parser.add_argument("--work", type=Path, required=True)
    parser.add_argument("--reports", type=Path, required=True)
    parser.add_argument("--release", action="store_true")
    parser.add_argument("--skip-python", action="store_true",
                        help="Run native tests only; Python is validated by a separate CI job")
    parser.add_argument("--output", type=Path)
    parser.add_argument("--test-dependencies", type=Path)
    parser.add_argument("--version", required=True)
    parser.add_argument("--generator", default=os.environ.get("CMAKE_GENERATOR", "Visual Studio 17 2022"))
    args = parser.parse_args()
    if args.release != bool(args.output):
        parser.error("--release and --output must be supplied together")
    if args.release and args.skip_python:
        parser.error("--skip-python cannot be used with --release")
    if os.name != "nt":
        parser.error("Windows is required")
    source = Path(__file__).resolve().parents[2]
    check(source, "v" + args.version)
    prefix = args.dependency_prefix.resolve(strict=True)
    work, reports = (p.resolve() for p in (args.work, args.reports))
    output = args.output.resolve() if args.output else None
    for path in (work, reports) + ((output,) if output else ()):
        path.mkdir(parents=True, exist_ok=False)
    if args.generator != "Ninja Multi-Config" and not args.generator.startswith("Visual Studio "):
        parser.error("Use a Visual Studio or Ninja Multi-Config generator (Release output layout)")
    architecture = ["-A", "x64"] if args.generator.startswith("Visual Studio ") else []
    environment = dict(os.environ, CMAKE_PREFIX_PATH=str(prefix), CMAKE_GENERATOR=args.generator,
                       PYTHONDONTWRITEBYTECODE="1")
    if architecture:
        environment["CMAKE_GENERATOR_PLATFORM"] = "x64"
    else:
        environment.pop("CMAKE_GENERATOR_PLATFORM", None)
    environment["PATH"] = str(prefix / "bin") + os.pathsep + environment["PATH"]
    for key in ("PYTHONPATH", "PYTHONHOME", "QT_PLUGIN_PATH", "QT_QPA_PLATFORM_PLUGIN_PATH"):
        environment.pop(key, None)
    commands = []

    def run(name, *command, env=None):
        command = list(map(str, command))
        print(f"[{name}] {command}", flush=True)
        started = datetime.now(timezone.utc).isoformat()
        start = time.monotonic()
        with (reports / f"{name}.log").open("w", encoding="utf-8") as log:
            result = subprocess.run(command, cwd=work, env=env or environment,
                                    stdout=log, stderr=subprocess.STDOUT)
        commands.append({"name": name, "command": command, "exit_code": result.returncode,
                         "started_utc": started, "elapsed_seconds": time.monotonic() - start})
        (reports / "commands.json").write_text(json.dumps(commands, indent=2), encoding="utf-8")
        if result.returncode:
            print((reports / f"{name}.log").read_text(encoding="utf-8", errors="replace")[-16000:])
            result.check_returncode()

    desktop = work / "desktop"
    run("desktop-configure", "cmake", "--preset", "desktop-graphics", "-S", source, "-B", desktop,
        "-G", args.generator, *architecture, f"-DCMAKE_PREFIX_PATH={prefix}",
        "-DCMAKE_MSVC_DEBUG_INFORMATION_FORMAT=$<$<CONFIG:Debug,RelWithDebInfo>:Embedded>")
    run("desktop-build", "cmake", "--build", desktop, "--config", "Release")
    run("desktop-tests", "ctest", "--test-dir", desktop, "-C", "Release", "--output-on-failure",
        "--no-tests=error", "--output-junit", reports / "desktop-tests.xml")
    if args.test_dependencies:
        run("export-python-dependencies", sys.executable, source / "tools/ci/export_python_dependencies.py",
            "--dependency-prefix", prefix, "--output", args.test_dependencies.resolve())
    if not args.release:
        if not args.skip_python:
            run("python-api", sys.executable, source / "tools/ci/check_python.py",
                "--dependency-prefix", prefix, "--work", work / "python", "--reports", reports / "python",
                "--cpp-example", desktop / "examples/cpp/Release/orbelle_example.exe")
        (reports / "passed.json").write_text(json.dumps({
            "passed": True, "version": args.version, "release": False,
        }), encoding="utf-8")
        return
    # Ordinary hosted runners do not provide the Vulkan GPU needed by --verify.
    run("desktop-package", sys.executable, source / "tools/packaging/package_windows.py",
        "--build-dir", desktop, "--dependency-prefix", prefix,
        "--output", work / f"Orbelle-{args.version}-win-x64",
        "--reports", reports / "desktop-package", "--version", args.version, "--zip")
    copy_desktop_assets(work, output, args.version)

    # Copy tracked sources only: scikit-build must not walk vcpkg/build caches.
    wheel_source = work / "wheel-source"
    tracked = subprocess.check_output(["git", "-C", str(source), "ls-files", "-z"]).decode("utf-8")
    for relative in filter(None, tracked.split("\0")):
        target = wheel_source / relative
        target.parent.mkdir(parents=True, exist_ok=True)
        shutil.copy2(source / relative, target)
    builder = work / "builder/Scripts/python.exe"
    run("builder-venv", sys.executable, "-m", "venv", work / "builder")
    requires = tomllib.loads((source / "pyproject.toml").read_text(encoding="utf-8"))["build-system"]["requires"]
    run("builder-dependencies", builder, "-m", "pip", "install", *requires)
    wheel_build = work / "wheel-build"
    run("wheel-build", builder, "-m", "pip", "wheel", wheel_source, "--no-deps", "--no-build-isolation",
        "--no-index", "--wheel-dir", output, f"--config-settings=build-dir={wheel_build}",
        "--config-settings=cmake.define.ORBELLE_INSTALL_SDK=ON",
        "--config-settings=cmake.define.CMAKE_MSVC_DEBUG_INFORMATION_FORMAT=$<$<CONFIG:Debug,RelWithDebInfo>:Embedded>")
    sdk = work / "sdk"
    run("sdk-install", "cmake", "--install", wheel_build, "--config", "Release", "--component", "SDK",
        "--prefix", sdk)
    run("sdk-package", sys.executable, source / "tools/packaging/package_sdk.py", "--sdk", sdk,
        "--dependencies", prefix, "--output", output, "--version", args.version, "--platform", "win-x64")

    # Validate the archive after relocation; no development DLL paths in this phase.
    relocated = work / "consumer"
    relocated.mkdir()
    with zipfile.ZipFile(output / f"Orbelle-{args.version}-sdk-win-x64.zip") as archive:
        archive.extractall(relocated)
    installed = relocated / f"Orbelle-{args.version}-sdk-win-x64"
    clean = dict(environment)
    clean["PATH"] = os.environ["PATH"]
    clean.pop("CMAKE_PREFIX_PATH", None)
    clean["ORBELLE_DATA_HOME"] = str(work / "user-data")
    sdk_environment = dict(clean, PATH=str(installed / "dependencies/bin") + os.pathsep + clean["PATH"])
    stage_validation_inputs(source, relocated)
    run("sdk-consumer-configure", "cmake", "-S", relocated / "tests/packaging/sdk_consumer",
        "-B", work / "sdk-consumer", "-G", args.generator, *architecture,
        f"-DCMAKE_PREFIX_PATH={installed};{installed / 'dependencies'}",
        "-DCMAKE_MSVC_DEBUG_INFORMATION_FORMAT=$<$<CONFIG:Debug,RelWithDebInfo>:Embedded>",
        "-DORBELLE_EXPECT_TIME_FRAMES=ON", "-DORBELLE_EXPECT_SPICE=ON",
        f"-DORBELLE_EOP_MANIFEST={installed / 'share/Orbelle/data/eop/iers_c04_2016_12_28_2017_01_04/manifest.json'}",
        f"-DORBELLE_SPICE_MANIFEST={installed / 'share/Orbelle/data/spice/de440s_2016_leap/manifest.json'}",
        env=sdk_environment)
    run("sdk-consumer-build", "cmake", "--build", work / "sdk-consumer", "--config", "Release", env=sdk_environment)
    run("sdk-consumer-tests", "ctest", "--test-dir", work / "sdk-consumer", "-C", "Release",
        "--output-on-failure", "--no-tests=error", "--output-junit", reports / "sdk-consumer-tests.xml",
        env=sdk_environment)
    wheels = list(output.glob("orbelle-*.whl"))
    if len(wheels) != 1:
        raise RuntimeError("Expected exactly one freshly built Windows wheel")
    python = work / "python-consumer/Scripts/python.exe"
    run("consumer-venv", sys.executable, "-m", "venv", work / "python-consumer", env=clean)
    run("wheel-install", python, "-m", "pip", "install", "--no-index", "--no-deps", wheels[0], env=clean)
    run("wheel-version", python, "-I", "-c",
        "import orbelle, sys; print(orbelle.__file__); assert orbelle.__version__ == sys.argv[1]",
        args.version, env=clean)
    run("python-tests", python, "-I", "-m", "unittest", "discover", "-s", relocated / "bindings/python/tests", "-v", env=clean)
    for name in PYTHON_EXAMPLES:
        run(f"example-{name}", python, "-I", relocated / f"examples/python/{name}.py", env=clean)
    run("cpp-python-parity", python, "-I", relocated / "compare_examples.py", "--cpp-example",
        desktop / "examples/cpp/Release/orbelle_example.exe", env=sdk_environment)
    (reports / "passed.json").write_text(json.dumps({
        "passed": True, "version": args.version, "release": True,
    }), encoding="utf-8")


if __name__ == "__main__":
    main()
