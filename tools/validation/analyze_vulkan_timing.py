"""Join Vulkan timing feedback by explicit (view, generation, present_id).

FIRST_PIXEL_OUT is an output boundary, not physical screen visibility. Revision 3
has no dropped flag: a zero timestamp is unavailable, never proof of a dropped
frame. Strict coverage gates prevent percentiles from hiding missing feedback.
"""
from __future__ import annotations

import argparse
from collections import Counter
import csv
import json
import math
from pathlib import Path

from analyze_graphics_resources import analyze_gpu, analyze_memory
from analyze_presentation_feedback import analyze_raw_feedback, validate_stage_declaration


def percentiles(values):
    values = sorted(values)
    if not values:
        return {"count": 0, "p50_ms": None, "p95_ms": None, "p99_ms": None, "max_ms": None}
    return {"count": len(values), **{f"p{p}_ms": values[math.ceil(len(values)*p/100)-1]
                                    for p in (50, 95, 99)}, "max_ms": values[-1]}


def read_csv(path):
    with path.open(encoding="utf-8-sig", newline="") as stream:
        return list(csv.DictReader(stream))


def analyze(frames, presents, feedback, timing, calibrations=(), gpu_queries=(),
            feedback_queries=(), feedback_stages=(), timing_properties=()):
    if timing.get("schema_version") not in (2, 3, 4):
        raise ValueError("Explicit Vulkan timing schema 2, 3, or 4 is required")
    validate_stage_declaration(timing)
    frequency = int(timing["qpc_frequency"])
    if frequency <= 0:
        raise ValueError("QPC frequency must be positive")
    # Python's integer multiplication is unbounded. Keep integer precision until
    # subtracting epochs; do not first convert long-running QPC values to floats.
    def ns(qpc): return int(qpc)*1_000_000_000//frequency
    frames = [{k: v if k == "phase" else int(v) for k, v in f.items()} for f in frames]
    presents = [{k: int(v) for k, v in p.items()} for p in presents]
    feedback = [{k: int(v) for k, v in r.items()} for r in feedback]
    calibrations = [{k: int(v) for k, v in r.items()} for r in calibrations]
    calibration_map = {c["id"]: c for c in calibrations}
    if len(calibration_map) != len(calibrations):
        raise ValueError("Duplicate calibration IDs")
    frame_map = {f["frame"]: f for f in frames}
    if len(frame_map) != len(frames):
        raise ValueError("Duplicate application frame IDs")
    for f in frames:
        if not 0 < f["begin_qpc"] <= f["update_qpc"] <= f["submit_qpc"] <= f["end_qpc"]:
            raise ValueError("Frame CPU timestamps are not ordered")
    domains = {v["view"]: v for v in timing["presentation_timing"]["views"]}
    if len(timing["presentation_timing"]["views"]) != 2 or set(domains) != {0, 1} or any(v["time_domain"] not in (3, 1000208000, 1000208001) for v in domains.values()):
        raise ValueError("Both benchmark views must declare supported clock domains")
    key = lambda r: (r["view"], r["generation"], r["present_id"])
    presentation_map = {}
    frame_view = set()
    frame_view_map = {}
    last_ids = {}
    for p in presents:
        if p["view"] not in domains or p["frame"] not in frame_map or p["present_id"] <= 0:
            raise ValueError("Invalid presentation identity")
        k = key(p)
        if k in presentation_map or (p["frame"], p["view"]) in frame_view:
            raise ValueError("Duplicate presentation identity or frame/view")
        if p["present_id"] <= last_ids.get(k[:2], 0):
            raise ValueError("Presentation IDs must increase within each swapchain")
        last_ids[k[:2]] = p["present_id"]
        f = frame_map[p["frame"]]
        if not 0 < f["submit_qpc"] <= p["enter_qpc"] <= p["leave_qpc"] <= f["end_qpc"]:
            raise ValueError("Invalid presentation QPC bracket")
        presentation_map[k] = p
        frame_view.add((p["frame"], p["view"]))
        frame_view_map[(p["frame"], p["view"])] = p
    errors, outputs, observed, states = [], {}, set(), {}
    calibration_errors = []
    for c in calibrations:
        if (c["local_ns"] <= 0 or not 0 < c["max_deviation_ns"] <= 1_000_000
                or not 0 < c["before_qpc"] <= c["after_qpc"]
                or not ns(c["before_qpc"])-c["max_deviation_ns"] <= ns(c["calibrated_qpc"]) <= ns(c["after_qpc"])+c["max_deviation_ns"]):
            calibration_errors.append(c["id"])

    def convert(r, stage, timestamp, calibration_id):
        if not timestamp:
            return 0
        if r["time_domain"] == 3:
            return timestamp
        c = calibration_map.get(calibration_id)
        if (not c or calibration_id in calibration_errors or c["stage"] != stage
                or any(c[k] != r[k] for k in ("view", "generation", "time_domain", "time_domain_id"))
                or abs(timestamp-c["local_ns"]) > 250_000_000):
            raise ValueError("Missing, mismatched, stale, or imprecise calibration")
        return timestamp-c["local_ns"]+ns(c["calibrated_qpc"])
    for r in feedback:
        k = key(r)
        if k not in presentation_map or k in observed:
            errors.append({"key": k, "reason": "unknown or duplicate feedback identity"})
            continue
        observed.add(k)
        states[k] = "invalid_feedback"
        p = presentation_map[k]
        d = domains[r["view"]]
        if (r["time_domain"] != d["time_domain"] or r["time_domain_id"] != d["time_domain_id"]
                or r["generation"] != d["generation"]):
            errors.append({"key": k, "reason": "clock domain or generation changed"})
            continue
        if r["complete"] != 1:
            states[k] = "partial"
            continue
        try:
            out = convert(r, 4, r["first_pixel_out_ns"], r.get("output_calibration_id", 0))
            queue = convert(r, 1, r["queue_end_ns"], r.get("queue_calibration_id", 0))
        except ValueError as error:
            errors.append({"key": k, "reason": str(error)})
            continue
        # Specialized EXT feedback is in ns, unlike calibrated raw QPC ticks.
        # No guessed scale/offset correction is allowed. A small 1 ms tolerance
        # admits driver timestamp precision only, not a different epoch or unit.
        low, high = ns(p["enter_qpc"])-1_000_000, ns(r["queried_qpc"])+1_000_000
        if any(t != 0 and not low <= t <= high for t in (out, queue)) or (out and queue and out < queue):
            errors.append({"key": k, "reason": "feedback timestamp units, epoch, or ordering invalid"})
            continue
        states[k] = "output_timestamp" if out else "output_unavailable"
        if out:
            outputs[k] = out
    stable = [f for f in frames if f["phase"] == "stable"]
    stable_ids = {f["frame"] for f in stable}
    missing_present = [(f["frame"], view) for f in stable for view in (0, 1)
                       if (f["frame"], view) not in frame_view]
    reports = []
    for view in (0, 1):
        subset = [p for p in presents if p["view"] == view and p["frame"] in stable_ids]
        coverage = Counter(states.get(key(p), "pending") for p in subset)
        ordered = [(outputs[key(p)], p["frame"]) for p in sorted(presents, key=lambda p: (p["generation"], p["present_id"]))
                   if p["view"] == view and key(p) in outputs]
        intervals = []
        for (earlier, previous_fid), (later, fid) in zip(ordered, ordered[1:]):
            if later <= earlier:
                errors.append({"view": view, "frame": fid, "reason": "non-increasing output time in submission order"})
            if previous_fid in stable_ids and fid in stable_ids:
                intervals.append((later-earlier)/1e6)
        reports.append({"view": view, "stable_submitted": len(subset),
                        "coverage": dict(coverage), "output_intervals_observed": percentiles(intervals)})
    requests = timing["inputs"]
    if len({r["id"] for r in requests}) != len(requests):
        raise ValueError("Duplicate input IDs")
    request_map = {r["id"]: r for r in requests}
    for f in frames:
        if f["input_id"]:
            request = request_map.get(f["input_id"])
            if not request or request["handled_qpc"] > f["begin_qpc"]:
                errors.append({"frame": f["frame"], "reason": "frame predates its handled input or references an unknown input"})
    responses = []
    for request in requests:
        if (request["kind"] not in ("cached-seek", "selection")
                or not 0 < request["posted_qpc"] <= request["handled_qpc"]
                or (request["kind"] == "selection" and request["view"] not in (0, 1))):
            raise ValueError("Invalid synthetic input record")
        candidates = {0: [], 1: []}
        response_frames = {0: [], 1: []}
        for k, output in outputs.items():
            f = frame_map[presentation_map[k]["frame"]]
            if (f["input_id"] == request["id"] and request["handled_qpc"] <= f["begin_qpc"]
                    and f["epoch_ns"] == request["target_epoch_ns"]
                    and (request["kind"] == "cached-seek" or f["selected_index"] == request["target_index"])):
                latency = (output-ns(request["posted_qpc"]))/1e6
                if latency >= 0:
                    candidates[k[0]].append(latency)
                    response_frames[k[0]].append((output, f["frame"]))
                else:
                    errors.append({"key": k, "reason": "response predates its input"})
        views = {str(v): min(values) for v, values in candidates.items() if values}
        value = (max(views.values()) if len(views) == 2 else None) if request["kind"] == "cached-seek" else views.get(str(request["view"]))
        required_views = (0, 1) if request["kind"] == "cached-seek" else (request["view"],)
        first_verified = value is not None
        for view in required_views:
            if not response_frames[view]:
                first_verified = False
                continue
            _, response_frame = min(response_frames[view])
            for f in frames:
                if f["input_id"] == request["id"] and f["frame"] <= response_frame:
                    matching = frame_view_map.get((f["frame"], view))
                    if not matching or states.get(key(matching)) != "output_timestamp":
                        first_verified = False
        responses.append({"id": request["id"], "kind": request["kind"],
                          "view_latency_ms": views, "latency_ms": value,
                          "first_response_verified": first_verified})
    response_stats = {}
    for kind in ("cached-seek", "selection"):
        subset = [r for r in responses if r["kind"] == kind]
        response_stats[kind] = {"requested": len(subset),
                                "unresolved": sum(r["latency_ms"] is None for r in subset),
                                "first_response_unverified": sum(not r["first_response_verified"] for r in subset),
                                **percentiles([r["latency_ms"] for r in subset if r["latency_ms"] is not None])}
    cpu = {name: percentiles([(f[end]-f[start])*1000/frequency for f in stable])
           for name, start, end in (("update", "begin_qpc", "update_qpc"),
                                    ("record_submit", "update_qpc", "submit_qpc"),
                                    ("present_envelope", "submit_qpc", "end_qpc"),
                                    ("render_body", "begin_qpc", "end_qpc"))}
    failed = [p for p in presents if p["result"] != 0]
    valid = (bool(stable) and not missing_present and not errors and not failed
             and timing["completed"] and not timing["failure"]
             and timing.get("validation_errors", 0) == 0 and timing.get("validation_warnings", 0) == 0
             and all(v["coverage"].get("output_timestamp", 0) == len(stable)
                     and v["output_intervals_observed"]["count"] > 0 for v in reports)
             and all(v["requested"] == timing["trials_per_kind"] and v["unresolved"] == 0
                     and v["first_response_unverified"] == 0 for v in response_stats.values()))
    gpu = analyze_gpu(frames, timing, gpu_queries, percentiles)
    memory = analyze_memory(frames, timing)
    raw = analyze_raw_feedback(frames, presents, feedback, timing, feedback_queries,
                               feedback_stages, timing_properties)
    if timing["schema_version"] >= 4:
        valid = valid and raw["valid"] and raw["drain_complete"]
    return {"schema_version": 4, "profile": timing["profile"], "timing_valid": bool(valid),
            "measurements_valid": bool(valid and gpu["valid"] and memory["valid"]),
            "scope": "first-pixel-output timing only; not physical visibility or full P0 acceptance",
            "association": "explicit (view, swapchain generation, present_id)",
            "percentile_method": "nearest rank, ceil(p*n)",
            "input_boundary": timing["input_boundary"], "feedback_errors": errors,
            "rejected_calibration_samples": calibration_errors,
            "calibration_samples": len(calibrations),
            "calibration_max_deviation_ns": max((c["max_deviation_ns"] for c in calibrations), default=0),
            "calibration_extrapolation_limit_ns": 250_000_000,
            "missing_stable_presentations": missing_present, "failed_presentations": failed,
            "all_feedback_coverage": dict(Counter(states.get(key(p), "pending") for p in presents)),
            "views": reports, "responses": response_stats, "input_results": responses,
            "cpu_diagnostic_ms": cpu, "gpu_timing": gpu, "process_resources": memory,
            "raw_feedback": raw,
            "acceptance_pass": None}


def analyze_directory(directory: Path):
    app = directory / "app"
    timing = json.loads((app / "timing.json").read_text(encoding="utf-8"))
    return analyze(read_csv(app / "frames.csv"), read_csv(app / "presents.csv"),
                   read_csv(app / "feedback.csv"), timing, read_csv(app / "calibrations.csv"),
                   read_csv(app / "gpu.csv") if timing.get("schema_version", 0) >= 3 else (),
                   read_csv(app / "feedback_queries.csv") if timing.get("schema_version", 0) >= 4 else (),
                   read_csv(app / "feedback_stages.csv") if timing.get("schema_version", 0) >= 4 else (),
                   read_csv(app / "timing_properties.csv") if timing.get("schema_version", 0) >= 4 else ())


if __name__ == "__main__":
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("directory", type=Path)
    args = parser.parse_args()
    result = analyze_directory(args.directory)
    (args.directory / "analysis.json").write_text(json.dumps(result, indent=2)+"\n", encoding="utf-8")
    print(json.dumps(result, indent=2))
