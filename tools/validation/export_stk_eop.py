"""Export daily finals2000A knots to an explicit STK IAU 2006 comparison file.

The source snapshot is validated by Orbelle. LOD uses its UT1 interpolation
slopes; dPsi/dEps are zero because this comparison uses dX/dY and IAU 2006.
This does not replace STK's global EOP file or make the interpolators identical.
"""
import argparse
import json
import math
from pathlib import Path

from measure_hpop_orbelle import load_native, sha256


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--eop", type=Path, required=True, help="finals2000A manifest")
    parser.add_argument("--output", type=Path, required=True)
    parser.add_argument("--native", type=Path)
    args = parser.parse_args()
    if args.output.exists():
        parser.error("Output already exists")
    ob = load_native(args.native)
    eop = ob.load_eop_snapshot(args.eop)
    time = ob.make_builtin_time_provider()
    manifest = json.loads(args.eop.read_text(encoding="utf-8"))
    source = args.eop.parent / manifest["data_file"]
    observed, predicted = [], []
    arcseconds = 180 * 3600 / math.pi
    for row in source.read_text(encoding="ascii").splitlines():
        year = int(row[:2])
        year += 2000 if year < 60 else 1900
        month, day, mjd = int(row[2:4]), int(row[4:6]), int(float(row[7:15]))
        epoch = time.from_utc(ob.UtcDateTime(year, month, day, 0, 0, 0, 0))
        state = eop.at(epoch)
        sample = state.sample
        dat = time.tai_minus_utc_seconds(epoch)
        dut1 = sample.ut1_minus_tai_seconds + dat
        lod = -86400 * state.rates.ut1_minus_tai_seconds_per_second
        text = (f"{year:4d} {month:02d} {day:02d} {mjd:5d} "
                f"{sample.polar_motion_x_rad * arcseconds:9.6f} "
                f"{sample.polar_motion_y_rad * arcseconds:9.6f} "
                f"{dut1:10.7f} {lod:10.7f} {0:9.6f} {0:9.6f} "
                f"{sample.celestial_pole_dx_rad * arcseconds:9.6f} "
                f"{sample.celestial_pole_dy_rad * arcseconds:9.6f} {dat:3d}")
        (predicted if "P" in (row[16], row[57], row[95]) else observed).append(text)
    lines = ["VERSION 1.1", "# Orbelle finals2000A comparison; IAU 2006 dX/dY.",
             f"# Source manifest SHA256: {sha256(args.eop)}",
             f"# Source data SHA256: {sha256(source)}",
             f"NUM_OBSERVED_POINTS {len(observed)}", "BEGIN OBSERVED", *observed,
             "END OBSERVED", f"NUM_PREDICTED_POINTS {len(predicted)}", "BEGIN PREDICTED",
             *predicted, "END PREDICTED", ""]
    with args.output.open("x", encoding="ascii", newline="\n") as output:
        output.write("\n".join(lines))


if __name__ == "__main__":
    main()
