"""Check binding/composition parity; this is not independent algorithm validation."""
import argparse
import csv
import io
import math
from pathlib import Path
import subprocess

import orbelle as ob


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--cpp-example", required=True, type=Path)
    args = parser.parse_args()
    output = subprocess.check_output([str(args.cpp_example.resolve())], text=True)
    rows = list(csv.DictReader(io.StringIO(output)))
    states = ob.make_circular_demo_session().sample(ob.demo_epochs()).states
    if len(rows) != len(states) or not states:
        raise RuntimeError("C++ and Python sample counts differ or are empty")
    for row, state in zip(rows, states):
        if int(row["tai_ns"]) != state.epoch.tai_nanoseconds:
            raise RuntimeError("C++ and Python epochs differ")
        for fields, values, tolerance in (
            (("x_m", "y_m", "z_m"), state.position_m, 1e-8),
            (("vx_mps", "vy_mps", "vz_mps"), state.velocity_mps, 1e-10),
        ):
            for field, value in zip(fields, values):
                if not math.isclose(float(row[field]), value, rel_tol=0.0, abs_tol=tolerance):
                    raise RuntimeError(f"C++ and Python disagree for {field} at {row['tai_ns']}")
    print(f"C++/Python parity passed for {len(states)} samples")


if __name__ == "__main__":
    main()
