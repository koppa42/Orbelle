"""Validate the fixed headless workload before computing any throughput statistics."""

import argparse
import json
import math
from pathlib import Path
import re
import statistics


PROFILES = {
    "baseline": dict(objects=10000, epochs=601, spice_epochs_per_body=10000,
                     context_switches=200, warmup_rounds=1, measured_rounds=5),
    "smoke": dict(objects=8, epochs=7, spice_epochs_per_body=16,
                  context_switches=8, warmup_rounds=1, measured_rounds=2),
}
LIMITS = {
    "application_sample": {"max_radius_error_m": 1e-8, "max_energy_error_m2_s2": 1e-7,
                           "max_momentum_error_m2_s": 1e-4, "max_phase_position_m": 1e-7,
                           "max_phase_velocity_mps": 1e-9},
    "frame_apply": {"max_roundtrip_position_m": 5e-8, "max_roundtrip_velocity_mps": 5e-10},
}
EXPECTED_DATA = {
    "eop_id": "iers-c04-p0-tai-zero-v1",
    "eop_sha256": "7c49148548609953431d1c6063d08d331f869edfc057cdd74d9a465a94c1f261",
    "eop_interpolation": "c04-hermite-tai-v1", "eop_reference_frame": "ITRF2020",
    "time_id": "iers-bulletin-c-72-2026-07",
    "time_sha256": "6cb6f5d4b819f2e568e25db4b0b26d89dedf031fdffb18bc94d40f4e94e268d7",
    "spice_id": "orbelle-de440s-2016-leap-v1",
    "spice_manifest_sha256": "3a2e2a06b00232e386e57d7f654956b3bad21f1234a3088e09d8c8f482ab5e21",
    "spice_toolkit_version": "CSPICE_N0067",
    "spice_model": "DE440s-derived-Earth-Moon-Sun; SPICE J2000 with DE440 ICRF axes",
    "spice_first_tai_ns": 536155236000000000, "spice_last_tai_ns": 536760037000000000,
    "frame_model": "erfa-2.0.1-iau2006-2000a-cpo-model-s-cip-cheb8-v1",
    "frame_derivative_model": "analytic-era-plus-polar-cip-fd4-1s-local-tangent",
    "kernels": [
        {"file": "naif0012.tls",
         "sha256": "678e32bdb5a744117a467cd9601cd6b373f0e9bc9bbde1371d5eee39600a039b"},
        {"file": "orbelle_de440s_2016_leap.bsp",
         "sha256": "3bdfc2c0d92beeec54b653776f58ca6ef07ddf356bc3924e74028a14cfa7f9f6"}],
}


def require(condition, message):
    if not condition:
        raise ValueError(message)


def integer(value, name, minimum=0):
    require(type(value) is int and value >= minimum, f"Invalid integer: {name}")
    return value


