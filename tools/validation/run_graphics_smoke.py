"""Run the real Qt/VSG application; save per-scale logs, images and assertions.

Requires a Windows desktop session and a Vulkan GPU. This is not a performance test.
Only the launched process receives the Qt and Vulkan environment settings.
"""
from __future__ import annotations

import argparse
from datetime import datetime, timezone
import json
from pathlib import Path
import subprocess

from desktop_test_support import desktop_environment, run_logged, timeout_message


def expected_failure(command, environment, directory, message, *, stem=""):
    try:
        process = run_logged(command, env=environment, cwd=Path(command[0]).parent,
                             directory=directory, timeout=15, stem=stem)
        stderr = (directory / f"{stem}stderr.log").read_bytes()
        return {"exit_code": process.returncode,
                "passed": process.returncode == 1 and message in stderr}
    except subprocess.TimeoutExpired as error:
        return {"passed": False, "failure": timeout_message(error)}


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--executable", required=True, type=Path)
    parser.add_argument("--dependency-prefix", required=True, type=Path)
    parser.add_argument("--output", required=True, type=Path)
    parser.add_argument("--scales", nargs="+", type=float, default=[1.0, 1.5, 2.0])
    parser.add_argument("--benchmark-scenario", type=Path,
                        help="Use the fixed 10k workload; still a validation preflight, not timing")
    parser.add_argument("--project-fixture", type=Path,
                        help="Open a Python-created two-body/J2 .orbitforge file in the desktop checks")
    args = parser.parse_args()
    executable = args.executable.resolve(strict=True)
    prefix = args.dependency_prefix.resolve(strict=True)
    scenario = args.benchmark_scenario.resolve(strict=True) if args.benchmark_scenario else None
    output = args.output.resolve()
    output.mkdir(parents=True, exist_ok=False)
    environment = desktop_environment(prefix, validation=True)
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
        if args.project_fixture:
            command += ["--project-smoke-input", str(args.project_fixture.resolve(strict=True))]
        record = {"requested_scale": scale, "command": command}
        try:
            process = run_logged(command, env=environment, cwd=executable.parent,
                                 directory=run_dir, timeout=120)
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
            record.update(passed=False, failure=timeout_message(error))
        records.append(record)
        success &= record["passed"]
        print(f"scale {scale:g}: {'PASS' if record['passed'] else 'FAIL'}", flush=True)
        if not success:
            break
    missing_resources = output / "missing-resources"
    missing_resources.mkdir()
    rejected = expected_failure(
        [str(executable), "--validation", "--rocky-resources", str(missing_resources)],
        environment, missing_resources, b"Rocky shader resource missing")
    success &= rejected["passed"]
    print(f"missing resources: {'PASS' if rejected['passed'] else 'FAIL'}", flush=True)
    no_driver = output / "no-vulkan-driver"
    no_driver.mkdir()
    no_driver_environment = environment.copy()
    # Override discovery only in this child, with an existing empty manifest directory.
    # No machine drivers, registry entries or parent environment are changed.
    no_driver_environment["VK_DRIVER_FILES"] = str(no_driver)
    driver_rejected = expected_failure(
        [str(executable), "--validation"], no_driver_environment, no_driver,
        b"failed to create VkInstance")
    success &= driver_rejected["passed"]
    print(f"missing Vulkan driver: {'PASS' if driver_rejected['passed'] else 'FAIL'}", flush=True)
    scenario_rejected = None
    if scenario:
        altered = output / "altered-scenario.json"
        content = json.loads(scenario.read_text(encoding="utf-8"))
        content["workload"]["satellites"] = 1
        altered.write_text(json.dumps(content), encoding="utf-8")
        scenario_rejected = expected_failure(
            [str(executable), "--benchmark-scenario", str(altered)], environment, output,
            b"Benchmark resource SHA256 mismatch", stem="altered-scenario.")
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
