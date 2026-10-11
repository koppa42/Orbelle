"""Build Linux headless artifacts, then validate them without the source tree.

The build stages need a pinned vcpkg checkout and normal Linux development tools.
The consumer stage needs only the exported delivery directory and those tools;
run it in a fresh, network-disabled container with no source/build mounts.
"""

import argparse
from datetime import datetime, timezone
import hashlib
import json
import os
from pathlib import Path
import platform
import shutil
import subprocess
import sys
import time
import tomllib
import uuid


def sha256(path):
    with path.open("rb") as stream:
        return hashlib.file_digest(stream, "sha256").hexdigest()


class Check:
    def __init__(self, args):
        self.args = args
        self.source = args.source.resolve() if args.source else None
        self.work = args.work.resolve()
        self.reports = args.reports.resolve()
        self.delivery = args.delivery.resolve() if args.delivery else self.work / "delivery"
        self.prefix = self.work / "dependencies/installed/x64-linux"
        self.work.mkdir(parents=True, exist_ok=True)
        self.reports.mkdir(parents=True, exist_ok=True)
        if (self.reports / "commands.json").exists():
            raise RuntimeError("Use a new reports directory; previous evidence is preserved")
        self.commands = []
        self.saved_artifacts = set()
        self.env = dict(os.environ, CMAKE_BUILD_PARALLEL_LEVEL=str(args.jobs),
                        VCPKG_MAX_CONCURRENCY=str(args.jobs), PYTHONDONTWRITEBYTECODE="1")
        # A developer's Python/library search path must not make packaging tests pass.
        for name in ("PYTHONPATH", "PYTHONHOME", "LD_LIBRARY_PATH", "CMAKE_PREFIX_PATH"):
            self.env.pop(name, None)

    def run(self, name, *command, cwd=None, extra_env=None, echo=True):
        command = [str(arg) for arg in command]
        record = {"name": name, "command": command, "cwd": str(cwd or self.work),
                  "started_utc": datetime.now(timezone.utc).isoformat(),
                  "environment_overrides": extra_env or {}}
        print(f"\n[{name}] {command}", flush=True)
        start = time.monotonic()
        with (self.reports / f"{name}.log").open("w", encoding="utf-8") as log:
            with subprocess.Popen(command, cwd=cwd or self.work,
                                  env=self.env | (extra_env or {}), stdout=subprocess.PIPE,
                                  stderr=subprocess.STDOUT, text=True, errors="replace") as proc:
                for line in proc.stdout:
                    log.write(line)
                    if echo:
                        print(line, end="", flush=True)
                code = proc.wait()
        record.update(exit_code=code, elapsed_seconds=time.monotonic() - start)
        self.commands.append(record)
        (self.reports / "commands.json").write_text(
            json.dumps(self.commands, indent=2) + "\n", encoding="utf-8")
        if code:
            raise subprocess.CalledProcessError(code, command)

    def environment(self):
        (self.reports / "environment.json").write_text(json.dumps({
            "platform": platform.platform(), "machine": platform.machine(),
            "python": sys.version, "os_release": platform.freedesktop_os_release(),
            "source": str(self.source), "work": str(self.work),
        }, indent=2) + "\n", encoding="utf-8")
        self.run("cmake-version", "cmake", "--version")
        self.run("gcc-version", "g++", "--version")
        self.run("clang-version", "clang++", "--version")
        self.run("ninja-version", "ninja", "--version")
        if shutil.which("dpkg-query"):
            self.run("os-packages", "dpkg-query", "-W", "-f=${binary:Package}\t${Version}\n", echo=False)
        if self.source:
            self.source_hashes = self.fingerprint()
            (self.reports / "source-sha256.json").write_text(
                json.dumps(self.source_hashes, indent=2) + "\n", encoding="utf-8")

    def fingerprint(self):
        roots = ("README.md", "CMakeLists.txt", "CMakePresets.json", "vcpkg.json",
                 "vcpkg-configuration.json", "pyproject.toml", "cmake", "modules",
                 "bindings", "tests", "data", "examples", "tools", "benchmarks/compute",
                 "docs/validation/p0_compute_protocol.md")
        files = []
        for root in roots:
            path = self.source / root
            files.extend([path] if path.is_file() else path.rglob("*"))
        return {path.relative_to(self.source).as_posix(): sha256(path)
                for path in sorted(files) if path.is_file() and "__pycache__" not in path.parts}

    def verify_source(self):
        if self.source and self.fingerprint() != self.source_hashes:
            for name in self.saved_artifacts:
                self.clear_artifact(name)
            raise RuntimeError("Source changed during validation; rerun with a new report directory")

    def dependencies(self):
        vcpkg_root = Path(self.args.vcpkg_root).resolve()
        baseline = json.loads((self.source / "vcpkg.json").read_text())["builtin-baseline"]
        actual = subprocess.check_output(
            ["git", "-C", str(vcpkg_root), "rev-parse", "HEAD"], text=True).strip()
        if actual != baseline:
            raise RuntimeError(f"vcpkg checkout {actual} differs from baseline {baseline}")
        self.run("vcpkg-version", vcpkg_root / "vcpkg", "version")
        self.run("vcpkg-revision", "git", "-C", vcpkg_root, "rev-parse", "HEAD")
        deps = self.work / "dependencies"
        for directory in ("installed", "buildtrees", "packages", "downloads", "binary-cache"):
            (deps / directory).mkdir(parents=True, exist_ok=True)
        self.run("dependencies", vcpkg_root / "vcpkg", "install",
                 f"--x-manifest-root={self.source}", f"--x-install-root={deps / 'installed'}",
                 f"--x-buildtrees-root={deps / 'buildtrees'}",
                 f"--x-packages-root={deps / 'packages'}", f"--downloads-root={deps / 'downloads'}",
                 "--triplet=x64-linux", "--x-no-default-features", "--x-feature=tests",
                 "--x-feature=time-frames", "--x-feature=spice", "--x-feature=data-catalog",
                 extra_env={"VCPKG_BINARY_SOURCES": f"clear;files,{deps / 'binary-cache'},readwrite"})
        shutil.copy2(deps / "installed/vcpkg/status", self.reports / "vcpkg-status.txt")

    def build(self, sanitizer=False):
        name = "sanitizers" if sanitizer else "release"
        build = self.work / "build" / name
        # A fresh staging prefix prevents a previous install from hiding missing files.
        prefix = self.work / "install" / f"{name}-{uuid.uuid4().hex}"
        if not sanitizer:
            self.clear_artifact("release")
        flags = {"ASAN_OPTIONS": "detect_leaks=1:halt_on_error=1",
                 "UBSAN_OPTIONS": "halt_on_error=1:print_stacktrace=1"} if sanitizer else {}
        self.run(f"{name}-configure", "cmake", "--preset",
                 "linux-sanitizers" if sanitizer else "linux-astronomy",
                 "-S", self.source, "-B", build, f"-DCMAKE_PREFIX_PATH={self.prefix}",
                 f"-DCMAKE_INSTALL_PREFIX={prefix}", cwd=self.source, extra_env=flags)
        self.run(f"{name}-build", "cmake", "--build", build, extra_env=flags)
        self.run(f"{name}-tests", "ctest", "--test-dir", build, "--output-on-failure",
                 "--no-tests=error", "--output-junit", self.reports / f"{name}-tests.xml",
                 extra_env=flags)
        shutil.copy2(build / "CMakeCache.txt", self.reports / f"{name}-CMakeCache.txt")
        shutil.copy2(build / "compile_commands.json", self.reports / f"{name}-compile_commands.json")
        if not sanitizer:
            self.run("sdk-install", "cmake", "--install", build, "--component", "SDK")
            self.run("cpp-example", build / "examples/cpp/orbitforge_example")
            self.save_artifact("release", prefix)

    def compute(self):
        """Validate portability and instrumented smoke workloads, not Linux performance."""
        for sanitizer in (False, True):
            name = "compute-sanitizers" if sanitizer else "compute-release"
            build = self.work / "build" / name
            flags = {"ASAN_OPTIONS": "detect_leaks=1:halt_on_error=1",
                     "UBSAN_OPTIONS": "halt_on_error=1:print_stacktrace=1"} if sanitizer else {}
            self.run(f"{name}-configure", "cmake", "--preset", "compute-benchmark", "-G", "Ninja",
                     "-S", self.source, "-B", build, f"-DCMAKE_PREFIX_PATH={self.prefix}",
                     f"-DCMAKE_BUILD_TYPE={'RelWithDebInfo' if sanitizer else 'Release'}",
                     f"-DORBITFORGE_ENABLE_SANITIZERS={'ON' if sanitizer else 'OFF'}",
                     cwd=self.source, extra_env=flags)
            self.run(f"{name}-build", "cmake", "--build", build, extra_env=flags)
            self.run(f"{name}-tests", "ctest", "--test-dir", build, "--output-on-failure",
                     "--no-tests=error", "--output-junit", self.reports / f"{name}-tests.xml",
                     extra_env=flags)
            self.run(f"{name}-smoke", sys.executable,
                     self.source / "tools/validation/run_compute_benchmark.py",
                     "--executable", build / "benchmarks/compute/orbitforge_compute_benchmark",
                     "--source", self.source, "--profile", "smoke",
                     "--output", self.reports / f"{name}-smoke", extra_env=flags)

    def clear_artifact(self, name):
        directory = self.work / "artifacts"
        directory.mkdir(exist_ok=True)
        (directory / f"{name}.json").unlink(missing_ok=True)

    def save_artifact(self, name, path):
        self.verify_source()
        (self.work / f"artifacts/{name}.json").write_text(json.dumps({
            "path": str(path), "source_sha256": self.source_hashes,
        }, indent=2) + "\n", encoding="utf-8")
        self.saved_artifacts.add(name)

    def load_artifact(self, name):
        record = json.loads((self.work / f"artifacts/{name}.json").read_text())
        if record["source_sha256"] != self.source_hashes:
            raise RuntimeError(f"Source changed since the {name} artifact was built; rebuild it")
        return Path(record["path"])

    def wheel(self):
        self.clear_artifact("wheel")
        identity = uuid.uuid4().hex
        dist = self.work / "dist" / identity
        source = self.work / "wheel-sources" / identity
        # scikit-build-core discovers nested .gitignore files below its source root.
        # Copy the fingerprinted inputs so a large local out/ cannot enter that walk.
        self.verify_source()
        for relative, expected in self.source_hashes.items():
            destination = source / relative
            destination.parent.mkdir(parents=True, exist_ok=True)
            shutil.copy2(self.source / relative, destination)
            if sha256(destination) != expected:
                raise RuntimeError(f"Source changed while copying wheel input: {relative}")
        self.verify_source()
        venv = self.work / "venvs/builder"
        self.run("builder-venv", sys.executable, "-m", "venv", venv)
        python = venv / "bin/python"
        config = tomllib.loads((self.source / "pyproject.toml").read_text())
        self.run("python-build-dependencies", python, "-m", "pip", "install",
                 *config["build-system"]["requires"])
        self.run("python-build-versions", python, "-m", "pip", "freeze", "--all")
        self.run("wheel-build", python, "-m", "pip", "wheel", source, "--verbose", "--no-deps",
                 "--no-build-isolation", "--no-index", "--wheel-dir", dist,
                 f"--config-settings=build-dir={self.work / 'build/wheel' / identity}",
                 extra_env={"CMAKE_PREFIX_PATH": str(self.prefix)})
        if len(list(dist.glob("orbitforge-*.whl"))) != 1:
            raise RuntimeError("Expected exactly one freshly built OrbitForge wheel")
        self.save_artifact("wheel", dist)

    def export(self):
        self.verify_source()
        if self.delivery.exists():
            raise RuntimeError("Use a new delivery directory; previous artifacts are preserved")
        sdk = self.load_artifact("release")
        wheels = self.load_artifact("wheel")
        self.delivery.mkdir(parents=True)
        shutil.copytree(sdk, self.delivery / "sdk")
        shutil.copytree(self.prefix, self.delivery / "dependencies", symlinks=True)
        shutil.copytree(wheels, self.delivery / "wheels")
        # Only packaging tests, frozen expected values and examples are delivered here.
        # Product headers/libraries come from the install trees above.
        for directory in ("tests/packaging/sdk_consumer", "tests/reference/time_frames",
                          "tests/integration/fixtures", "bindings/python/tests", "examples/python"):
            shutil.copytree(self.source / directory, self.delivery / directory)
        shutil.copy2(self.source / "tools/validation/compare_examples.py", self.delivery)
        shutil.copy2(self.source / "tools/ci/check_linux.py", self.delivery)
        shutil.copy2(self.source / "tools/packaging/package_sdk.py", self.delivery)
        shutil.copy2(self.reports / "source-sha256.json", self.delivery)
        (self.delivery / "bin").mkdir()
        shutil.copy2(self.work / "build/release/examples/cpp/orbitforge_example",
                     self.delivery / "bin/orbitforge_example")
        entries = []
        for path in sorted(self.delivery.rglob("*")):
            if path.is_file():
                entries.append({"path": path.relative_to(self.delivery).as_posix(),
                                "size": path.stat().st_size,
                                "sha256": sha256(path)})
        (self.delivery / "manifest.json").write_text(
            json.dumps(entries, indent=2) + "\n", encoding="utf-8")
        shutil.copy2(self.delivery / "manifest.json", self.reports / "delivery-manifest.json")
        print(f"Exported {len(entries)} delivery files to {self.delivery}", flush=True)

    def consumer(self):
        # Copy only the delivery into a fresh writable location. The input can be read-only.
        delivery = self.work / "relocated-delivery"
        shutil.copytree(self.delivery, delivery, symlinks=True)
        for entry in json.loads((delivery / "manifest.json").read_text()):
            path = delivery / entry["path"]
            digest = sha256(path)
            if digest != entry["sha256"] or path.stat().st_size != entry["size"]:
                raise RuntimeError(f"Delivery checksum mismatch: {entry['path']}")
        sdk = delivery / "sdk"
        build = self.work / "sdk-consumer"
        self.run("sdk-consumer-configure", "cmake", "-G", "Ninja", "-S",
                 delivery / "tests/packaging/sdk_consumer", "-B", build,
                 "-DCMAKE_BUILD_TYPE=Release",
                 f"-DCMAKE_PREFIX_PATH={sdk};{delivery / 'dependencies'}",
                 "-DORBITFORGE_EXPECT_TIME_FRAMES=ON", "-DORBITFORGE_EXPECT_SPICE=ON",
                 f"-DORBITFORGE_EOP_MANIFEST={sdk / 'share/OrbitForge/data/eop/iers_c04_2016_12_28_2017_01_04/manifest.json'}",
                 f"-DORBITFORGE_SPICE_MANIFEST={sdk / 'share/OrbitForge/data/spice/de440s_2016_leap/manifest.json'}")
        self.run("sdk-consumer-build", "cmake", "--build", build)
        self.run("sdk-consumer-tests", "ctest", "--test-dir", build, "--output-on-failure",
                 "--no-tests=error", "--output-junit", self.reports / "sdk-consumer-tests.xml")
        wheels = list((delivery / "wheels").glob("orbitforge-*.whl"))
        if len(wheels) != 1:
            raise RuntimeError("Expected exactly one OrbitForge wheel")
        venv = self.work / "venvs/consumer"
        self.run("consumer-venv", sys.executable, "-m", "venv", venv)
        python = venv / "bin/python"
        self.run("wheel-install", python, "-m", "pip", "install", "--no-index", "--no-deps", wheels[0])
        self.run("python-runtime-packages", python, "-m", "pip", "freeze", "--all")
        self.run("python-import-path", python, "-I", "-c",
                 "import orbitforge; print(orbitforge.__file__)")
        self.run("python-api-tests", python, "-I", "-m", "unittest", "discover", "-v",
                 "-s", delivery / "bindings/python/tests")
        for name in ("circular_orbit", "time_frames", "eop_state", "sun_moon", "two_body_scenario"):
            self.run(f"python-example-{name}", python, "-I", delivery / f"examples/python/{name}.py")
        self.run("cpp-python-parity", python, "-I", delivery / "compare_examples.py",
                 "--cpp-example", delivery / "bin/orbitforge_example")
        native = list(venv.glob("lib/python*/site-packages/orbitforge/_native*.so"))
        if len(native) != 1:
            raise RuntimeError("Expected exactly one installed native Python module")
        self.run("python-native-dependencies", "ldd", native[0])
        self.run("python-native-elf", "readelf", "-d", native[0])
        self.run("sdk-consumer-dependencies", "ldd", build / "sdk_consumer")


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--stage", choices=("all", "dependencies", "release", "sanitizers",
                                           "wheel", "export", "consumer", "compute"), default="all")
    parser.add_argument("--source", type=Path)
    parser.add_argument("--work", type=Path, required=True)
    parser.add_argument("--reports", type=Path, required=True)
    parser.add_argument("--delivery", type=Path)
    parser.add_argument("--vcpkg-root", default=os.environ.get("VCPKG_ROOT", "/opt/vcpkg"))
    parser.add_argument("--jobs", type=int, default=4)
    args = parser.parse_args()
    if platform.system() != "Linux" or args.jobs < 1:
        parser.error("A Linux host and a positive job count are required")
    if args.stage != "consumer" and not args.source:
        parser.error("Build stages require --source")
    if args.stage == "consumer" and not args.delivery:
        parser.error("Consumer validation requires --delivery")
    check = Check(args)
    check.environment()
    stages = ("dependencies", "release", "sanitizers", "wheel", "export") if args.stage == "all" else (args.stage,)
    for stage in stages:
        if stage in ("release", "sanitizers"):
            check.build(stage == "sanitizers")
        else:
            getattr(check, stage)()
    check.verify_source()
    (check.reports / "passed.json").write_text(
        json.dumps({"stages": stages, "passed": True}, indent=2) + "\n", encoding="utf-8")


if __name__ == "__main__":
    main()
