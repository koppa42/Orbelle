"""Validate raw per-view GPU queries and process-local resource samples.

GPU timestamp spans are not additive busy time. DXGI local/non-local usage is an
OS process accounting metric, not device-global usage or Vulkan allocation size.
"""
from __future__ import annotations

import math
import re


def analyze_gpu(frames, timing, rows, percentiles):
    metadata = timing.get("gpu_timing", {}).get("views", [])
    if not metadata:
        return {"instrumented": False, "valid": False, "errors": ["GPU query metadata missing"]}
    errors, durations, seen, uses = [], {}, set(), {}
    domains = {v["view"]: v for v in metadata}
    if len(metadata) != 2 or set(domains) != {0, 1}:
        raise ValueError("GPU timing must identify exactly two views")
    for d in domains.values():
        if (not 36 <= d["timestamp_valid_bits"] <= 64 or d["generation"] != 1
                or not math.isfinite(d["timestamp_period_ns"]) or d["timestamp_period_ns"] <= 0
                or d["queue_family"] < 0 or d["query_slots"] <= 0):
            raise ValueError("Invalid GPU timestamp properties")
    frequency = int(timing["qpc_frequency"])
    finished = int(timing.get("finished_qpc", 0))
    if finished <= 0:
        raise ValueError("GPU timing requires a run-finished QPC upper bound")
    frame_map = {f["frame"]: f for f in frames}
    rows = [{k: int(v) for k, v in r.items()} for r in rows]
    for r in rows:
        key = (r["frame"], r["view"])
        d, f = domains.get(r["view"]), frame_map.get(r["frame"])
        if not d or not f or key in seen or r["generation"] != d["generation"]:
            errors.append({"key": key, "reason": "unknown, duplicate, or mismatched GPU query identity"})
            continue
        seen.add(key)
        if (not 0 <= r["slot"] < d["query_slots"] or r["slot_use"] <= 0
                or not f["update_qpc"] <= r["prepared_qpc"] <= f["submit_qpc"]
                or not r["prepared_qpc"] <= r["read_qpc"] <= finished):
            errors.append({"key": key, "reason": "GPU slot or CPU time bracket invalid"})
            continue
        slot = (r["view"], r["generation"], r["slot"])
        previous = uses.get(slot)
        if ((not previous and r["slot_use"] != 1)
                or (previous and (r["slot_use"] != previous["slot_use"]+1
                                  or r["frame"] <= previous["frame"]
                                  or not r["prepared_qpc"] <= previous["read_qpc"] <= f["submit_qpc"]))):
            errors.append({"key": key, "reason": "GPU fence slot generations/readback order invalid"})
        uses[slot] = r
        if r["result"] != 0 or r["begin_available"] <= 0 or r["end_available"] <= 0:
            errors.append({"key": key, "reason": "GPU queries unavailable or retrieval failed"})
            continue
        bits, period = d["timestamp_valid_bits"], d["timestamp_period_ns"]
        if not all(0 <= r[k] < 1 << bits for k in ("begin_tick", "end_tick")):
            errors.append({"key": key, "reason": "GPU query ticks exceed the valid counter bits"})
            continue
        mask = (1 << bits)-1  # Python integers also handle the 64-bit mask without overflow.
        delta = (r["end_tick"]-r["begin_tick"]) & mask
        elapsed_ns = delta*period
        # GPU commands are recorded after prepared_qpc and completed before read_qpc.
        # This CPU upper bound must exclude an entire wrap, even if modulo delta looks small.
        bound_ns = (r["read_qpc"]-r["prepared_qpc"])*1_000_000_000/frequency
        tolerance_ns = 1_000_000
        if bound_ns+tolerance_ns >= (1 << bits)*period:
            errors.append({"key": key, "reason": "GPU counter may have wrapped more than once"})
            continue
        if elapsed_ns > bound_ns+tolerance_ns:
            errors.append({"key": key, "reason": "GPU duration exceeds its CPU lifetime bound"})
            continue
        durations[key] = elapsed_ns/1_000_000
    stable = [f["frame"] for f in frames if f["phase"] == "stable"]
    missing = [(f["frame"], view) for f in frames for view in (0, 1)
               if (f["frame"], view) not in durations]
    views = [{"view": view, "stable_expected": len(stable),
              "stable_gpu_span_ms": percentiles([durations[(fid, view)] for fid in stable
                                                  if (fid, view) in durations]),
              "timestamp_valid_bits": domains[view]["timestamp_valid_bits"],
              "timestamp_period_ns": domains[view]["timestamp_period_ns"]} for view in (0, 1)]
    return {"instrumented": True, "valid": bool(stable) and not missing and not errors,
            "scope": "per-view RenderGraph TOP_OF_PIPE to BOTTOM_OF_PIPE GPU elapsed span; "
                     "may include waits/overlap; not additive exclusive GPU busy time",
            "record_count": len(rows), "errors": errors, "missing_frame_queries": missing, "views": views}


def range_stats(values):
    return {"count": len(values), "min": min(values) if values else None,
            "max": max(values) if values else None,
            "mean": sum(values)/len(values) if values else None}


