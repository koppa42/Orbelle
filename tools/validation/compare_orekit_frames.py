"""Compare the installed OrbitForge frame API against independent Orekit celestial series."""
import argparse
import hashlib
import json
import math
import os
from pathlib import Path
import subprocess

import orbitforge as of

MATRIX_TOLERANCE = 2e-11  # ~4 microarcseconds; <1 mm at GEO for a single-axis error.
VELOCITY_TOLERANCE = 2e-5  # m/s for r <= 42164 km, v <= 8 km/s.


def sample_values(sample):
    return [sample.ut1_minus_tai_seconds, sample.polar_motion_x_rad,
            sample.polar_motion_y_rad, sample.celestial_pole_dx_rad, sample.celestial_pole_dy_rad]


def rate_values(rates):
    return [rates.ut1_minus_tai_seconds_per_second, rates.polar_motion_x_rad_per_second,
            rates.polar_motion_y_rad_per_second, rates.celestial_pole_dx_rad_per_second,
            rates.celestial_pole_dy_rad_per_second]


def product(matrix, vector):
    return [sum(matrix[3*i+j] * vector[j] for j in range(3)) for i in range(3)]


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--jar-dir", type=Path, required=True)
    parser.add_argument("--work-dir", type=Path, required=True)
    parser.add_argument("--manifest", type=Path, required=True)
    args = parser.parse_args()
    reference = Path(__file__).resolve().parent / "orekit"
    dependencies = json.loads((reference / "dependencies.json").read_text())
    jars = []
    for dependency in dependencies:
        path = args.jar_dir / dependency["filename"]
        if hashlib.sha256(path.read_bytes()).hexdigest() != dependency["sha256"]:
            raise ValueError(f"Reference JAR hash mismatch: {path}")
        jars.append(str(path.resolve()))
    args.work_dir.mkdir(parents=True, exist_ok=True)
    classpath = os.pathsep.join(jars)
    subprocess.run(["javac", "-cp", classpath, "-d", str(args.work_dir),
                    str(reference / "OrbitForgeFrameReference.java")], check=True)
    eop = of.load_eop_snapshot(args.manifest)
    first, last = eop.data_info.first_epoch.tai_nanoseconds, eop.data_info.last_epoch.tai_nanoseconds
    cases = [eop.at(of.Epoch(first + (last-first)*i//32)) for i in range(33)]
    # The measured snapshot covers one week; add explicitly synthetic dates to span the model.
    time = of.make_builtin_time_provider()
    for year in (1984, 2000, 2026):
        epoch = time.from_utc(of.UtcDateTime(year, 7, 1, 12))
        sample = of.EarthOrientationSample(epoch, -time.tai_minus_utc_seconds(epoch)+0.1,
                                           1e-6, -2e-6, 1e-9, -2e-9, f"synthetic-{year}")
        cases.append(of.EarthOrientationState(sample, of.EarthOrientationRates(-1e-8, 1e-13, -2e-13, 2e-15, -1e-15)))
    rows = []
    shifts = (0, -8, -4, -2, 2, 4, 8)
    for case in cases:
        values, rates = sample_values(case.sample), rate_values(case.rates)
        for dt in shifts:
            inputs = [case.sample.epoch.tai_nanoseconds + dt*1_000_000_000]
            inputs += [v + dt*r for v, r in zip(values, rates)]
            rows.append(",".join(map(str, inputs)))
    run = subprocess.run(["java", "-cp", str(args.work_dir.resolve()) + os.pathsep + classpath,
                          "OrbitForgeFrameReference"], input="\n".join(rows)+"\n",
                         text=True, capture_output=True, check=True)
    reference_rows = [[float(x) for x in line.split(",")] for line in run.stdout.splitlines()]
    if len(reference_rows) != len(rows) or any(len(row) != 9 for row in reference_rows):
        raise ValueError("Incomplete independent reference output")
    if not all(math.isfinite(v) for row in reference_rows for v in row):
        raise ValueError("Nonfinite independent reference output")
    frames = of.make_frame_provider()
    max_matrix = max_velocity = max_step_change = 0.0
    for i, case in enumerate(cases):
        group = dict(zip(shifts, reference_rows[i*len(shifts):(i+1)*len(shifts)]))
        transform = frames.gcrf_to_itrf_state(case.sample.epoch, case)
        actual = [value for row in transform.rotation.matrix for value in row]
        max_matrix = max(max_matrix, max(abs(a-b) for a, b in zip(actual, group[0])))
        for position in ([7_000_000.0, 0.0, 0.0], [0.0, 42_164_000.0, 0.0],
                         [0.0, 0.0, 42_164_000.0]):
            velocity = [1000.0, -2000.0, 6000.0]
            state = of.CartesianState(case.sample.epoch, of.OriginId.EARTH, of.FrameId.GCRF,
                                      position, velocity)
            actual_v = transform.apply(state).velocity_mps
            reference_v = []
            for h in (2, 4):
                derivative = [(8*(group[h][k]-group[-h][k]) -
                               (group[2*h][k]-group[-2*h][k]))/(12*h) for k in range(9)]
                rotated = product(group[0], velocity)
                changing = product(derivative, position)
                reference_v.append([a+b for a, b in zip(rotated, changing)])
                max_velocity = max(max_velocity, max(abs(a-b) for a, b in zip(actual_v, reference_v[-1])))
            max_step_change = max(max_step_change, max(abs(a-b) for a,b in zip(*reference_v)))
    report = {"reference": "Orekit 13.1.2 / Hipparchus 4.0.2, independent IERS series, model s",
              "cases": len(cases), "matrices": len(rows),
              "snapshot_sha256": eop.data_info.sha256,
              "input_sha256": hashlib.sha256(("\n".join(rows)+"\n").encode()).hexdigest(),
              "reference_dependencies": dependencies,
              "max_matrix_element_error": max_matrix, "matrix_tolerance": MATRIX_TOLERANCE,
              "max_velocity_component_error_mps": max_velocity,
              "velocity_tolerance_mps": VELOCITY_TOLERANCE,
              "reference_step_2_vs_4_seconds_max_delta_mps": max_step_change}
    (args.work_dir / "report.json").write_text(json.dumps(report, indent=2)+"\n")
    (args.work_dir / "input.csv").write_text("\n".join(rows)+"\n")
    (args.work_dir / "reference.csv").write_text(run.stdout)
    print(json.dumps(report, indent=2))
    if max_matrix > MATRIX_TOLERANCE:
        raise RuntimeError("Independent rotation comparison failed")
    if max_velocity > VELOCITY_TOLERANCE:
        raise RuntimeError("Independent state velocity comparison failed")
    if max_step_change > VELOCITY_TOLERANCE:
        raise RuntimeError("Reference derivative did not converge")


if __name__ == "__main__":
    main()
