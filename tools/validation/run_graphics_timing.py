"""Capture explicit Vulkan presentation timing from the launched OrbitForge process.

The probe verifies IDs, clock calibration and coverage before a full run.
Optional PresentMon diagnostics require permission to create an ETW session;
this script never requests UAC, installs services or stops another session.
"""
from __future__ import annotations

import argparse
import ctypes
import hashlib
import json
import os
from pathlib import Path
import shutil
import subprocess
import time
import uuid

from analyze_vulkan_timing import analyze_directory

PRESENTMON_SHA256 = "b2a706bc6ad475749e3b7e3409263aa1e6906d45bdcf993f6dbc0f660188f1af"


def wait_until(predicate, process, timeout):
    deadline = time.monotonic() + timeout
    while not predicate():
        if process.poll() is not None:
            raise RuntimeError(f"Process {process.pid} exited early with {process.returncode}")
        if time.monotonic() >= deadline:
            raise TimeoutError("Timed out waiting for capture readiness")
        time.sleep(0.1)


def qpc():
    value = ctypes.c_longlong()
    if not ctypes.windll.kernel32.QueryPerformanceCounter(ctypes.byref(value)):
        raise OSError("QueryPerformanceCounter failed")
    return value.value


def start_device_sampler(log, record):
    diagnostic = {"scope": "optional device-global NVIDIA diagnostics; not process memory"}
    record["device_gpu_diagnostic"] = diagnostic
    executable = shutil.which("nvidia-smi")
    if not executable:
        diagnostic.update(status="unavailable", reason="nvidia-smi not found")
        return None
    try:
        sampler = subprocess.Popen([executable, "--query-gpu=timestamp,uuid,name,driver_version,memory.total,memory.used,utilization.gpu,utilization.memory,power.draw", "--format=csv", "--loop-ms=1000"],
                                   stdout=log("gpu-device.csv"), stderr=log("gpu.stderr.log"),
                                   creationflags=getattr(subprocess, "CREATE_NO_WINDOW", 0))
    except OSError as error:
        diagnostic.update(status="unavailable", reason=str(error))
        return None
    diagnostic.update(status="started", executable=executable)
    return sampler


