"""Capture a headless computation run and its exact local inputs in a new directory."""

import argparse
from datetime import datetime, timezone
import hashlib
import json
import os
from pathlib import Path
import platform
import re
import shutil
import subprocess
import sys

from analyze_compute_benchmark import PROFILES, analyze


def sha256(path):
    digest = hashlib.sha256()
    with path.open("rb") as stream:
        for chunk in iter(lambda: stream.read(1024 * 1024), b""):
            digest.update(chunk)
    return digest.hexdigest()


def source_fingerprint(source):
    roots = ["CMakeLists.txt", "CMakePresets.json", ".gitattributes", "vcpkg.json",
             "vcpkg-configuration.json", "cmake", "modules", "benchmarks/compute",
             "data/samples/eop/iers_c04_2000_01_01_2000_01_02", "data/samples/time",
             "data/samples/spice/de440s_2016_leap", "data/manifests",
             "docs/validation/p0_compute_protocol.md", "tools/ci/check_linux.py"]
    roots += [f"tools/validation/{name}_compute_benchmark.py" for name in ("run", "analyze", "test")]
    result = {}
    for root in roots:
        path = source / root
        if not path.exists():
            raise FileNotFoundError(path)
        paths = [path] if path.is_file() else sorted(path.rglob("*"))
        for child in paths:
            if child.is_file() and "__pycache__" not in child.parts:
                result[child.relative_to(source).as_posix()] = sha256(child)
    return result


def runtime_fingerprint(executable, libraries):
    files = [executable, *sorted(executable.parent.glob("*.dll")), *libraries]
    return {str(path): sha256(path) for path in files}


def build_inputs(executable):
    cache = next((parent / "CMakeCache.txt" for parent in executable.parents
                  if (parent / "CMakeCache.txt").is_file()), None)
    if cache is None:
        raise ValueError("Run the executable from its CMake build tree to capture build flags")
    entries = {}
    for line in cache.read_text(encoding="utf-8").splitlines():
        if not line.startswith(("//", "#")) and ":" in line and "=" in line:
            key, value = line.split("=", 1)
            entries[key.split(":", 1)[0]] = value
    files = {"CMakeCache.txt": cache}
    for name in ("compile_commands.json", "benchmarks/compute/orbitforge_compute_benchmark.vcxproj"):
        path = cache.parent / name
        if path.is_file():
            files[path.name] = path
    for index, prefix in enumerate(entries.get("CMAKE_PREFIX_PATH", "").split(";")):
        status = Path(prefix).parent / "vcpkg/status"
        if prefix and status.is_file():
            files[f"dependency-status-{index}.txt"] = status
    return files


def command_output(command, cwd=None):
    completed = subprocess.run(command, cwd=cwd, text=True, capture_output=True, check=False)
    return {"command": [str(x) for x in command], "exit_code": completed.returncode,
            "stdout": completed.stdout, "stderr": completed.stderr}


def host_info():
    cpu = platform.processor()
    if sys.platform == "win32":
        import winreg
        with winreg.OpenKey(winreg.HKEY_LOCAL_MACHINE,
                           r"HARDWARE\DESCRIPTION\System\CentralProcessor\0") as key:
            cpu = winreg.QueryValueEx(key, "ProcessorNameString")[0].strip()
    elif Path("/proc/cpuinfo").exists():
        for line in Path("/proc/cpuinfo").read_text().splitlines():
            if line.startswith("model name"):
                cpu = line.split(":", 1)[1].strip()
                break
    return {"platform": platform.platform(), "machine": platform.machine(), "cpu": cpu,
            "logical_cpus": os.cpu_count(), "python": platform.python_version()}


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--executable", required=True, type=Path)
    parser.add_argument("--source", type=Path, default=Path(__file__).resolve().parents[2])
    parser.add_argument("--output", required=True, type=Path)
    parser.add_argument("--profile", choices=PROFILES, default="baseline")
    parser.add_argument("--timeout", type=float, default=600)
    args = parser.parse_args()
    source, executable, output = args.source.resolve(), args.executable.resolve(), args.output.resolve()
    if output.exists():
        parser.error("Use a new output directory; existing evidence will not be overwritten")
    output.mkdir(parents=True)
    capture = {"schema": 1, "valid": False, "profile": args.profile,
               "started_utc": datetime.now(timezone.utc).isoformat(), "source": str(source),
               "executable": str(executable)}
    summary = {"valid": False}
    try:
        capture["host"] = host_info()
        capture["git_head"] = command_output(["git", "rev-parse", "HEAD"], source)
        capture["git_status"] = command_output(["git", "status", "--porcelain=v1"], source)
        capture["source_sha256"] = source_fingerprint(source)
        libraries = []
        if sys.platform.startswith("linux"):
            capture["ldd"] = command_output(["ldd", executable])
            if capture["ldd"]["exit_code"] or "not found" in capture["ldd"]["stdout"]:
                raise ValueError("Cannot resolve Linux runtime libraries")
            libraries = sorted({Path(path) for path in re.findall(r"(?:=>\s*|^\s*)(/\S+)",
                               capture["ldd"]["stdout"], flags=re.MULTILINE)})
        capture["runtime_sha256"] = runtime_fingerprint(executable, libraries)
        build_files = build_inputs(executable)
        capture["build_input_sha256"] = {str(path): sha256(path) for path in build_files.values()}
        (output / "build-inputs").mkdir()
        for name, path in build_files.items():
            shutil.copyfile(path, output / "build-inputs" / name)
        command = [str(executable), "--profile", args.profile,
                   "--data-root", str(source / "data/samples"),
                   "--workspace", str(output / "spice-workspace"),
                   "--output", str(output / "raw.json")]
        capture["command"] = command
        with (output / "stdout.log").open("w", encoding="utf-8") as stdout, \
                (output / "stderr.log").open("w", encoding="utf-8") as stderr:
            completed = subprocess.run(command, stdout=stdout, stderr=stderr, timeout=args.timeout,
                                       check=False)
        capture["exit_code"] = completed.returncode
        capture["source_unchanged"] = capture["source_sha256"] == source_fingerprint(source)
        capture["runtime_unchanged"] = capture["runtime_sha256"] == runtime_fingerprint(executable, libraries)
        capture["build_inputs_unchanged"] = capture["build_input_sha256"] == {
            str(path): sha256(path) for path in build_files.values()}
        if (completed.returncode or not capture["source_unchanged"] or
                not capture["runtime_unchanged"] or not capture["build_inputs_unchanged"]):
            raise ValueError("Process failed or measured inputs changed during the run")
        raw = output / "raw.json"
        summary = analyze(json.loads(raw.read_text(encoding="utf-8")), args.profile)
        capture["raw_sha256"] = sha256(raw)
        capture["valid"] = True
    except (ValueError, KeyError, TypeError, OSError, subprocess.SubprocessError) as error:
        capture["error"] = str(error)
        summary = {"valid": False, "error": str(error)}
    finally:
        capture["finished_utc"] = datetime.now(timezone.utc).isoformat()
        for name, value in (("capture.json", capture), ("summary.json", summary)):
            (output / name).write_text(json.dumps(value, indent=2, allow_nan=False) + "\n",
                                      encoding="utf-8")
    print(json.dumps({"valid": capture["valid"], "profile": args.profile, "output": str(output)}))
    return 0 if capture["valid"] else 1


if __name__ == "__main__":
    raise SystemExit(main())
