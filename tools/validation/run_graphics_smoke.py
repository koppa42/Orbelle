"""Run the real Qt/VSG application; save per-scale logs, images and assertions.

Requires a Windows desktop session and a Vulkan GPU. This is not a performance test.
Only the launched process receives the Qt and Vulkan environment settings.
"""
from __future__ import annotations

import argparse
from datetime import datetime, timezone
import json
import os
from pathlib import Path
import subprocess


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--executable", required=True, type=Path)
    parser.add_argument("--dependency-prefix", required=True, type=Path)
    parser.add_argument("--output", required=True, type=Path)
    parser.add_argument("--scales", nargs="+", type=float, default=[1.0, 1.5, 2.0])
    parser.add_argument("--benchmark-scenario", type=Path,
                        help="Use the fixed 10k workload; still a validation preflight, not timing")
    args = parser.parse_args()
    executable = args.executable.resolve(strict=True)
    prefix = args.dependency_prefix.resolve(strict=True)
    scenario = args.benchmark_scenario.resolve(strict=True) if args.benchmark_scenario else None
    output = args.output.resolve()
    output.mkdir(parents=True, exist_ok=False)
    environment = os.environ.copy()
    environment["PATH"] = str(prefix / "bin") + os.pathsep + environment.get("PATH", "")
    environment["QT_QPA_PLATFORM_PLUGIN_PATH"] = str(prefix / "Qt6/plugins/platforms")
    environment["QT_QPA_PLATFORM"] = "windows"
    environment["VK_LAYER_PATH"] = str(prefix / "bin")
    environment["PROJ_DATA"] = str(prefix / "share/proj")
    environment["GDAL_DATA"] = str(prefix / "share/gdal")
    environment["PROJ_NETWORK"] = "OFF"
    # Fixed VVL 1.4.350's supported setting (deprecated VK_LAYER_ENABLES is not used).
    # https://github.com/KhronosGroup/Vulkan-ValidationLayers/blob/vulkan-sdk-1.4.350.0/docs/updating_from_VK_EXT_validation_features.md
    for key in ("VK_LAYER_ENABLES", "VK_LAYER_DISABLES"):
        environment.pop(key, None)
    environment["VK_LAYER_VALIDATE_SYNC"] = "1"
    environment["VK_LAYER_VALIDATE_CORE"] = "1"
    environment["VK_LAYER_OBJECT_LIFETIME"] = "1"
    environment["VK_LAYER_THREAD_SAFETY"] = "1"
    records = []
    success = True
    for scale in args.scales:
        if not 0.5 <= scale <= 4:
            raise ValueError("Scale must be in [0.5, 4]")
        run_dir = output / f"scale-{scale:g}"
        run_dir.mkdir()
        environment["QT_SCALE_FACTOR"] = f"{scale:g}"
        command = [str(executable), "--graphics-smoke", str(run_dir)]
        if scenario:
            command += ["--benchmark-scenario", str(scenario)]
        record = {"requested_scale": scale, "command": command}
        try:
            process = subprocess.run(command, env=environment, cwd=executable.parent,
                                     capture_output=True, timeout=60, check=False)
            (run_dir / "stdout.log").write_bytes(process.stdout)
            (run_dir / "stderr.log").write_bytes(process.stderr)
            record["exit_code"] = process.returncode
            report_path = run_dir / "report.json"
            report = json.loads(report_path.read_text(encoding="utf-8")) if report_path.exists() else None
            record["report"] = report
            passed = (process.returncode == 0 and report is not None
                      and report["validation_errors"] == 0 and report["shared_device"]
                      and report["shared_scene"] and report["all_windows_closed_before_shutdown"]
                      and report["independent_cameras"]
                      and len(report["views"]) == 2)
            if passed:
                for view in report["views"]:
                    ratio = view["device_pixel_ratio"]
                    # Native monitor scaling multiplies QT_SCALE_FACTOR, so check the actual DPR.
                    passed &= abs(view["width_pixels"] - view["width_logical"] * ratio) <= 1
                    passed &= abs(view["height_pixels"] - view["height_logical"] * ratio) <= 1
                    passed &= view["capture_width_pixels"] == view["width_pixels"]
                    passed &= view["capture_height_pixels"] == view["height_pixels"]
                    passed &= view["foreground_pixels_at_edge"] == 0
                    passed &= view["native_creations"] >= 2 and view["native_destructions"] >= 1
                    passed &= view["successful_presentations"] >= 30 and view["marker_pixels"] >= 6
                    if scenario:
                        passed &= (view["width_pixels"], view["height_pixels"]) == (1280, 720)
                        passed &= view["submitted_markers"] == 10000 and view["submitted_labels"] == 200
                        passed &= view["selection_pixels"] >= 20
                if scenario:
                    kinds = [item["kind"] for item in report.get("interactions", [])]
                    passed &= kinds == ["cached-slider-seek", "synthetic-Qt-click", "clear-selection", "synthetic-Qt-click"]
                    passed &= report.get("benchmark", {}).get("satellites") == 10000
            record["passed"] = bool(passed)
        except subprocess.TimeoutExpired as error:
            (run_dir / "stdout.log").write_bytes(error.stdout or b"")
            (run_dir / "stderr.log").write_bytes(error.stderr or b"")
            record.update(passed=False, failure="External 60-second timeout")
        records.append(record)
        success &= record["passed"]
        print(f"scale {scale:g}: {'PASS' if record['passed'] else 'FAIL'}", flush=True)
        if not success:
            break
    missing_resources = output / "missing-resources"
    missing_resources.mkdir()
    rejected = {"passed": False}
    try:
        process = subprocess.run(
            [str(executable), "--validation", "--rocky-resources", str(missing_resources)],
            env=environment, cwd=executable.parent, capture_output=True, timeout=15, check=False)
        (missing_resources / "stderr.log").write_bytes(process.stderr)
        (missing_resources / "stdout.log").write_bytes(process.stdout)
        rejected = {"exit_code": process.returncode,
                    "passed": process.returncode == 1 and b"Rocky shader resource missing" in process.stderr}
    except subprocess.TimeoutExpired as error:
        (missing_resources / "stderr.log").write_bytes(error.stderr or b"")
        rejected["failure"] = "External 15-second timeout"
    success &= rejected["passed"]
    print(f"missing resources: {'PASS' if rejected['passed'] else 'FAIL'}", flush=True)
    no_driver = output / "no-vulkan-driver"
    no_driver.mkdir()
    no_driver_environment = environment.copy()
    # Override discovery only in this child, with an existing empty manifest directory.
    # No machine drivers, registry entries or parent environment are changed.
    no_driver_environment["VK_DRIVER_FILES"] = str(no_driver)
    driver_rejected = {"passed": False}
    try:
        process = subprocess.run(
            [str(executable), "--validation"], env=no_driver_environment, cwd=executable.parent,
            capture_output=True, timeout=15, check=False)
        (no_driver / "stderr.log").write_bytes(process.stderr)
        (no_driver / "stdout.log").write_bytes(process.stdout)
        driver_rejected = {
            "exit_code": process.returncode,
            "passed": process.returncode == 1 and b"failed to create VkInstance" in process.stderr}
    except subprocess.TimeoutExpired as error:
        (no_driver / "stderr.log").write_bytes(error.stderr or b"")
        driver_rejected["failure"] = "External 15-second timeout"
    success &= driver_rejected["passed"]
    print(f"missing Vulkan driver: {'PASS' if driver_rejected['passed'] else 'FAIL'}", flush=True)
    scenario_rejected = None
    if scenario:
        altered = output / "altered-scenario.json"
        content = json.loads(scenario.read_text(encoding="utf-8"))
        content["workload"]["satellites"] = 1
        altered.write_text(json.dumps(content), encoding="utf-8")
        process = subprocess.run(
            [str(executable), "--benchmark-scenario", str(altered)], env=environment,
            cwd=executable.parent, capture_output=True, timeout=15, check=False)
        (output / "altered-scenario.stderr.log").write_bytes(process.stderr)
        scenario_rejected = {"exit_code": process.returncode,
                             "passed": process.returncode == 1 and b"Benchmark resource SHA256 mismatch" in process.stderr}
        success &= scenario_rejected["passed"]
        print(f"altered workload: {'PASS' if scenario_rejected['passed'] else 'FAIL'}", flush=True)
    summary = {"timestamp_utc": datetime.now(timezone.utc).isoformat(),
               "scope": "graphics integration; not performance acceptance",
               "synchronization_validation_requested": True, "runs": records,
               "missing_resources_rejected": rejected,
               "altered_scenario_rejected": scenario_rejected,
               "missing_vulkan_driver_rejected": driver_rejected, "passed": bool(success)}
    (output / "summary.json").write_text(json.dumps(summary, ensure_ascii=False, indent=2) + "\n",
                                        encoding="utf-8")
    return 0 if success else 1


if __name__ == "__main__":
    raise SystemExit(main())
