"""Compare matched HPOP runs; report timing separately from trajectory differences."""
from __future__ import annotations

import argparse
from datetime import datetime, timedelta
import hashlib
import json
import math
from pathlib import Path
import statistics


def read(path):
    return json.loads(path.read_text(encoding="utf-8-sig"))


def norm(v):
    return math.hypot(*v)


def cross(a, b):
    return [a[1] * b[2] - a[2] * b[1], a[2] * b[0] - a[0] * b[2],
            a[0] * b[1] - a[1] * b[0]]


def statistics_for(vectors):
    lengths = [norm(value) for value in vectors]
    count = len(vectors)
    return {"maximum_norm": max(lengths),
            "rms_norm": math.sqrt(sum(v * v for v in lengths) / count),
            "end_norm": lengths[-1], "end_components": vectors[-1],
            "maximum_components": [max(abs(v[i]) for v in vectors) for i in range(3)],
            "rms_components": [math.sqrt(sum(v[i]**2 for v in vectors) / count) for i in range(3)]}


def difference(reference, actual):
    if len(reference) != len(actual) or not reference:
        raise ValueError("Sample counts disagree or are empty")
    position, velocity, rtn = [], [], []
    for old, new in zip(reference, actual, strict=True):
        if old["epoch_tai_ns"] != new["epoch_tai_ns"]:
            raise ValueError("Sample epochs disagree")
        dr = [b - a for a, b in zip(old["position_m"], new["position_m"], strict=True)]
        dv = [b - a for a, b in zip(old["velocity_mps"], new["velocity_mps"], strict=True)]
        if not all(math.isfinite(v) for v in dr + dv):
            raise ValueError("Nonfinite sample")
        radial = [v / norm(old["position_m"]) for v in old["position_m"]]
        normal = cross(old["position_m"], old["velocity_mps"])
        normal = [v / norm(normal) for v in normal]
        transverse = cross(normal, radial)
        rtn.append([sum(d * u for d, u in zip(dr, axis, strict=True))
                    for axis in (radial, transverse, normal)])
        position.append(dr)
        velocity.append(dv)
    return {"position_m": statistics_for(position), "velocity_mps": statistics_for(velocity),
            "position_rtn_m": statistics_for(rtn)}


def stk_samples(path, reference, start_utc):
    data = read(path)
    if data["frame"] != "ICRF" or data["units"] != "m,m/s" or len(data["time"]) != len(reference):
        raise ValueError("Unexpected STK axes, units or sample count")
    initial = datetime(*start_utc)
    result = []
    for i, (time, sample) in enumerate(zip(data["time"], reference, strict=True)):
        # These fixed cases have integral UTC seconds and do not cross a leap.
        whole, fraction = time.split(".")
        if any(digit != "0" for digit in fraction):
            raise ValueError("Unexpected fractional STK report epoch")
        at = datetime.strptime(whole, "%d %b %Y %H:%M:%S")
        seconds = (sample["epoch_tai_ns"] - reference[0]["epoch_tai_ns"]) / 1e9
        if at != initial + timedelta(seconds=seconds):
            raise ValueError("STK report grid differs from Orbelle")
        result.append({"epoch_tai_ns": sample["epoch_tai_ns"],
                       "position_m": [axis[i] for axis in data["position"]],
                       "velocity_mps": [axis[i] for axis in data["velocity"]]})
    return result


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    for name in ("before", "after", "stk", "output"):
        parser.add_argument("--" + name, required=True, type=Path)
    parser.add_argument("--inputs", type=Path, default=Path(__file__).resolve().parents[2] /
                        "tests/reference/propagators/hpop_performance.json")
    args = parser.parse_args()
    before, after, stk = [read(p / "profile.json") for p in (args.before, args.after, args.stk)]
    for field in ("inputs_sha256", "gravity_sha256", "eop_manifest_sha256", "ephemeris_manifest_sha256"):
        if before[field] != after[field]:
            raise ValueError(f"Changed Orbelle input: {field}")
    if after["inputs_sha256"].lower() != stk["inputs_sha256"].lower():
        raise ValueError("STK and Orbelle use different shared inputs")
    inputs = read(args.inputs)
    with args.inputs.open("rb") as stream:
        if hashlib.file_digest(stream, "sha256").hexdigest() != after["inputs_sha256"]:
            raise ValueError("Comparison input file does not match the measured fixture")
    row_key = lambda row: (row["name"], row["forces"])
    previous = {row_key(row): row for row in before["cases"]}
    external = {row_key(row): row for row in stk["cases"]}
    results = []
    for row in after["cases"]:
        key = row_key(row)
        old, comparison = previous[key], external[key]
        if old["initial_gcrf"] != row["initial_gcrf"] or comparison["initial_gcrf"] != row["initial_gcrf"]:
            raise ValueError(f"Changed initial state: {key}")
        filename = "-".join(key) + "-samples.json"
        old_samples = read(args.before / filename)
        new_samples = read(args.after / filename)
        reference = stk_samples(args.stk / filename, new_samples, inputs["epoch_utc"])
        external_ms = statistics.median(comparison["propagation_ms"])
        result = {"name": row["name"], "forces": row["forces"], "samples": len(new_samples),
                  "before_ms": old["median_ms"], "after_ms": row["median_ms"], "stk_ms": external_ms,
                  "speedup": old["median_ms"] / row["median_ms"], "after_over_stk": row["median_ms"] / external_ms,
                  "change": difference(old_samples, new_samples),
                  "before_vs_stk": difference(reference, old_samples),
                  "after_vs_stk": difference(reference, new_samples)}
        results.append(result)
        print(f"{key}: {old['median_ms']:.2f} -> {row['median_ms']:.2f} ms; "
              f"STK {external_ms:.2f} ms; change {result['change']['position_m']['maximum_norm']:.6g} m")
    report = {"stk_version": stk["version"], "inputs_sha256": after["inputs_sha256"],
              "orbelle_gravity_sha256": after["gravity_sha256"],
              "stk_gravity_sha256": stk["gravity_sha256"], "cases": results,
              "limitations": ["STK EOP, planetary ephemerides and integrator error norm are not fully matched.",
                              "Gravity file hashes are recorded separately; coefficient equality requires a documented comparison.",
                              "Times exclude application startup, model loading, UI publication and rendering."]}
    with args.output.open("x", encoding="utf-8") as stream:
        json.dump(report, stream, indent=2)
        stream.write("\n")


if __name__ == "__main__":
    main()