def analyze(report, expected_profile=None):
    require(report.get("schema") == 1 and report.get("protocol") == "p0-compute-v1",
            "Unsupported computation protocol")
    require(report.get("valid") is True and "error" not in report, "Run did not succeed")
    profile = report.get("profile")
    require(profile in PROFILES and (expected_profile is None or profile == expected_profile),
            "Unexpected profile")
    workload = PROFILES[profile]
    require(report.get("workload") == workload, "Workload differs from the fixed protocol")
    for key, value in report["workload"].items():
        integer(value, key, 1)
    build = report["build"]
    require(isinstance(build.get("compiler"), str) and build["compiler"], "Missing compiler")
    require(type(build.get("sanitizers")) is bool, "Missing sanitizer mode")
    if profile == "baseline":
        require(build.get("configuration") == "Release" and not build["sanitizers"],
                "Baseline requires Release without sanitizers")
    clocks = report["clocks"]
    require(clocks.get("wall") == "std::chrono::steady_clock" and
            clocks.get("steady") is True and clocks.get("unit") == "ns", "Unexpected wall clock")
    integer(clocks.get("wall_period_num"), "wall_period_num", 1)
    integer(clocks.get("wall_period_den"), "wall_period_den", 1)
    require(clocks.get("cpu") in {
        "GetProcessTimes (100 ns representation; scheduler accounting granularity)",
        "CLOCK_PROCESS_CPUTIME_ID"}, "Unexpected CPU clock")
    data = report["data"]
    for key, expected in EXPECTED_DATA.items():
        require(type(data.get(key)) is type(expected) and data[key] == expected,
                f"Missing or changed frozen data: {key}")
    eop_source = (f"{data['eop_id']};sha256={data['eop_sha256']};time-sha256={data['time_sha256']};"
                  "first-tai-ns=-43168000000000;last-tai-ns=43232000000000;"
                  "interpolation=c04-hermite-tai-v1;subdaily-tides=none")
    spice_source = (f"manifest-sha256={data['spice_manifest_sha256']};"
                    "toolkit=CSPICE_N0067;frame=J2000;abcorr=NONE;velocity-scale=TDB")
    require(data.get("eop_source_id") == eop_source and data.get("spice_source_id") == spice_source,
            "Missing or changed full provenance")
    integer(data.get("provider_setup_ns"), "provider_setup_ns", 1)
    operations = dict(application_sample=workload["objects"] * workload["epochs"],
                      frame_build=workload["epochs"],
                      frame_apply=workload["objects"] * workload["epochs"],
                      spice_scalar=2 * workload["spice_epochs_per_body"],
                      spice_batch=2 * workload["spice_epochs_per_body"],
                      spice_context_switch=workload["context_switches"])
    samples = report["samples"]
    rounds = workload["measured_rounds"] + 1
    require(len(samples) == len(operations) * rounds, "Missing or extra samples")
    last_end = last_cpu_end = 0
    digests = {}
    summaries = {}
    for case_index, (case, count) in enumerate(operations.items()):
        wall_seconds, cpu_seconds, peaks = [], [], []
        for iteration in range(rounds):
            sample = samples[case_index * rounds + iteration]
            require(sample.get("case") == case and sample.get("iteration") == iteration and
                    sample.get("phase") == ("warmup" if iteration == 0 else "measure"),
                    "Unexpected case, phase or iteration order")
            integer(sample.get("iteration"), "iteration")
            require(integer(sample.get("operations"), "operations", 1) == count,
                    "Incorrect operation count")
            require(sample.get("valid") is True, "A sample failed validation")
            begin = integer(sample.get("begin_ns"), "begin_ns")
            end = integer(sample.get("end_ns"), "end_ns")
            cpu_begin = integer(sample.get("cpu_begin_ns"), "cpu_begin_ns")
            cpu_end = integer(sample.get("cpu_end_ns"), "cpu_end_ns")
            require(end > begin >= last_end and cpu_end >= cpu_begin >= last_cpu_end,
                    "Invalid or overlapping clock interval")
            last_end, last_cpu_end = end, cpu_end
            checks = sample["checks"]
            digest = checks.get("digest")
            require(isinstance(digest, str) and re.fullmatch(r"[0-9a-f]{16}", digest),
                    "Missing result digest")
            require(case not in digests or digest == digests[case], "Non-deterministic results")
            digests[case] = digest
            for key, limit in LIMITS.get(case, {}).items():
                value = checks.get(key)
                require(type(value) in (int, float) and math.isfinite(value) and 0 <= value <= limit,
                        f"Numerical check failed: {key}")
            peak = integer(sample.get("process_lifetime_peak_rss_bytes"), "peak RSS", 1)
            if iteration:
                wall_seconds.append((end - begin) / 1e9)
                cpu_seconds.append((cpu_end - cpu_begin) / 1e9)
                peaks.append(peak)
        median = statistics.median(wall_seconds)
        summaries[case] = {"operations_per_round": count,
                           "wall_seconds": {"min": min(wall_seconds), "median": median,
                                            "max": max(wall_seconds)},
                           "cpu_seconds_median": statistics.median(cpu_seconds),
                           "operations_per_second": count / median,
                           "process_lifetime_peak_rss_bytes": max(peaks),
                           "digest": digests[case]}
    require(digests["spice_scalar"] == digests["spice_batch"], "Batch/scalar outputs differ")
    return {"valid": True, "profile": profile, "performance_baseline": profile == "baseline",
            "acceptance": None, "cases": summaries,
            "limitations": ["No agreed throughput or memory acceptance thresholds",
                            "Single process and calling thread; warm allocator and OS caches",
                            "RSS is a cumulative process peak, including retained results and validation",
                            "Numerical checks here are not independent algorithm validation"]}


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("report", type=Path)
    parser.add_argument("--profile", choices=PROFILES)
    args = parser.parse_args()
    try:
        print(json.dumps(analyze(json.loads(args.report.read_text(encoding="utf-8")), args.profile),
                         indent=2, allow_nan=False))
        return 0
    except (ValueError, KeyError, TypeError, OSError) as error:
        print(json.dumps({"valid": False, "error": str(error)}))
        return 1


if __name__ == "__main__":
    raise SystemExit(main())
