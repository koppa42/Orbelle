"""Adversarial timing records: exact identities, independent clocks and gaps."""
from copy import deepcopy
import unittest

from analyze_vulkan_timing import analyze, percentiles


def fixture():
    base = 6_500_000_000_000  # QPC ticks, deliberately too large for naive uint64 ns multiplication.
    frames, presents, feedback = [], [], []
    for i in range(7):
        q = base+i*200_000  # 20 ms frames at QPC frequency 10 MHz.
        frames.append(dict(frame=i, phase="warmup" if i == 0 else "stable" if i < 3 else "inputs",
                           begin_qpc=q, update_qpc=q+5_000, submit_qpc=q+10_000, end_qpc=q+20_000,
                           epoch_ns=300 if i >= 3 else i, input_id=1 if 3 <= i <= 4 else 2 if i >= 5 else 0,
                           selected_index=42 if i >= 5 else -1))
        for view in (0, 1):
            presents.append(dict(frame=i, view=view, generation=1, present_id=i+1,
                                 enter_qpc=q+10_000, leave_qpc=q+20_000, result=0))
            feedback.append(dict(view=view, generation=1, present_id=i+1, queried_qpc=q+190_000,
                                 time_domain=3, time_domain_id=20+view, complete=1,
                                 queue_end_ns=(q+30_000)*100,
                                 first_pixel_out_ns=(q+60_000+view*20_000)*100))
    timing = dict(schema_version=2, qpc_frequency=10_000_000, profile="synthetic-fixture",
                  completed=True, failure="", trials_per_kind=1, input_boundary="test",
                  presentation_timing={"views": [dict(view=v, generation=1, time_domain=3, time_domain_id=20+v) for v in (0, 1)]},
                  inputs=[dict(id=1, kind="cached-seek", posted_qpc=base+580_000, handled_qpc=base+590_000,
                               target_epoch_ns=300),
                          dict(id=2, kind="selection", posted_qpc=base+980_000, handled_qpc=base+990_000,
                               target_epoch_ns=300, target_index=42, view=0)])
    return frames, presents, feedback, timing


