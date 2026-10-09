"""Audit schema 4 raw Vulkan feedback independently of timestamp availability.

Complete feedback is consumed even when a requested timestamp is zero. Refresh
properties describe the query-time swapchain, not every historical record in a
returned batch; VK_NOT_READY and UINT64_MAX are preserved without substitution.
"""
from collections import Counter, defaultdict


def validate_stage_declaration(timing):
    """Legacy records must not bypass the probe-only stage mode restriction."""
    metadata = timing["presentation_timing"]
    modern = timing["schema_version"] >= 4
    stage_mask = int(metadata["stage_mask"] if modern else metadata.get("stage_mask", 5))
    output_only = metadata.get("output_only_probe", False)
    if (stage_mask not in (4, 5) or not isinstance(output_only, bool)
            or output_only != (stage_mask == 4)
            or (output_only and (not modern or timing["profile"] != "trace-probe"))):
        raise ValueError("Output-only stages require schema 4, trace-probe, and an explicit diagnostic flag")
    return {1, 4} if stage_mask == 5 else {4}


def analyze_raw_feedback(frames, presents, feedback, timing, queries, stages, properties):
    if timing["schema_version"] < 4:
        return {"instrumented": False, "valid": False, "errors": ["Raw feedback schema 4 required"]}
    queries = [{k: v if k == "phase" else int(v) for k, v in r.items()} for r in queries]
    stages = [{k: int(v) for k, v in r.items()} for r in stages]
    properties = [{k: int(v) for k, v in r.items()} for r in properties]
    metadata = timing["presentation_timing"]
    domains = {v["view"]: v for v in metadata["views"]}
    capacity = int(metadata["feedback_capacity_per_view"])
    stage_capacity = int(metadata["feedback_stage_capacity"])
    requested_stages = validate_stage_declaration(timing)
    if (capacity <= 0 or stage_capacity < len(requested_stages)
            or metadata["feedback_query_flags"] != 0):
        raise ValueError("Raw feedback requires declared stages and complete ordered queries; output-only is probe-only")
    frequency, finished = int(timing["qpc_frequency"]), int(timing["finished_qpc"])
    drain = timing["feedback_drain"]
    begin, end, deadline = (int(drain[k]) for k in ("begin_qpc", "end_qpc", "deadline_qpc"))
    errors = []

    def error(reason, **identity):
        errors.append({**identity, "reason": reason})

    def unsigned(row, fields):
        return all(0 <= row[k] < (1 << 64) for k in fields)

    record_map, stage_map, property_map = {}, defaultdict(list), {}
    for r in feedback:
        key = (r["query_id"], r["record_index"])
        if key in record_map:
            error("duplicate raw feedback record index", query_id=key[0], record_index=key[1])
        record_map[key] = r
    records_by_query = defaultdict(list)
    for (identifier, _), r in record_map.items():
        records_by_query[identifier].append(r)
    for s in stages:
        key = (s["query_id"], s["record_index"])
        stage_map[key].append(s)
        if key not in record_map:
            error("orphan raw stage", query_id=key[0], record_index=key[1])
    for p in properties:
        if p["query_id"] in property_map:
            error("duplicate timing properties query", query_id=p["query_id"])
        property_map[p["query_id"]] = p

    present_map = {(p["view"], p["generation"], p["present_id"]): p for p in presents}
    consumed, query_ids, known_properties, last_present_ids = set(), set(), {}, {}
    last_properties_observed = {}
    last_after, last_phase = 0, -1
    phases = {name: i for i, name in enumerate(("waiting", "calibration", "warmup", "stable", "inputs", "drain"))}
    for index, q in enumerate(queries):
        identifier, view = q["id"], q["view"]
        query_ids.add(identifier)
        d = domains.get(view)
        if (identifier != index+1 or not d or q["generation"] != d["generation"]
                or q["capacity"] != capacity or q["flags"] != 0):
            error("invalid query identity, capacity, or flags", query_id=identifier)
        phase = phases.get(q["phase"], -1)
        if (not 0 < q["before_qpc"] <= q["after_qpc"] <= finished
                or q["before_qpc"] < last_after or phase < last_phase or phase < 0):
            error("invalid query QPC or phase order", query_id=identifier)
        if ((q["phase"] == "drain" and not begin <= q["before_qpc"] <= q["after_qpc"] <= end)
                or (q["phase"] != "drain" and begin > 0 and q["after_qpc"] > begin)):
            error("query crosses feedback drain boundary", query_id=identifier)
        if q["phase"] == "drain" and q["before_qpc"] >= deadline:
            error("feedback query began at or after the drain deadline", query_id=identifier)
        last_after, last_phase = q["after_qpc"], phase
        rows = records_by_query[identifier]
        valid_output = q["result"] in (0, 5)  # VK_SUCCESS / VK_INCOMPLETE.
        if q["output_valid"] != int(valid_output):
            error("query output validity disagrees with VkResult", query_id=identifier)
        if not valid_output:
            error("feedback query failed", query_id=identifier, result=q["result"])
            if rows or any(q[k] for k in ("returned_count", "time_domains_counter", "timing_properties_counter")):
                error("undefined failed-query outputs were interpreted", query_id=identifier)
            continue
        if (not 0 <= q["returned_count"] <= capacity
                or len(rows) != q["returned_count"]
                or {r["record_index"] for r in rows} != set(range(q["returned_count"]))):
            error("raw record count or indices disagree with query", query_id=identifier)
        if (not d or q["time_domains_counter"] != d["time_domains_counter"]
                or not unsigned(q, ("time_domains_counter", "timing_properties_counter"))):
            error("query clock domains changed or counters invalid", query_id=identifier)
        if q["timing_properties_counter"] < last_properties_observed.get(view, 0):
            error("timing properties counter moved backwards", query_id=identifier)
        last_properties_observed[view] = q["timing_properties_counter"]
        for r in sorted(rows, key=lambda r: r["record_index"]):
            key = (identifier, r["record_index"])
            identity = (r["view"], r["generation"], r["present_id"])
            p = present_map.get(identity)
            if (r["view"] != view or r["generation"] != q["generation"]
                    or r["queried_qpc"] != q["after_qpc"] or not p or p["result"] != 0
                    or (p and p["leave_qpc"] > q["before_qpc"]) or identity in consumed
                    or r["present_id"] <= last_present_ids.get(view, 0)):
                error("raw record identity, order, or query association invalid", query_id=identifier,
                      record_index=r["record_index"])
            last_present_ids[view] = r["present_id"]
            if r["complete"] == 1:
                consumed.add(identity)  # A complete zero timestamp still consumes the record.
            if (r["complete"] != 1 or r["target_time"] != 0 or r["stage_count"] != len(requested_stages)
                    or r["stage_count"] > stage_capacity or not d
                    or r["time_domain"] != d["time_domain"] or r["time_domain_id"] != d["time_domain_id"]):
                error("raw record completeness, target, stage count, or clock invalid", query_id=identifier,
                      record_index=r["record_index"])
            raw = stage_map[key]
            by_stage = {s["stage"]: s["time_ns"] for s in raw}
            if (len(raw) != r["stage_count"] or {s["stage_index"] for s in raw} != set(range(r["stage_count"]))
                    or len(by_stage) != len(raw) or set(by_stage) != requested_stages
                    or any(not unsigned(s, ("time_ns",)) for s in raw)
                    or by_stage.get(1, 0) != r["queue_end_ns"] or by_stage.get(4) != r["first_pixel_out_ns"]):
                error("raw stages missing, duplicated, or different from derived feedback", query_id=identifier,
                      record_index=r["record_index"])
        p = property_map.get(identifier)
        expected = known_properties.get(view) != q["timing_properties_counter"]
        if (p is not None) != expected:
            error("missing or unexpected timing properties query", query_id=identifier)
        if p is not None:
            if (p["view"] != view or p["generation"] != q["generation"]
                    or not q["after_qpc"] <= p["before_qpc"] <= p["after_qpc"] <= finished
                    or (index+1 < len(queries) and p["after_qpc"] > queries[index+1]["before_qpc"])
                    or (q["phase"] == "drain" and p["after_qpc"] > end)
                    or p["output_valid"] != int(p["result"] == 0)
                    or not unsigned(p, ("counter", "refresh_duration", "refresh_interval"))):
                error("invalid timing properties identity, bracket, or output validity", query_id=identifier)
            if p["result"] == 0:
                # A counter can change between the feedback and property calls.
                if p["counter"] < last_properties_observed[view]:
                    error("timing properties counter moved backwards", query_id=identifier)
                last_properties_observed[view] = p["counter"]
                known_properties[view] = p["counter"]
            elif any(p[k] for k in ("counter", "refresh_duration", "refresh_interval")):
                error("undefined timing properties outputs were interpreted", query_id=identifier)
            if p["result"] not in (0, 1):  # VK_NOT_READY is a valid unavailable sample.
                error("timing properties query failed", query_id=identifier, result=p["result"])
    for identifier in {r["query_id"] for r in feedback} | set(property_map):
        if identifier not in query_ids:
            error("raw record or properties reference an unknown query", query_id=identifier)
    if not queries:
        error("raw feedback queries missing")
    pending = set(present_map)-consumed
    pending_views = []
    for view, d in domains.items():
        ids = sorted(k[2] for k in pending if k[:2] == (view, d["generation"]))
        if d["pending_present_ids"] != ids:
            error("pending IDs disagree with consumed complete records", view=view)
        pending_views.append({"view": view, "present_ids": ids})
    if (not 0 < begin <= end <= finished or drain["timeout_seconds"] != 2
            or deadline != begin+2*frequency
            or not isinstance(drain["complete"], bool) or drain["complete"] != (not pending)):
        error("invalid feedback drain lifecycle or completion state")
    if (not drain["complete"] and end-begin < 2*frequency):
        error("feedback drain stopped before its timeout")
    if any(f["phase"] in ("drain", "done") or f["end_qpc"] > begin for f in frames) or any(p["leave_qpc"] > begin for p in presents):
        error("new frame or presentation during feedback drain")
    expected_input = timing["trials_per_kind"]*2
    requests = timing["inputs"]
    last_frame = max(frames, key=lambda f: f["frame"], default=None)
    last_request = next((r for r in requests if r["id"] == expected_input), None)
    if (len(requests) != expected_input or {r["id"] for r in requests} != set(range(1, expected_input+1))
            or not last_frame or not last_request or last_frame["input_id"] != expected_input
            or (last_request and last_frame and (last_request["handled_qpc"] > last_frame["begin_qpc"]
                                                 or last_frame["epoch_ns"] != last_request["target_epoch_ns"]
                                                 or (last_request["kind"] == "selection" and last_frame["selected_index"] != last_request["target_index"])))
            or (last_frame and any(not any(p["frame"] == last_frame["frame"] and p["view"] == view
                                          and p["result"] == 0 for p in presents) for view in (0, 1)))):
        error("feedback drain began before the final input was presented in both views")
    return {"instrumented": True, "valid": not errors, "errors": errors,
            "query_count": len(queries), "record_count": len(feedback), "stage_count": len(stages),
            "query_results": dict(Counter(q["result"] for q in queries)),
            "zero_stage_timestamps": dict(Counter(s["stage"] for s in stages if s["time_ns"] == 0)),
            "complete_records_without_output": sum(r["complete"] == 1 and r["first_pixel_out_ns"] == 0 for r in feedback),
            "pending_views": pending_views, "drain_complete": drain["complete"],
            "drain_deadline_qpc": deadline,
            "drain_elapsed_ms": (end-begin)*1000/frequency if 0 < begin <= end else None,
            "timing_properties": properties,
            "refresh_scope": "query-time swapchain properties; not historical per-record refresh"}