def run(args, directory, index):
    directory.mkdir()
    environment = os.environ.copy()
    prefix = args.dependency_prefix
    environment.update(PATH=str(prefix / "bin") + os.pathsep + environment.get("PATH", ""),
                       QT_QPA_PLATFORM_PLUGIN_PATH=str(prefix / "Qt6/plugins/platforms"),
                       QT_QPA_PLATFORM="windows", QT_SCALE_FACTOR="1", VK_LAYER_PATH=str(prefix / "bin"),
                       GDAL_DATA=str(prefix / "share/gdal"), PROJ_DATA=str(prefix / "share/proj"), PROJ_NETWORK="OFF",
                       DISABLE_VULKAN_OBS_CAPTURE="1")
    for name in ("VK_INSTANCE_LAYERS", "VK_LOADER_LAYERS_ENABLE", "VK_LAYER_ENABLES", "VK_LAYER_DISABLES"):
        environment.pop(name, None)
    if args.validation:
        environment.update(VK_LAYER_VALIDATE_SYNC="1", VK_LAYER_VALIDATE_CORE="1",
                           VK_LAYER_OBJECT_LIFETIME="1", VK_LAYER_THREAD_SAFETY="1")
    session = "OrbitForgeTiming-" + uuid.uuid4().hex
    app = capture = gpu = None
    streams = []
    record = {"run": index, "session_name": session if args.presentmon else None,
              "profile": args.profile, "validation": args.validation,
              "output_only_probe": args.output_only_probe,
              "implicit_obs_capture_disabled_for_child": True}
    def log(name):
        stream = (directory / name).open("wb")
        streams.append(stream)
        return stream
    try:
        command = [str(args.executable), "--benchmark-scenario", str(args.scenario),
                   "--benchmark-run", str(directory / "app")]
        if args.profile == "probe": command.append("--benchmark-trace-probe")
        if args.output_only_probe: command.append("--benchmark-output-only-probe")
        if args.validation: command.append("--validation")
        record["launch_qpc"] = qpc()
        record["app_command"] = command
        app = subprocess.Popen(command, cwd=args.executable.parent, env=environment,
                               stdout=log("app.stdout.log"), stderr=log("app.stderr.log"))
        record["process_id"] = app.pid
        wait_until(lambda: (directory / "app/ready.json").exists(), app, 40)
        record["ready_observed_qpc"] = qpc()
        ready = json.loads((directory / "app/ready.json").read_text())
        if ready["process_id"] != app.pid:
            raise RuntimeError("Readiness does not belong to the launched process")
        if ready["presentation_timing"]["stage_mask"] != (4 if args.output_only_probe else 5):
            raise RuntimeError("The application did not use the requested presentation stages")
        capture_command = [str(args.presentmon), "--process_id", str(app.pid), "--session_name", session,
                           "--output_file", str(directory / "presentmon.csv"), "--qpc_time", "--v1_metrics",
                           "--no_track_input", "--no_console_stats", "--timed", "20" if args.profile == "probe" else "210",
                           "--terminate_after_timed", "--terminate_on_proc_exit"]
        record["capture_command"] = capture_command if args.presentmon else None
        if args.presentmon:
            capture = subprocess.Popen(capture_command, cwd=directory, stdout=log("presentmon.stdout.log"),
                                       stderr=log("presentmon.stderr.log"), creationflags=subprocess.CREATE_NO_WINDOW)
            wait_until(lambda: (directory / "presentmon.csv").exists(), capture, 15)
        gpu = start_device_sampler(log, record)
        (directory / "app/go").write_text("explicit Vulkan timing ready\n", encoding="ascii")
        print(f"run {index}: capture active for PID {app.pid}", flush=True)
        deadline = time.monotonic() + (45 if args.profile == "probe" else 200)
        previous = ""
        while app.poll() is None:
            if capture and capture.poll() is not None:
                raise RuntimeError("PresentMon exited before the application completed")
            if time.monotonic() >= deadline:
                raise TimeoutError("Application timing run exceeded its deadline")
            progress = (directory / "app.stdout.log").read_text(encoding="utf-8", errors="replace")
            if progress != previous:
                for line in progress[len(previous):].splitlines():
                    if line.startswith("benchmark phase:"):
                        print(line, flush=True)
                previous = progress
            time.sleep(0.5)
        record["app_exit"] = app.returncode
        record["capture_exit"] = capture.wait(timeout=30) if capture else None
        if app.returncode != 0 or (capture and capture.returncode != 0):
            raise RuntimeError("Application or presentation trace failed")
        analysis = analyze_directory(directory)
        (directory / "analysis.json").write_text(json.dumps(analysis, indent=2) + "\n", encoding="utf-8")
        record["timing_valid"] = analysis["timing_valid"]
        record["measurements_valid"] = analysis["measurements_valid"]
        print(json.dumps({"run":index, "valid": analysis["timing_valid"],
                          "coverage":analysis["all_feedback_coverage"], "views":analysis["views"], "responses":analysis["responses"],
                          "gpu_valid": analysis["gpu_timing"]["valid"], "process_resources_valid": analysis["process_resources"]["valid"],
                          "raw_feedback_valid": analysis["raw_feedback"]["valid"],
                          "drain_complete": analysis["raw_feedback"].get("drain_complete")}), flush=True)
        return record
    except BaseException as error:
        record["failure"] = str(error)
        raise
    finally:
        if capture and capture.poll() is None:
            subprocess.run([str(args.presentmon), "--session_name", session, "--terminate_existing_session"],
                           stdout=log("capture-stop.stdout.log"), stderr=log("capture-stop.stderr.log"),
                           timeout=10, check=False, creationflags=subprocess.CREATE_NO_WINDOW)
            try: capture.wait(timeout=10)
            except subprocess.TimeoutExpired: capture.terminate(); capture.wait(timeout=10)
        for process in (app, gpu):
            if process and process.poll() is None:
                process.terminate()
                process.wait(timeout=10)
        for stream in streams: stream.close()
        if gpu:
            lines = (directory / "gpu-device.csv").read_text(encoding="utf-8", errors="replace").splitlines()
            diagnostic = record["device_gpu_diagnostic"]
            diagnostic["exit_code_after_cleanup"] = gpu.returncode
            diagnostic["status"] = "collected" if len(lines) > 1 and "uuid" in lines[0] else "unavailable"
            if diagnostic["status"] == "unavailable": diagnostic["reason"] = "sampler produced no data rows"
        (directory / "capture.json").write_text(json.dumps(record, indent=2) + "\n", encoding="utf-8")


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--executable", type=Path, required=True)
    parser.add_argument("--dependency-prefix", type=Path, required=True)
    parser.add_argument("--presentmon", type=Path, help="Optional independent ETW diagnostics; not used for frame association")
    parser.add_argument("--scenario", type=Path, required=True)
    parser.add_argument("--output", type=Path, required=True)
    parser.add_argument("--profile", choices=("probe", "full"), default="probe")
    parser.add_argument("--validation", action="store_true")
    parser.add_argument("--output-only-probe", action="store_true",
                        help="Diagnostic probe requesting FIRST_PIXEL_OUT only; full runs are disallowed")
    args = parser.parse_args()
    if args.output_only_probe and args.profile != "probe":
        raise ValueError("Output-only timing is restricted to diagnostic probes")
    if os.name != "nt": raise RuntimeError("This capture entry point requires Windows")
    if args.validation and args.profile != "probe": raise ValueError("Validation is restricted to probe runs")
    for field in ("executable", "dependency_prefix", "scenario"):
        setattr(args, field, getattr(args, field).resolve(strict=True))
    if args.presentmon:
        args.presentmon = args.presentmon.resolve(strict=True)
    if args.presentmon and hashlib.sha256(args.presentmon.read_bytes()).hexdigest() != PRESENTMON_SHA256:
        raise ValueError("PresentMon binary differs from the fixed 2.6.0 asset")
    output = args.output.resolve()
    output.mkdir(parents=True, exist_ok=False)
    records = []
    requested_profile = args.profile
    preflight = None
    failure = None
    try:
        if requested_profile == "full":
            args.profile = "probe"
            preflight = run(args, output / "probe", 0)
            args.profile = requested_profile
        if preflight is None or preflight["measurements_valid"]:
            for index in range(1, 2 if requested_profile == "probe" else 4):
                record = run(args, output / f"run-{index}", index)
                records.append(record)
                if not record["measurements_valid"]: break
    except (RuntimeError, TimeoutError, ValueError, OSError) as error:
        failure = str(error)
        print(f"capture failed: {failure}", flush=True)
    expected_runs = 1 if requested_profile == "probe" else 3
    summary = {"runs": records, "profile": requested_profile, "preflight": preflight, "failure": failure,
               "output_only_probe": args.output_only_probe,
               "timing_valid": len(records) == expected_runs and all(r["timing_valid"] for r in records) and not failure,
               "measurements_valid": len(records) == expected_runs and all(r["measurements_valid"] for r in records) and not failure,
               "presentmon_sha256": PRESENTMON_SHA256 if args.presentmon else None,
               "executable_sha256": hashlib.sha256(args.executable.read_bytes()).hexdigest()}
    (output / "summary.json").write_text(json.dumps(summary, indent=2) + "\n", encoding="utf-8")
    return 0 if summary["measurements_valid"] else 2


if __name__ == "__main__":
    raise SystemExit(main())