class TimingTest(unittest.TestCase):
    def test_dual_view_exact_ids_and_response_boundary(self):
        data = fixture()
        data[2].reverse()  # Feedback arrival order must not change the frame join.
        report = analyze(*data)
        self.assertTrue(report["timing_valid"])
        self.assertEqual(report["responses"]["cached-seek"]["p95_ms"], 10.0)  # Slower view.
        self.assertEqual(report["responses"]["selection"]["p95_ms"], 8.0)  # Clicked view.
        self.assertEqual(report["views"][0]["output_intervals_observed"]["count"], 1)
        self.assertEqual(report["views"][0]["output_intervals_observed"]["p95_ms"], 20.0)
        self.assertIsNone(report["acceptance_pass"])

    def test_reverse_output_order_is_rejected_before_sorting(self):
        data = fixture()
        for r in data[2]:
            r["queue_end_ns"] = 0
            r["queried_qpc"] += 1_000_000
        data[2][2]["first_pixel_out_ns"] = (data[0][0]["begin_qpc"]+800_000)*100
        data[2][4]["first_pixel_out_ns"] = (data[0][0]["begin_qpc"]+700_000)*100
        report = analyze(*data)
        self.assertFalse(report["timing_valid"])
        self.assertTrue(any("submission order" in e["reason"] for e in report["feedback_errors"]))

    def test_zero_output_is_unavailable_not_dropped(self):
        data = fixture()
        data[2][2]["first_pixel_out_ns"] = 0
        report = analyze(*data)
        self.assertFalse(report["timing_valid"])
        self.assertEqual(report["views"][0]["coverage"]["output_unavailable"], 1)
        self.assertNotIn("dropped", report["all_feedback_coverage"])

    def test_missing_stable_feedback_is_not_zero_latency(self):
        data = fixture()
        del data[2][2]
        report = analyze(*data)
        self.assertFalse(report["timing_valid"])
        self.assertEqual(report["views"][0]["coverage"]["pending"], 1)

    def test_missing_first_response_cannot_be_hidden_by_later_frame(self):
        data = fixture()
        data[2][6]["first_pixel_out_ns"] = 0
        report = analyze(*data)
        self.assertFalse(report["timing_valid"])
        self.assertEqual(report["responses"]["cached-seek"]["first_response_unverified"], 1)
        self.assertGreater(report["responses"]["cached-seek"]["p95_ms"], 10.0)

    def test_unknown_duplicate_and_foreign_clock_feedback(self):
        for modification in ("unknown", "duplicate", "clock"):
            with self.subTest(modification=modification):
                data = fixture()
                if modification == "unknown": data[2][0]["present_id"] = 90
                elif modification == "duplicate": data[2].append(deepcopy(data[2][0]))
                else: data[2][0]["time_domain_id"] = 99
                self.assertFalse(analyze(*data)["timing_valid"])

    def test_failed_present_and_duplicate_frame_rejected(self):
        data = fixture()
        data[1][2]["result"] = -1
        self.assertFalse(analyze(*data)["timing_valid"])
        data = fixture()
        data[0].append(deepcopy(data[0][0]))
        with self.assertRaises(ValueError): analyze(*data)

    def test_tick_values_mislabeled_nanoseconds_are_rejected(self):
        data = fixture()
        data[2][2]["first_pixel_out_ns"] //= 100
        self.assertFalse(analyze(*data)["timing_valid"])

    def test_distinct_stage_local_clocks_and_rejected_unused_sample(self):
        data = fixture()
        calibrations = []
        for d in data[3]["presentation_timing"]["views"]:
            d["time_domain"] = 1000208000
        for r in data[2]:
            r["time_domain"] = 1000208000
            for field, id_field, stage, offset in (("queue_end_ns", "queue_calibration_id", 1, 1_790_000_000_000_000_000),
                                                  ("first_pixel_out_ns", "output_calibration_id", 4, 5_000_000)):
                identifier = len(calibrations)+1
                q = r["queried_qpc"]
                calibrations.append(dict(id=identifier, view=r["view"], generation=1, time_domain=1000208000,
                                         time_domain_id=r["time_domain_id"], stage=stage, before_qpc=q-10,
                                         after_qpc=q+10, local_ns=q*100+offset,
                                         calibrated_qpc=q, max_deviation_ns=100))
                r[field] += offset
                r[id_field] = identifier
        unused = deepcopy(calibrations[0])
        unused.update(id=999, max_deviation_ns=0)
        calibrations.append(unused)
        report = analyze(*data, calibrations)
        self.assertTrue(report["timing_valid"])
        self.assertEqual(report["rejected_calibration_samples"], [999])
        self.assertEqual(report["responses"]["cached-seek"]["p95_ms"], 10.0)
        data[2][2]["output_calibration_id"] = 999
        self.assertFalse(analyze(*data, calibrations)["timing_valid"])

    def test_percentiles_nearest_rank(self):
        self.assertEqual(percentiles(list(range(1, 101)))["p95_ms"], 95)
        self.assertIsNone(percentiles([])["p95_ms"])


def resource_fixture():
    data = fixture()
    frames, _, _, timing = data
    timing["schema_version"] = 3
    timing["finished_qpc"] = frames[-1]["end_qpc"]+100
    timing["gpu_timing"] = {"views": [dict(view=view, generation=1, queue_family=0,
                                             timestamp_valid_bits=64, timestamp_period_ns=1.0,
                                             query_slots=3, device_uuid_hex="01"*16,
                                             device_luid_hex="02"*8) for view in (0, 1)]}
    timing["video_memory"] = dict(source="IDXGIAdapter3::QueryVideoMemoryInfo",
                                   adapter_match="Vulkan deviceLUID == DXGI AdapterLuid",
                                   adapter_luid_hex="02"*8, device_uuid_hex="01"*16,
                                   node_mask=1, node_index=0, sample_interval_seconds=1)
    queries = []
    for i, f in enumerate(frames):
        for view in (0, 1):
            read_qpc = frames[i+3]["update_qpc"]+2 if i+3 < len(frames) else frames[-1]["end_qpc"]+10
            queries.append(dict(frame=i, view=view, generation=1, slot=i % 3, slot_use=i//3+1,
                                prepared_qpc=f["update_qpc"]+1, read_qpc=read_qpc,
                                begin_tick=i*100_000, end_tick=i*100_000+50_000,
                                begin_available=1, end_available=1, result=0))
    timing["resources"] = [dict(qpc=f["end_qpc"], phase=f["phase"],
                                 sample_begin_qpc=f["end_qpc"]+1, sample_end_qpc=f["end_qpc"]+10,
                                 video_memory={segment: dict(current_usage_bytes=123 if segment == "local" else 456,
                                                            budget_bytes=1000, current_reservation_bytes=0,
                                                            available_for_reservation_bytes=900)
                                               for segment in ("local", "non_local")},
                                 working_set_bytes=999, private_bytes=1000, peak_working_set_bytes=1001,
                                 cpu_percent_of_one_core=200 if f["frame"] == 1 else 50) for f in frames]
    return data, queries


