"""Measure normal multi-satellite workbench interactions on a Windows Vulkan GPU.

Reports GUI CPU time and frame cadence, not input-to-photon latency or GPU time.
Synthetic Qt drags use the same handlers as normal input. Validation layers are
disabled during timing; run run_graphics_smoke.py separately for correctness.
"""
from __future__ import annotations

import argparse
from datetime import datetime, timezone
import hashlib
import json
import os
from pathlib import Path
import subprocess


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--executable", required=True, type=Path)
    parser.add_argument("--dependency-prefix", required=True, type=Path)
    parser.add_argument("--output", required=True, type=Path)
    parser.add_argument("--satellites", type=int, default=24)
    parser.add_argument("--scale", type=float, default=1.0)
    args = parser.parse_args()
    if not 1 <= args.satellites <= 128 or not 0.5 <= args.scale <= 4:
        parser.error("satellites must be in [1, 128] and scale in [0.5, 4]")
    executable = args.executable.resolve(strict=True)
    prefix = args.dependency_prefix.resolve(strict=True)
    output = args.output.resolve()
    output.mkdir(parents=True, exist_ok=False)
    env = os.environ.copy()
    env.update(PATH=str(prefix / "bin") + os.pathsep + env.get("PATH", ""),
               QT_QPA_PLATFORM_PLUGIN_PATH=str(prefix / "Qt6/plugins/platforms"),
               QT_PLUGIN_PATH=str(prefix / "Qt6/plugins"), QT_QPA_PLATFORM="windows",
               QT_SCALE_FACTOR=str(args.scale), PROJ_DATA=str(prefix / "share/proj"),
               GDAL_DATA=str(prefix / "share/gdal"), PROJ_NETWORK="OFF")
    env.pop("VK_INSTANCE_LAYERS", None)
    command = [str(executable), "--workbench-profile", str(output / "run"),
               "--profile-satellites", str(args.satellites)]
    (output / "run-info.json").write_text(json.dumps({
        "timestamp_utc": datetime.now(timezone.utc).isoformat(), "command": command,
        "executable_sha256": hashlib.sha256(executable.read_bytes()).hexdigest(),
        "requested_scale": args.scale, "validation_layers_requested": False,
    }, indent=2) + "\n", encoding="utf-8")
    with (output / "stdout.log").open("wb") as stdout, (output / "stderr.log").open("wb") as stderr:
        process = subprocess.run(command, cwd=executable.parent, env=env, stdout=stdout,
                                 stderr=stderr, timeout=900, check=False)
    if process.returncode:
        print((output / "stderr.log").read_text(encoding="utf-8", errors="replace"))
        return process.returncode
    report = json.loads((output / "run/profile.json").read_text(encoding="utf-8"))
    for phase, metrics in report["phases"].items():
        print(phase, {key: round(value["p95"], 3) for key, value in metrics.items()}, flush=True)
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
