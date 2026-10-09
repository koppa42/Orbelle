"""Rejected experiment: PresentMon QPC interval association is not reliable here.

Retained for the original diagnostic records only. Use analyze_vulkan_timing.py
for current captures; this experiment never grants timing validity.

No nearest-neighbor or row-order matching. Dropped and unassociated rows remain
diagnostics; missing responses are never converted to zero or silently omitted.
"""
from __future__ import annotations

import argparse
from bisect import bisect_right
from collections import Counter, defaultdict
import csv
import json
import math
from pathlib import Path


def percentiles(values: list[float]) -> dict:
    ordered = sorted(values)
    if not ordered:
        return {"count": 0, "p50_ms": None, "p95_ms": None, "p99_ms": None, "max_ms": None}
    return {"count": len(ordered), **{
        f"p{p}_ms": ordered[max(0, math.ceil(len(ordered) * p / 100) - 1)] for p in (50, 95, 99)},
        "max_ms": ordered[-1]}


def read_csv(path: Path) -> list[dict]:
    with path.open(encoding="utf-8-sig", newline="") as stream:
        return list(csv.DictReader(stream))


def analyze(frames: list[dict], presents: list[dict], rows: list[dict], timing: dict,
            expected_pid: int) -> dict:
    frequency = int(timing["qpc_frequency"])
    if frequency <= 0:
        raise ValueError("QPC frequency must be positive")
    frame_by_id = {int(value["frame"]): value for value in frames}
    if len(frame_by_id) != len(frames):
        raise ValueError("Duplicate application frame IDs")
    intervals = sorted(presents, key=lambda value: int(value["enter_qpc"]))
    starts = [int(value["enter_qpc"]) for value in intervals]
    for i, interval in enumerate(intervals):
        if (int(interval["result"]) != 0 or int(interval["view_mask"]) not in (1, 2, 3)
                or int(interval["leave_qpc"]) < starts[i]):
            raise ValueError("Failed or malformed application present")
        if i and int(intervals[i - 1]["leave_qpc"]) >= starts[i]:
            raise ValueError("Overlapping QPC present intervals are ambiguous")
        if int(interval["frame"]) not in frame_by_id:
            raise ValueError("Present references an unknown frame")

    calibration = defaultdict(Counter)
    associated, unmatched = [], []
    for row in rows:
        if int(row["ProcessID"]) != expected_pid:
            raise ValueError("The trace includes an unrelated process")
        timestamp = int(row["QPCTime"])
        index = bisect_right(starts, timestamp) - 1
        if index < 0 or timestamp > int(intervals[index]["leave_qpc"]):
            unmatched.append(row)
            continue
        interval = intervals[index]
        frame = frame_by_id[int(interval["frame"])]
        address = row["SwapChainAddress"]
        if frame["phase"] == "calibration":
            mask = int(interval["view_mask"])
            if mask not in (1, 2):
                raise ValueError("Calibration must identify exactly one view")
            calibration[address][0 if mask == 1 else 1] += 1
        associated.append((row, interval, frame))

    mapping = {}
    invalid_calibration = {}
    for address, votes in calibration.items():
        if len(votes) == 1 and sum(votes.values()) >= 10:
            mapping[address] = next(iter(votes))
        else:
            invalid_calibration[address] = dict(votes)
    mapping_valid = len(mapping) == 2 and set(mapping.values()) == {0, 1} and not invalid_calibration
    by_view = defaultdict(list)
    duplicates = []
    unknown = []
    observed = set()
    invalid_display = []
    for row, interval, frame in associated:
        address = row["SwapChainAddress"]
        if address not in mapping:
            unknown.append(row)
            continue
        view = mapping[address]
        if not int(interval["view_mask"]) & (1 << view):
            invalid_calibration.setdefault(address, {})["inconsistent_view"] = True
            continue
        key = (int(frame["frame"]), view)
        if key in observed:
            duplicates.append(key)
        observed.add(key)
        dropped = int(row["Dropped"]) == 1
        delay = float(row["msUntilDisplayed"])
        if not dropped and (not math.isfinite(delay) or delay <= 0):
            invalid_display.append(key)
            continue
        by_view[view].append({"frame": frame, "row": row, "dropped": dropped,
                              "display_qpc": int(row["QPCTime"]) + delay * frequency / 1000})

    stable = [value for value in frames if value["phase"] == "stable"]
    missing_stable = [(int(frame["frame"]), view) for frame in stable for view in (0, 1)
                      if (int(frame["frame"]), view) not in observed]
    view_reports = []
    duplicate_display = []
    for view in (0, 1):
        displayed = sorted((value for value in by_view[view] if not value["dropped"]),
                           key=lambda value: value["display_qpc"])
        intervals_ms = []
        for previous, current in zip(displayed, displayed[1:]):
            if current["frame"]["phase"] != "stable":
                continue
            delta = (current["display_qpc"] - previous["display_qpc"]) * 1000 / frequency
            if delta <= 0:
                duplicate_display.append((view, current["frame"]["frame"]))
            intervals_ms.append(delta)
        submitted = [value for value in by_view[view] if value["frame"]["phase"] == "stable"]
        view_reports.append({"view": view, "display_intervals": percentiles(intervals_ms),
                             "stable_submitted": len(submitted),
                             "stable_dropped": sum(value["dropped"] for value in submitted),
                             "present_modes": dict(Counter(value["row"]["PresentMode"] for value in submitted))})

    input_reports = []
    for request in timing["inputs"]:
        candidates = {}
        for view in (0, 1):
            candidates[view] = [value for value in by_view[view]
                               if not value["dropped"]
                               and int(value["frame"]["input_id"]) == request["id"]
                               and int(value["frame"]["epoch_ns"]) == request["target_epoch_ns"]
                               and (request["kind"] == "cached-seek" or
                                    int(value["frame"]["selected_index"]) == request["target_index"])]
        response = {"id": request["id"], "kind": request["kind"], "view_latency_ms": {}}
        for view in (0, 1):
            if candidates[view]:
                first = min(candidates[view], key=lambda value: value["display_qpc"])
                latency = (first["display_qpc"] - request["posted_qpc"]) * 1000 / frequency
                if latency >= 0 and math.isfinite(latency):
                    response["view_latency_ms"][str(view)] = latency
        views = response["view_latency_ms"]
        response["latency_ms"] = (max(views.values()) if len(views) == 2 else None) if request["kind"] == "cached-seek" else views.get(str(request["view"]))
        input_reports.append(response)

    cpu = {name: percentiles([(int(value[end]) - int(value[start])) * 1000 / frequency for value in stable])
           for name, start, end in (("update", "begin_qpc", "update_qpc"),
                                    ("record_submit", "update_qpc", "submit_qpc"),
                                    ("present_envelope", "submit_qpc", "end_qpc"),
                                    ("render_body", "begin_qpc", "end_qpc"))}
    responses = {}
    for kind in ("cached-seek", "selection"):
        subset = [value for value in input_reports if value["kind"] == kind]
        responses[kind] = {"requested": len(subset),
                           "unresolved": sum(value["latency_ms"] is None for value in subset),
                           **percentiles([value["latency_ms"] for value in subset if value["latency_ms"] is not None])}
    valid = (mapping_valid and not invalid_calibration and not duplicates and not invalid_display
             and not missing_stable and not duplicate_display and bool(stable)
             and timing["completed"] and not timing["failure"]
             and all(value["requested"] == timing["trials_per_kind"] and value["unresolved"] == 0 for value in responses.values()))
    return {"scope": "presentation intervals and posted Qt synthetic input response",
            "profile": timing["profile"], "timing_valid": False,
            "experimental_association_checks_passed": bool(valid),
            "percentile_method": "nearest rank, ceil(p*n)",
            "swapchain_view_mapping": mapping, "calibration_votes": dict(calibration),
            "invalid_calibration": invalid_calibration, "csv_rows": len(rows),
            "unmatched_rows": len(unmatched), "unknown_swapchain_rows": len(unknown),
            "duplicate_frame_view_rows": duplicates, "invalid_display_rows": invalid_display,
            "missing_stable_frame_view_rows": missing_stable, "duplicate_display_timestamps": duplicate_display,
            "views": view_reports, "responses": responses, "input_results": input_reports,
            "cpu_diagnostic_ms": cpu,
            "input_boundary": timing["input_boundary"],
            "gpu_execution_and_process_vram": "not instrumented yet"}


def analyze_directory(directory: Path) -> dict:
    timing = json.loads((directory / "app/timing.json").read_text(encoding="utf-8"))
    ready = json.loads((directory / "app/ready.json").read_text(encoding="utf-8"))
    return analyze(read_csv(directory / "app/frames.csv"), read_csv(directory / "app/presents.csv"),
                   read_csv(directory / "presentmon.csv"), timing, ready["process_id"])


if __name__ == "__main__":
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("directory", type=Path)
    args = parser.parse_args()
    result = analyze_directory(args.directory)
    (args.directory / "analysis.json").write_text(json.dumps(result, indent=2) + "\n", encoding="utf-8")
    print(json.dumps(result, indent=2))