class ResourceTest(unittest.TestCase):
    def test_complete_queries_and_process_memory(self):
        data, queries = resource_fixture()
        report = analyze(*data, gpu_queries=queries)
        self.assertTrue(report["measurements_valid"])
        self.assertEqual(report["gpu_timing"]["views"][0]["stable_gpu_span_ms"]["p95_ms"], 0.05)
        memory = report["process_resources"]
        self.assertEqual(memory["stable_video_memory_bytes"]["local"]["current_usage_bytes"]["max"], 123)
        self.assertEqual(memory["stable_video_memory_bytes"]["non_local"]["current_usage_bytes"]["max"], 456)
        self.assertEqual(memory["stable_cpu_percent_of_one_core"]["mean"], 50)  # Not the warmup delta of 200.
        self.assertIsNone(report["acceptance_pass"])

    def test_available_zero_tick_and_64_bit_wrap_are_valid(self):
        data, queries = resource_fixture()
        queries[2].update(begin_tick=(1 << 64)-50_000, end_tick=0)
        self.assertTrue(analyze(*data, gpu_queries=queries)["gpu_timing"]["valid"])

    def test_36_bit_single_wrap(self):
        data, queries = resource_fixture()
        data[3]["gpu_timing"]["views"][0]["timestamp_valid_bits"] = 36
        queries[2].update(begin_tick=(1 << 36)-50_000, end_tick=0)
        report = analyze(*data, gpu_queries=queries)
        self.assertTrue(report["gpu_timing"]["valid"])
        self.assertEqual(report["gpu_timing"]["views"][0]["stable_gpu_span_ms"]["p95_ms"], 0.05)

    def test_ambiguous_wrap_and_duration_outside_cpu_bound_are_rejected(self):
        for modification in ("multiple_wraps", "exceeds_bound"):
            with self.subTest(modification=modification):
                data, queries = resource_fixture()
                if modification == "multiple_wraps":
                    data[3]["gpu_timing"]["views"][0]["timestamp_valid_bits"] = 36
                    queries[-2]["read_qpc"] += 1_000_000_000  # 100 s > the 68.7 s wrap period.
                    data[3]["finished_qpc"] += 1_000_000_000
                else:
                    data[3]["gpu_timing"]["views"][0]["timestamp_period_ns"] = 1_000_000
                self.assertFalse(analyze(*data, gpu_queries=queries)["gpu_timing"]["valid"])

    def test_illegal_counter_bits_and_nonzero_upper_bits_are_rejected(self):
        for bits in (0, 1, 32, 35, 65):
            with self.subTest(bits=bits):
                data, queries = resource_fixture()
                data[3]["gpu_timing"]["views"][0]["timestamp_valid_bits"] = bits
                with self.assertRaises(ValueError): analyze(*data, gpu_queries=queries)
        data, queries = resource_fixture()
        data[3]["gpu_timing"]["views"][0]["timestamp_valid_bits"] = 36
        queries[2].update(begin_tick=(1 << 48)+(1 << 36)-50_000, end_tick=0)
        self.assertFalse(analyze(*data, gpu_queries=queries)["gpu_timing"]["valid"])

    def test_unavailable_duplicate_foreign_and_lost_queries_are_rejected(self):
        for modification in ("unavailable", "duplicate", "foreign", "lost", "failure"):
            with self.subTest(modification=modification):
                data, queries = resource_fixture()
                if modification == "unavailable": queries[2]["begin_available"] = 0
                elif modification == "duplicate": queries.append(deepcopy(queries[2]))
                elif modification == "foreign": queries[2]["generation"] = 2
                elif modification == "lost": del queries[2]
                else: queries[2]["result"] = 1
                report = analyze(*data, gpu_queries=queries)
                self.assertFalse(report["measurements_valid"])
                self.assertFalse(report["gpu_timing"]["valid"])
                self.assertTrue(report["timing_valid"])  # Presentation remains an independent metric.

    def test_stale_slot_result_and_query_outside_frame_are_rejected(self):
        for modification in ("slot_use", "readback_order", "prepared_qpc", "read_after_finish"):
            with self.subTest(modification=modification):
                data, queries = resource_fixture()
                if modification == "slot_use": queries[6]["slot_use"] = 1
                elif modification == "readback_order": queries[0]["read_qpc"] -= 50_000
                elif modification == "prepared_qpc": queries[2]["prepared_qpc"] = data[0][1]["end_qpc"]+1
                else: queries[-2]["read_qpc"] = data[3]["finished_qpc"]+1
                self.assertFalse(analyze(*data, gpu_queries=queries)["gpu_timing"]["valid"])

    def test_legacy_present_only_cannot_pass_all_measurements(self):
        report = analyze(*fixture())
        self.assertTrue(report["timing_valid"])
        self.assertFalse(report["measurements_valid"])

    def test_memory_requires_matching_adapter_node_and_valid_samples(self):
        for modification in ("uuid", "luid", "node", "missing", "negative", "phase"):
            with self.subTest(modification=modification):
                data, queries = resource_fixture()
                if modification in ("uuid", "luid", "node"):
                    field = {"uuid": "device_uuid_hex", "luid": "adapter_luid_hex", "node": "node_mask"}[modification]
                    data[3]["video_memory"][field] = 3 if modification == "node" else "ff"*(16 if modification == "uuid" else 8)
                    with self.assertRaises(ValueError): analyze(*data, gpu_queries=queries)
                else:
                    sample = data[3]["resources"][1]
                    if modification == "missing": del sample["video_memory"]["local"]["current_usage_bytes"]
                    elif modification == "negative": sample["video_memory"]["local"]["current_usage_bytes"] = -1
                    else: sample["phase"] = "drain"
                    report = analyze(*data, gpu_queries=queries)
                    self.assertFalse(report["process_resources"]["valid"])
                    self.assertFalse(report["measurements_valid"])

    def test_long_gap_or_absent_stable_resource_samples_cannot_pass(self):
        from analyze_graphics_resources import analyze_memory
        data, _ = resource_fixture()
        data[3]["resources"] = []
        self.assertFalse(analyze_memory(data[0], data[3])["valid"])
        data, _ = resource_fixture()
        # Shift the final stable frame and all following frames by 3 s, creating
        # a real coverage gap while retaining legal per-frame sample envelopes.
        data[3]["resources"] = data[3]["resources"][:3]
        for f in data[0][2:]:
            for field in ("begin_qpc", "update_qpc", "submit_qpc", "end_qpc"):
                f[field] += 30_000_000
        data[3]["finished_qpc"] += 30_000_000
        data[3]["resources"][2].update(qpc=data[0][2]["end_qpc"],
                                      sample_begin_qpc=data[0][2]["end_qpc"],
                                      sample_end_qpc=data[0][2]["end_qpc"]+10)
        self.assertFalse(analyze_memory(data[0], data[3])["valid"])

    def test_samples_outside_frame_or_stable_period_are_rejected(self):
        for modification in ("all_delayed", "cross_next_frame", "past_run_end"):
            with self.subTest(modification=modification):
                data, queries = resource_fixture()
                samples = data[3]["resources"]
                if modification == "all_delayed":
                    for r in samples:
                        r["sample_begin_qpc"] += 1_000_000_000
                        r["sample_end_qpc"] += 1_000_000_000
                elif modification == "cross_next_frame":
                    samples[2]["sample_end_qpc"] = data[0][3]["begin_qpc"]+1
                else:
                    samples[-1]["sample_end_qpc"] = data[3]["finished_qpc"]+1
                self.assertFalse(analyze(*data, gpu_queries=queries)["process_resources"]["valid"])

    def test_optional_nvidia_sampler_absence_and_start_failure(self):
        from unittest.mock import Mock, patch
        from run_graphics_timing import start_device_sampler
        for available in (False, True):
            with self.subTest(executable_found=available):
                record, log = {}, Mock()
                with patch("run_graphics_timing.shutil.which", return_value="nvidia-smi" if available else None), \
                     patch("run_graphics_timing.subprocess.Popen", side_effect=OSError("diagnostic unavailable")):
                    self.assertIsNone(start_device_sampler(log, record))
                self.assertEqual(record["device_gpu_diagnostic"]["status"], "unavailable")
                if not available: log.assert_not_called()


