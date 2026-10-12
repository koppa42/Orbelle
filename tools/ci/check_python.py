"""Test one interpreter's native API; build and install wheels only in release mode."""
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

from version import ROOT, check, python_versions


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--dependency-prefix", type=Path, required=True)
    parser.add_argument("--work", type=Path, required=True)
    parser.add_argument("--reports", type=Path, required=True)
    parser.add_argument("--release", action="store_true")
    parser.add_argument("--output", type=Path)
    parser.add_argument("--cpp-example", type=Path)
    args = parser.parse_args()
    if args.release != bool(args.output):
        parser.error("--release and --output must be supplied together")
    version = check(ROOT)
    python_version = f"{sys.version_info.major}.{sys.version_info.minor}"
    if python_version not in python_versions()[1]:
        parser.error(f"Python {python_version} is not in the supported CI matrix")
    prefix = args.dependency_prefix.resolve(strict=True)
    work, reports = (path.resolve() for path in (args.work, args.reports))
    output = args.output.resolve() if args.output else None
    for path in (work, reports) + ((output,) if output else ()):
        path.mkdir(parents=True, exist_ok=False)
    environment = dict(os.environ, CMAKE_PREFIX_PATH=str(prefix), PYTHONDONTWRITEBYTECODE="1")
    for name in ("PYTHONPATH", "PYTHONHOME", "LD_LIBRARY_PATH"):
        environment.pop(name, None)
    if os.name == "nt":
        environment.setdefault("CMAKE_GENERATOR", "Visual Studio 17 2022")
        environment["CMAKE_GENERATOR_PLATFORM"] = "x64"
    records = []

    def run(name, *command, env=None):
        command = list(map(str, command))
        print(f"[{name}] {command}", flush=True)
        started = datetime.now(timezone.utc).isoformat()
        start = time.monotonic()
        with (reports / f"{name}.log").open("w", encoding="utf-8") as log:
            process = subprocess.run(command, cwd=work, env=env or environment,
                                     stdout=log, stderr=subprocess.STDOUT)
        records.append({"name": name, "command": command, "exit_code": process.returncode,
                        "started_utc": started, "elapsed_seconds": time.monotonic() - start})
        (reports / "commands.json").write_text(json.dumps(records, indent=2), encoding="utf-8")
        if process.returncode:
            print((reports / f"{name}.log").read_text(encoding="utf-8", errors="replace")[-16000:])
            process.check_returncode()

    source = ROOT
    if args.release:
        # Only the wheel backend walks the source tree; exclude local caches from it.
        source = work / "source"
        tracked = subprocess.check_output(["git", "-C", str(ROOT), "ls-files", "-z"]).decode("utf-8")
        for relative in filter(None, tracked.split("\0")):
            target = source / relative
            target.parent.mkdir(parents=True, exist_ok=True)
            shutil.copy2(ROOT / relative, target)
    suffix = "Scripts/python.exe" if os.name == "nt" else "bin/python"
    builder = work / "builder" / suffix
    run("builder-venv", sys.executable, "-m", "venv", work / "builder")
    config = tomllib.loads((source / "pyproject.toml").read_text(encoding="utf-8"))
    run("builder-dependencies", builder, "-m", "pip", "install", *config["build-system"]["requires"])
    clean = dict(environment)
    clean.pop("CMAKE_PREFIX_PATH", None)
    clean["ORBITFORGE_DATA_HOME"] = str(work / "user-data")
    consumer = work / "consumer" / suffix
    run("consumer-venv", sys.executable, "-m", "venv", work / "consumer", env=clean)
    if args.release:
        run("wheel-build", builder, "-m", "pip", "wheel", source, "--no-deps", "--no-build-isolation",
            "--no-index", "--wheel-dir", output, f"--config-settings=build-dir={work / 'build'}")
        wheels = list(output.glob("orbitforge-*.whl"))
        tag = "cp" + python_version.replace(".", "")
        if len(wheels) != 1 or f"-{tag}-{tag}-" not in wheels[0].name:
            raise ValueError("Expected exactly one wheel for this interpreter")
        run("wheel-install", consumer, "-m", "pip", "install", "--no-index", "--no-deps", wheels[0], env=clean)
    else:
        # Compile the same feature set as the wheel without invoking a package backend.
        pybind_dir = subprocess.check_output(
            [str(builder), "-I", "-m", "pybind11", "--cmakedir"], env=environment, text=True).strip()
        definitions = config["tool"]["scikit-build"]["cmake"]["define"]
        build = work / "build"
        run("python-configure", "cmake", "-S", source, "-B", build,
            "-DCMAKE_BUILD_TYPE=Release", f"-DPython_EXECUTABLE={builder}",
            f"-Dpybind11_DIR={pybind_dir}", f"-DCMAKE_PREFIX_PATH={prefix}",
            *(f"-D{name}={value}" for name, value in definitions.items()))
        run("python-build", "cmake", "--build", build, "--config", "Release", "--target", "_native")
        site_packages = Path(subprocess.check_output(
            [str(consumer), "-I", "-c", "import sysconfig; print(sysconfig.get_path('purelib'))"],
            env=clean, text=True).strip())
        run("python-stage", "cmake", "--install", build, "--config", "Release",
            "--component", "Python", "--prefix", site_packages)
        shutil.copytree(source / "bindings/python/src/orbitforge", site_packages / "orbitforge",
                        dirs_exist_ok=True, ignore=shutil.ignore_patterns("__pycache__", "*.pyc"))
    run("python-version", consumer, "-I", "-c",
        "import orbitforge, sys; print(orbitforge.__file__); assert orbitforge.__version__ == sys.argv[1]",
        version, env=clean)
    run("python-tests", consumer, "-I", "-m", "unittest", "discover", "-s", source / "bindings/python/tests", "-v", env=clean)
    for name in ("circular_orbit", "time_frames", "eop_state", "sun_moon", "two_body_scenario", "propagators"):
        run(f"example-{name}", consumer, "-I", source / f"examples/python/{name}.py", env=clean)
    if args.cpp_example:
        run("cpp-python-parity", consumer, "-I", source / "tools/validation/compare_examples.py",
            "--cpp-example", args.cpp_example.resolve(), env=clean)
    (reports / "passed.json").write_text(json.dumps({
        "passed": True, "python": python_version, "release": args.release,
    }), encoding="utf-8")


if __name__ == "__main__":
    main()
