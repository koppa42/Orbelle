"""Build/test the desktop, then validate the packaged SDK and installed wheel."""
import argparse
import json
import os
from pathlib import Path
import shutil
import subprocess
import sys
import tomllib
import zipfile

from version import check


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--dependency-prefix", type=Path, required=True)
    parser.add_argument("--work", type=Path, required=True)
    parser.add_argument("--reports", type=Path, required=True)
    parser.add_argument("--output", type=Path, required=True)
    parser.add_argument("--version", required=True)
    parser.add_argument("--generator", default="Visual Studio 17 2022")
    args = parser.parse_args()
    if os.name != "nt":
        parser.error("Windows is required")
    source = Path(__file__).resolve().parents[2]
    check(source, "v" + args.version)
    prefix = args.dependency_prefix.resolve(strict=True)
    work, reports, output = (p.resolve() for p in (args.work, args.reports, args.output))
    for path in (work, reports, output):
        path.mkdir(parents=True, exist_ok=False)
    environment = dict(os.environ, CMAKE_PREFIX_PATH=str(prefix), CMAKE_GENERATOR=args.generator,
                       CMAKE_GENERATOR_PLATFORM="x64", PYTHONDONTWRITEBYTECODE="1")
    environment["PATH"] = str(prefix / "bin") + os.pathsep + environment["PATH"]
    for key in ("PYTHONPATH", "PYTHONHOME", "QT_PLUGIN_PATH", "QT_QPA_PLATFORM_PLUGIN_PATH"):
        environment.pop(key, None)
    commands = []

    def run(name, *command, env=None):
        command = list(map(str, command))
        print(f"[{name}] {command}", flush=True)
        with (reports / f"{name}.log").open("w", encoding="utf-8") as log:
            result = subprocess.run(command, cwd=work, env=env or environment,
                                    stdout=log, stderr=subprocess.STDOUT)
        commands.append({"name": name, "command": command, "exit_code": result.returncode})
        (reports / "commands.json").write_text(json.dumps(commands, indent=2), encoding="utf-8")
        if result.returncode:
            print((reports / f"{name}.log").read_text(encoding="utf-8", errors="replace")[-16000:])
            result.check_returncode()

    desktop = work / "desktop"
    run("desktop-configure", "cmake", "--preset", "desktop-graphics", "-S", source, "-B", desktop,
        "-G", args.generator, "-A", "x64", f"-DCMAKE_PREFIX_PATH={prefix}")
    run("desktop-build", "cmake", "--build", desktop, "--config", "Release")
    run("desktop-tests", "ctest", "--test-dir", desktop, "-C", "Release", "--output-on-failure",
        "--no-tests=error", "--output-junit", reports / "desktop-tests.xml")
    # Ordinary hosted runners do not provide the Vulkan GPU needed by --verify.
    run("desktop-package", sys.executable, source / "tools/packaging/package_windows.py",
        "--build-dir", desktop, "--dependency-prefix", prefix,
        "--output", work / f"OrbitForge-{args.version}-win-x64",
        "--reports", reports / "desktop-package", "--version", args.version, "--zip")
    shutil.copy2(work / f"OrbitForge-{args.version}-win-x64.zip", output)

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
        "--config-settings=cmake.define.ORBITFORGE_INSTALL_SDK=ON")
    sdk = work / "sdk"
    run("sdk-install", "cmake", "--install", wheel_build, "--config", "Release", "--component", "SDK",
        "--prefix", sdk)
    run("sdk-package", sys.executable, source / "tools/packaging/package_sdk.py", "--sdk", sdk,
        "--dependencies", prefix, "--output", output, "--version", args.version, "--platform", "win-x64")

    # Validate the archive after relocation; no development DLL paths in this phase.
    relocated = work / "consumer"
    relocated.mkdir()
    with zipfile.ZipFile(output / f"OrbitForge-{args.version}-sdk-win-x64.zip") as archive:
        archive.extractall(relocated)
    installed = relocated / f"OrbitForge-{args.version}-sdk-win-x64"
    clean = dict(environment)
    clean["PATH"] = os.environ["PATH"]
    clean.pop("CMAKE_PREFIX_PATH", None)
    clean["ORBITFORGE_DATA_HOME"] = str(work / "user-data")
    sdk_environment = dict(clean, PATH=str(installed / "dependencies/bin") + os.pathsep + clean["PATH"])
    for relative in ("tests/packaging/sdk_consumer", "bindings/python/tests", "tests/integration/fixtures",
                     "tests/reference/time_frames"):
        shutil.copytree(source / relative, relocated / relative)
    run("sdk-consumer-configure", "cmake", "-S", relocated / "tests/packaging/sdk_consumer",
        "-B", work / "sdk-consumer", "-G", args.generator, "-A", "x64",
        f"-DCMAKE_PREFIX_PATH={installed};{installed / 'dependencies'}",
        "-DORBITFORGE_EXPECT_TIME_FRAMES=ON", "-DORBITFORGE_EXPECT_SPICE=ON",
        f"-DORBITFORGE_EOP_MANIFEST={installed / 'share/OrbitForge/data/eop/iers_c04_2016_12_28_2017_01_04/manifest.json'}",
        f"-DORBITFORGE_SPICE_MANIFEST={installed / 'share/OrbitForge/data/spice/de440s_2016_leap/manifest.json'}",
        env=sdk_environment)
    run("sdk-consumer-build", "cmake", "--build", work / "sdk-consumer", "--config", "Release", env=sdk_environment)
    run("sdk-consumer-tests", "ctest", "--test-dir", work / "sdk-consumer", "-C", "Release",
        "--output-on-failure", "--no-tests=error", "--output-junit", reports / "sdk-consumer-tests.xml",
        env=sdk_environment)
    wheels = list(output.glob("orbitforge-*.whl"))
    if len(wheels) != 1:
        raise RuntimeError("Expected exactly one freshly built Windows wheel")
    python = work / "python-consumer/Scripts/python.exe"
    run("consumer-venv", sys.executable, "-m", "venv", work / "python-consumer", env=clean)
    run("wheel-install", python, "-m", "pip", "install", "--no-index", "--no-deps", wheels[0], env=clean)
    run("wheel-version", python, "-I", "-c",
        "import orbitforge, sys; print(orbitforge.__file__); assert orbitforge.__version__ == sys.argv[1]",
        args.version, env=clean)
    run("python-tests", python, "-I", "-m", "unittest", "discover", "-s", relocated / "bindings/python/tests", "-v", env=clean)
    (reports / "passed.json").write_text(json.dumps({"passed": True, "version": args.version}), encoding="utf-8")


if __name__ == "__main__":
    main()