def raw_fixture():
    data, gpu = resource_fixture()
    frames, _, feedback, timing = data
    begin = frames[-1]["end_qpc"]+100
    end = feedback[-1]["queried_qpc"]+100
    timing.update(schema_version=4, finished_qpc=end+100,
                  feedback_drain=dict(begin_qpc=begin, end_qpc=end, deadline_qpc=begin+20_000_000,
                                       timeout_seconds=2, complete=True))
    timing["presentation_timing"].update(stage_mask=5, feedback_capacity_per_view=512,
                                          feedback_stage_capacity=4, feedback_query_flags=0)
    for d in timing["presentation_timing"]["views"]:
        d.update(time_domains_counter=7, pending_present_ids=[])
    queries, stages, properties = [], [], []
    for i, r in enumerate(feedback):
        qpc = r["queried_qpc"]-(10 if r["view"] == 0 else 0)
        r.update(query_id=i+1, record_index=0, target_time=0, stage_count=2, queried_qpc=qpc)
        phase = "drain" if qpc >= begin else frames[i//2]["phase"]
        queries.append(dict(id=i+1, view=r["view"], generation=1, phase=phase,
                            before_qpc=qpc-1, after_qpc=qpc, result=0, output_valid=1,
                            capacity=512, returned_count=1, flags=0, time_domains_counter=7,
                            timing_properties_counter=9))
        for j, (stage, field) in enumerate(((1, "queue_end_ns"), (4, "first_pixel_out_ns"))):
            stages.append(dict(query_id=i+1, record_index=0, stage_index=j, stage=stage, time_ns=r[field]))
        if i < 2:
            properties.append(dict(query_id=i+1, view=r["view"], generation=1, before_qpc=qpc+1,
                                   after_qpc=qpc+2, result=0, output_valid=1, counter=9,
                                   refresh_duration=16_666_667, refresh_interval=16_666_667))
    return data, dict(gpu_queries=gpu, feedback_queries=queries, feedback_stages=stages,
                     timing_properties=properties)


class RawFeedbackTest(unittest.TestCase):
    def test_complete_raw_feedback_and_real_drain(self):
        data, rows = raw_fixture()
        report = analyze(*data, **rows)
        self.assertTrue(report["measurements_valid"], report["raw_feedback"])
        self.assertTrue(report["raw_feedback"]["valid"])
        self.assertEqual(report["raw_feedback"]["record_count"], 14)
        self.assertEqual(report["raw_feedback"]["stage_count"], 28)
        self.assertTrue(report["raw_feedback"]["drain_complete"])

    def test_complete_zero_is_consumed_and_cannot_be_recovered_by_drain(self):
        data, rows = raw_fixture()
        data[2][2]["first_pixel_out_ns"] = 0
        rows["feedback_stages"][5]["time_ns"] = 0
        report = analyze(*data, **rows)
        self.assertTrue(report["raw_feedback"]["valid"])
        self.assertTrue(report["raw_feedback"]["drain_complete"])
        self.assertEqual(report["raw_feedback"]["zero_stage_timestamps"], {4: 1})
        self.assertEqual(report["raw_feedback"]["complete_records_without_output"], 1)
        self.assertFalse(report["timing_valid"])

    def test_raw_tables_cannot_be_forged_by_good_derived_timestamps(self):
        for mutation in ("missing_stage", "duplicate_stage", "wrong_stage", "wrong_time", "orphan_stage",
                         "missing_record", "duplicate_record", "missing_query", "record_count", "capacity",
                         "stage_count", "target", "partial", "invalid_bool", "foreign_query", "clock_counter",
                         "counter_bounds", "record_order", "stale_query", "duplicate_query", "flags", "bad_phase",
                         "pending_ids", "missing_properties", "property_result", "property_bracket"):
            with self.subTest(mutation=mutation):
                data, rows = raw_fixture()
                q, s, p = rows["feedback_queries"], rows["feedback_stages"], rows["timing_properties"]
                if mutation == "missing_stage": del s[0]
                elif mutation == "duplicate_stage": s.append(deepcopy(s[0]))
                elif mutation == "wrong_stage": s[0]["stage"] = 4
                elif mutation == "wrong_time": s[0]["time_ns"] += 1
                elif mutation == "orphan_stage": s[0]["record_index"] = 99
                elif mutation == "missing_record": del data[2][0]
                elif mutation == "duplicate_record": data[2].append(deepcopy(data[2][0]))
                elif mutation == "missing_query": del q[0]
                elif mutation == "record_count": q[0]["returned_count"] = 2
                elif mutation == "capacity": q[0]["returned_count"] = 513
                elif mutation == "stage_count": data[2][0]["stage_count"] = 5
                elif mutation == "target": data[2][0]["target_time"] = 1
                elif mutation in ("partial", "invalid_bool"): data[2][0]["complete"] = 0 if mutation == "partial" else 2
                elif mutation == "foreign_query": data[2][0]["query_id"] = 99
                elif mutation == "clock_counter": q[0]["time_domains_counter"] += 1
                elif mutation == "counter_bounds": q[0]["timing_properties_counter"] = -1
                elif mutation == "record_order": data[2][0]["record_index"] = 1
                elif mutation == "stale_query": q[0]["before_qpc"] = data[1][0]["enter_qpc"]-1
                elif mutation == "duplicate_query": q[1]["id"] = 1
                elif mutation == "flags": q[0]["flags"] = 1
                elif mutation == "bad_phase": q[0]["phase"] = "done"
                elif mutation == "pending_ids": data[3]["presentation_timing"]["views"][0]["pending_present_ids"] = [1]
                elif mutation == "missing_properties": del p[0]
                elif mutation == "property_result": p[0]["result"] = 1
                else: p[0]["after_qpc"] = data[3]["finished_qpc"]+1
                report = analyze(*data, **rows)
                self.assertFalse(report["raw_feedback"]["valid"])
                self.assertFalse(report["measurements_valid"])

    def test_drain_rejects_new_frames_early_exit_and_lost_final_input(self):
        for mutation in ("frame", "end", "early_timeout", "input", "query", "false_complete"):
            with self.subTest(mutation=mutation):
                data, rows = raw_fixture()
                drain = data[3]["feedback_drain"]
                if mutation == "frame": data[0][-1]["phase"] = "drain"
                elif mutation == "end": drain["begin_qpc"] = data[0][-1]["end_qpc"]-1
                elif mutation == "early_timeout": drain["complete"] = False
                elif mutation == "input": data[0][-1]["input_id"] = 1
                elif mutation == "query": drain["end_qpc"] = rows["feedback_queries"][-1]["before_qpc"]-1
                else: drain["complete"] = 1  # bool required, not truthy arbitrary JSON.
                self.assertFalse(analyze(*data, **rows)["raw_feedback"]["valid"])

    def test_pending_timeout_preserves_truthful_capture_but_rejects_measurement(self):
        data, rows = raw_fixture()
        del data[2][-1]
        del rows["feedback_stages"][-2:]
        rows["feedback_queries"][-1]["returned_count"] = 0
        d = data[3]
        d["presentation_timing"]["views"][1]["pending_present_ids"] = [7]
        d["feedback_drain"].update(complete=False, end_qpc=d["feedback_drain"]["begin_qpc"]+2*d["qpc_frequency"])
        d["finished_qpc"] = d["feedback_drain"]["end_qpc"]+100
        report = analyze(*data, **rows)
        self.assertTrue(report["raw_feedback"]["valid"], report["raw_feedback"])
        self.assertFalse(report["timing_valid"])
        self.assertFalse(report["raw_feedback"]["drain_complete"])

    def test_not_ready_refresh_retries_and_uint64_max_stays_unsigned(self):
        data, rows = raw_fixture()
        initial = rows["timing_properties"][0]
        later = deepcopy(initial)
        q = rows["feedback_queries"][2]
        later.update(query_id=q["id"], before_qpc=q["after_qpc"]+1, after_qpc=q["after_qpc"]+2,
                     refresh_interval=(1 << 64)-1)
        initial.update(result=1, output_valid=0, counter=0, refresh_duration=0, refresh_interval=0)
        rows["timing_properties"].append(later)
        report = analyze(*data, **rows)
        self.assertTrue(report["raw_feedback"]["valid"], report["raw_feedback"])
        self.assertEqual(report["raw_feedback"]["timing_properties"][-1]["refresh_interval"], (1 << 64)-1)

    def test_property_counter_can_change_between_adjacent_calls(self):
        data, rows = raw_fixture()
        rows["timing_properties"][0]["counter"] = 10
        for q in rows["feedback_queries"][2::2]: q["timing_properties_counter"] = 10
        self.assertTrue(analyze(*data, **rows)["raw_feedback"]["valid"])

    def test_failed_query_outputs_remain_undefined_and_fail_measurement(self):
        data, rows = raw_fixture()
        del data[2][-1]
        del rows["feedback_stages"][-2:]
        rows["feedback_queries"][-1].update(result=-4, output_valid=0, returned_count=0,
                                             time_domains_counter=0, timing_properties_counter=0)
        report = analyze(*data, **rows)
        self.assertFalse(report["measurements_valid"])
        self.assertTrue(any(e["reason"] == "feedback query failed" for e in report["raw_feedback"]["errors"]))
        self.assertFalse(any("undefined" in e["reason"] for e in report["raw_feedback"]["errors"]))
        rows["feedback_queries"][-1]["returned_count"] = 512
        report = analyze(*data, **rows)
        self.assertTrue(any("undefined" in e["reason"] for e in report["raw_feedback"]["errors"]))

    def test_refresh_counter_regression_is_rejected_across_both_query_kinds(self):
        for mutation in ("query_after_properties", "properties_after_query", "query_after_query"):
            with self.subTest(mutation=mutation):
                data, rows = raw_fixture()
                q, p = rows["feedback_queries"], rows["timing_properties"]
                later = deepcopy(p[0])
                later.update(query_id=q[2]["id"], before_qpc=q[2]["after_qpc"]+1, after_qpc=q[2]["after_qpc"]+2)
                if mutation == "query_after_properties":
                    p[0]["counter"] = 10
                    rows["timing_properties"].append(later)
                elif mutation == "properties_after_query":
                    p[0]["counter"] = 8
                    rows["timing_properties"].append(later)
                else:
                    p[0].update(result=1, output_valid=0, counter=0, refresh_duration=0, refresh_interval=0)
                    q[0]["timing_properties_counter"] = 10
                    rows["timing_properties"].append(later)
                report = analyze(*data, **rows)
                self.assertFalse(report["measurements_valid"])
                self.assertTrue(any("counter moved backwards" in e["reason"] for e in report["raw_feedback"]["errors"]))

    def test_response_frame_must_follow_handled_input_and_final_state(self):
        for mutation in ("handled_after_frame", "unknown_input", "final_epoch", "final_selection"):
            with self.subTest(mutation=mutation):
                data, rows = raw_fixture()
                if mutation == "handled_after_frame":
                    data[3]["inputs"][-1]["handled_qpc"] = data[0][-1]["end_qpc"]+1
                elif mutation == "unknown_input": data[0][0]["input_id"] = 999
                elif mutation == "final_epoch": data[0][-1]["epoch_ns"] += 1
                else: data[0][-1]["selected_index"] = 99
                report = analyze(*data, **rows)
                self.assertFalse(report["measurements_valid"])
                if mutation in ("handled_after_frame", "unknown_input"):
                    self.assertTrue(any("handled input" in e["reason"] for e in report["feedback_errors"]))
                if mutation != "unknown_input":
                    self.assertFalse(report["raw_feedback"]["valid"])

    def test_drain_deadline_limits_new_calls_but_allows_in_flight_return(self):
        for offset in (-1, 0, 1):
            with self.subTest(query_begin_relative_to_deadline=offset):
                data, rows = raw_fixture()
                drain = data[3]["feedback_drain"]
                q = rows["feedback_queries"][-1]
                q.update(before_qpc=drain["deadline_qpc"]+offset, after_qpc=drain["deadline_qpc"]+100)
                data[2][-1]["queried_qpc"] = q["after_qpc"]
                drain["end_qpc"] = q["after_qpc"]+10
                data[3]["finished_qpc"] = drain["end_qpc"]+100
                report = analyze(*data, **rows)
                self.assertEqual(report["raw_feedback"]["valid"], offset < 0, report["raw_feedback"])
        data, rows = raw_fixture()
        data[3]["feedback_drain"]["deadline_qpc"] += 1
        self.assertFalse(analyze(*data, **rows)["raw_feedback"]["valid"])

    def test_output_only_probe_still_requires_every_output_timestamp(self):
        data, rows = raw_fixture()
        data[3]["profile"] = "trace-probe"
        data[3]["presentation_timing"].update(stage_mask=4, output_only_probe=True)
        for r in data[2]: r.update(stage_count=1, queue_end_ns=0)
        rows["feedback_stages"] = [s for s in rows["feedback_stages"] if s["stage"] == 4]
        for s in rows["feedback_stages"]: s["stage_index"] = 0
        self.assertTrue(analyze(*data, **rows)["measurements_valid"])
        data[2][2]["first_pixel_out_ns"] = 0
        rows["feedback_stages"][2]["time_ns"] = 0
        report = analyze(*data, **rows)
        self.assertTrue(report["raw_feedback"]["valid"])
        self.assertFalse(report["measurements_valid"])
        self.assertEqual(report["views"][0]["coverage"]["output_unavailable"], 1)

    def test_probe_stage_declaration_cannot_hide_missing_or_extra_stages(self):
        for mutation in ("missing_flag", "extra_flag", "full_profile", "queue_only", "unrequested_stage", "invented_queue"):
            with self.subTest(mutation=mutation):
                data, rows = raw_fixture()
                data[3]["profile"] = "trace-probe"
                d = data[3]["presentation_timing"]
                d.update(stage_mask=4, output_only_probe=True)
                for r in data[2]: r.update(stage_count=1, queue_end_ns=0)
                rows["feedback_stages"] = [s for s in rows["feedback_stages"] if s["stage"] == 4]
                for s in rows["feedback_stages"]: s["stage_index"] = 0
                if mutation == "missing_flag": del d["output_only_probe"]
                elif mutation == "extra_flag": d["stage_mask"] = 5
                elif mutation == "full_profile": data[3]["profile"] = "timing-characterization-v1"
                elif mutation == "queue_only": d["stage_mask"] = 1
                elif mutation == "unrequested_stage": rows["feedback_stages"][0]["stage"] = 1
                else: data[2][0]["queue_end_ns"] = 123
                if mutation in ("unrequested_stage", "invented_queue"):
                    self.assertFalse(analyze(*data, **rows)["raw_feedback"]["valid"])
                else:
                    with self.assertRaises(ValueError): analyze(*data, **rows)

    def test_runner_rejects_output_only_full_before_launch_or_output_creation(self):
        from unittest.mock import patch
        from run_graphics_timing import main
        arguments = ["run_graphics_timing.py", "--executable", "not-used.exe",
                     "--dependency-prefix", "not-used", "--scenario", "not-used.json",
                     "--output", "not-created", "--profile", "full", "--output-only-probe"]
        with patch("sys.argv", arguments), patch("run_graphics_timing.subprocess.Popen") as launch, \
             patch("run_graphics_timing.Path.mkdir") as mkdir:
            with self.assertRaisesRegex(ValueError, "restricted to diagnostic probes"): main()
            launch.assert_not_called()
            mkdir.assert_not_called()

    def test_legacy_schema_cannot_bypass_output_only_probe_restriction(self):
        for schema in (2, 3):
            for profile in ("trace-probe", "timing-characterization-v1"):
                with self.subTest(schema=schema, profile=profile):
                    data, rows = raw_fixture()
                    data[3].update(schema_version=schema, profile=profile)
                    data[3]["presentation_timing"].update(stage_mask=4, output_only_probe=True)
                    with self.assertRaisesRegex(ValueError, "require schema 4"): analyze(*data, **rows)


if __name__ == "__main__":
    unittest.main()