def analyze_memory(frames, timing):
    metadata = timing.get("video_memory")
    if not metadata:
        return {"instrumented": False, "valid": False, "errors": ["Process video memory metadata missing"]}
    mask, node = metadata.get("node_mask", 0), metadata.get("node_index", -1)
    if (metadata.get("source") != "IDXGIAdapter3::QueryVideoMemoryInfo"
            or metadata.get("adapter_match") != "Vulkan deviceLUID == DXGI AdapterLuid"
            or not re.fullmatch("[0-9a-f]{16}", metadata.get("adapter_luid_hex", ""))
            or not re.fullmatch("[0-9a-f]{32}", metadata.get("device_uuid_hex", ""))
            or not 0 <= node < 32 or mask != 1 << node
            or metadata.get("sample_interval_seconds") != 1):
        raise ValueError("Process memory must identify the Vulkan adapter LUID and node")
    if any(d.get("device_uuid_hex") != metadata["device_uuid_hex"]
           or d.get("device_luid_hex") != metadata["adapter_luid_hex"]
           for d in timing.get("gpu_timing", {}).get("views", [])):
        raise ValueError("GPU timestamps and process memory refer to different Vulkan devices")
    errors, stable_samples, stable_cpu, previous = [], [], [], 0
    previous_stable = False
    frames = sorted(frames, key=lambda f: f["frame"])
    finished = int(timing.get("finished_qpc", 0))
    if not frames or finished < frames[-1]["end_qpc"]:
        raise ValueError("Process resources require a run-finished QPC upper bound")
    if any(a["end_qpc"] > b["begin_qpc"] for a, b in zip(frames, frames[1:])):
        raise ValueError("Process resource frame bounds overlap or go backwards")
    by_end = {f["end_qpc"]: f for f in frames}
    upper_bound = {f["end_qpc"]: frames[i+1]["begin_qpc"] if i+1 < len(frames) else finished
                   for i, f in enumerate(frames)}
    stable_frames = [f for f in frames if f["phase"] == "stable"]
    stable_start = stable_frames[0]["begin_qpc"] if stable_frames else 0
    stable_end = upper_bound[stable_frames[-1]["end_qpc"]] if stable_frames else 0
    if any(f["phase"] != "stable" for f in frames if stable_start <= f["begin_qpc"] < stable_end):
        raise ValueError("Stable resource window must be contiguous")
    rows = timing.get("resources", [])
    for r in rows:
        qpc = r["qpc"]
        frame = by_end.get(qpc)
        if (not frame or r["phase"] != frame["phase"] or qpc <= previous
                or not qpc <= r["sample_begin_qpc"] <= r["sample_end_qpc"] <= upper_bound.get(qpc, 0)):
            errors.append({"qpc": qpc, "reason": "resource sample frame/phase/time bracket invalid"})
        previous = qpc
        memory = r.get("video_memory", {})
        for segment in ("local", "non_local"):
            info = memory.get(segment, {})
            if any(type(info.get(k)) is not int or info[k] < 0 for k in
                   ("current_usage_bytes", "budget_bytes", "current_reservation_bytes",
                    "available_for_reservation_bytes")):
                errors.append({"qpc": qpc, "reason": f"invalid {segment} process memory sample"})
        if any(type(r.get(k)) is not int or r[k] < 0 for k in
               ("working_set_bytes", "private_bytes", "peak_working_set_bytes")):
            errors.append({"qpc": qpc, "reason": "invalid process RAM sample"})
        in_stable = (r["phase"] == "stable"
                     and stable_start <= r["sample_begin_qpc"] <= r["sample_end_qpc"] <= stable_end)
        if in_stable:
            cpu = r.get("cpu_percent_of_one_core")
            if cpu is None or not math.isfinite(cpu) or cpu < 0:
                errors.append({"qpc": qpc, "reason": "invalid stable CPU usage sample"})
            stable_samples.append(r)
            # CPU usage is an interval delta. Exclude the warmup -> stable interval.
            if previous_stable:
                stable_cpu.append(cpu)
        previous_stable = in_stable
    frequency = int(timing["qpc_frequency"])
    # Bound the longest possible gap between the actual API samples, whose exact
    # instants lie inside each recorded envelope. A late/long call cannot borrow
    # its frame's earlier end_qpc to appear inside the stable measurement window.
    gaps = ([(stable_samples[0]["sample_end_qpc"]-stable_start)/frequency]
            + [(b["sample_end_qpc"]-a["sample_begin_qpc"])/frequency
               for a, b in zip(stable_samples, stable_samples[1:])]
            + [(stable_end-stable_samples[-1]["sample_begin_qpc"])/frequency]) if stable_samples else []
    if not stable_samples or not stable_cpu or any(g < 0 or g > 1.5 for g in gaps):
        errors.append({"reason": "stable resource gap exceeds 1.5 s, or no complete stable CPU interval"})
    # Missing or malformed samples never become zeros in statistics.
    if errors:
        return {"instrumented": True, "valid": False, "errors": errors,
                "record_count": len(rows), "stable_sample_count": len(stable_samples)}
    segments = {segment: {key: range_stats([r["video_memory"][segment][key] for r in stable_samples])
                          for key in ("current_usage_bytes", "budget_bytes")}
                for segment in ("local", "non_local")}
    ram = {key: range_stats([r[key] for r in stable_samples])
           for key in ("working_set_bytes", "private_bytes", "peak_working_set_bytes")}
    return {"instrumented": True, "valid": True, "errors": [], "metadata": metadata,
            "record_count": len(rows), "stable_sample_count": len(stable_samples),
            "max_stable_sampling_gap_seconds": max(gaps, default=None),
            "stable_window_qpc": [stable_start, stable_end],
            "scope": "process-local samples, not a continuous peak or device-global VRAM",
            "stable_video_memory_bytes": segments, "stable_ram_bytes": ram,
            "stable_cpu_percent_of_one_core": range_stats(stable_cpu),
            "sample_overhead_ms": range_stats([(r["sample_end_qpc"]-r["sample_begin_qpc"])*1000/frequency for r in rows])}
