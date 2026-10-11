"""Build and test one interpreter's wheel using already compiled dependencies."""
import argparse
import json
import os
from pathlib import Path
import shutil
import subprocess
import sys
import tomllib

from version import ROOT, check, python_versions


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--dependency-prefix", type=Path, required=True)
    parser.add_argument("--work", type=Path, required=True)
    parser.add_argument("--reports", type=Path, required=True)
    parser.add_argument("--output", type=Path, required=True)
    args = parser.parse_args()
    version = check(ROOT)
    python_version = f"{sys.version_info.major}.{sys.version_info.minor}"
    if python_version not in python_versions()[1]:
        parser.error(f"Python {python_version} is not in the supported CI matrix")
    prefix = args.dependency_prefix.resolve(strict=True)
    work, reports, output = (path.resolve() for path in (args.work, args.reports, args.output))
    for path in (work, reports, output):
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
        with (reports / f"{name}.log").open("w", encoding="utf-8") as log:
            process = subprocess.run(command, cwd=work, env=env or environment,
                                     stdout=log, stderr=subprocess.STDOUT)
        records.append({"name": name, "command": command, "exit_code": process.returncode})
        (reports / "commands.json").write_text(json.dumps(records, indent=2), encoding="utf-8")
        if process.returncode:
            print((reports / f"{name}.log").read_text(encoding="utf-8", errors="replace")[-16000:])
            process.check_returncode()

    source = work / "source"
    tracked = subprocess.check_output(["git", "-C", str(ROOT), "ls-files", "-z"]).decode("utf-8")
    for relative in filter(None, tracked.split("\0")):
        target = source / relative
        target.parent.mkdir(parents=True, exist_ok=True)
        shutil.copy2(ROOT / relative, target)
    suffix = "Scripts/python.exe" if os.name == "nt" else "bin/python"
    builder = work / "builder" / suffix
    run("builder-venv", sys.executable, "-m", "venv", work / "builder")
    requirements = tomllib.loads((source / "pyproject.toml").read_text(encoding="utf-8"))["build-system"]["requires"]
    run("builder-dependencies", builder, "-m", "pip", "install", *requirements)
    run("wheel-build", builder, "-m", "pip", "wheel", source, "--no-deps", "--no-build-isolation",
        "--no-index", "--wheel-dir", output, f"--config-settings=build-dir={work / 'build'}")
    wheels = list(output.glob("orbitforge-*.whl"))
    tag = "cp" + python_version.replace(".", "")
    if len(wheels) != 1 or f"-{tag}-{tag}-" not in wheels[0].name:
        raise ValueError("Expected exactly one wheel for this interpreter")
    clean = dict(environment)
    clean.pop("CMAKE_PREFIX_PATH", None)
    clean["ORBITFORGE_DATA_HOME"] = str(work / "user-data")
    consumer = work / "consumer" / suffix
    run("consumer-venv", sys.executable, "-m", "venv", work / "consumer", env=clean)
    run("wheel-install", consumer, "-m", "pip", "install", "--no-index", "--no-deps", wheels[0], env=clean)
    run("wheel-version", consumer, "-I", "-c",
        "import orbitforge, sys; print(orbitforge.__file__); assert orbitforge.__version__ == sys.argv[1]",
        version, env=clean)
    run("python-tests", consumer, "-I", "-m", "unittest", "discover", "-s", source / "bindings/python/tests", "-v", env=clean)
    for name in ("circular_orbit", "time_frames", "eop_state", "sun_moon", "two_body_scenario", "propagators"):
        run(f"example-{name}", consumer, "-I", source / f"examples/python/{name}.py", env=clean)
    (reports / "passed.json").write_text(json.dumps({"passed": True, "python": python_version}), encoding="utf-8")


if __name__ == "__main__":
    main()
