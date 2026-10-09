"""Create an immutable, offline IERS 20 C04 0h dX/dY snapshot from a local source file."""
import argparse
from datetime import date
import hashlib
import json
import math
from pathlib import Path


def make_snapshot(source: Path, output: Path, start: date, end: date, snapshot_id: str,
                  source_url: str, retrieved_on: str) -> dict:
    raw = source.read_bytes()
    lines = raw.splitlines(keepends=True)
    header = b"".join(line for line in lines if line.startswith(b"#"))
    if (b"20 C04" not in header or b"sampled at 0h UTC" not in header or
            b'dX(")' not in header or b'dY(")' not in header):
        raise ValueError("Expected IERS 20 C04 0h UTC dX/dY format")
    if start < date(1984, 1, 1):
        raise ValueError("This interpolation requires C04 observed rates/CPO from 1984 onward")
    if end <= start or not snapshot_id or not source_url:
        raise ValueError("Choose a nonempty snapshot ID/source and at least two daily records")
    date.fromisoformat(retrieved_on)
    mjd_zero = date(1858, 11, 17)
    selected = []
    selected_mjds = []
    for line in lines:
        if not line.strip() or line.startswith(b"#"):
            continue
        fields = line.split()
        if len(fields) != 21:
            raise ValueError("C04 record must have exactly 21 columns")
        year, month, day, hour = map(int, fields[:4])
        calendar = date(year, month, day)
        numbers = list(map(float, fields[4:]))
        mjd = (calendar - mjd_zero).days
        if hour != 0 or numbers[0] != mjd or not all(map(math.isfinite, numbers)):
            raise ValueError("Invalid C04 date, MJD, epoch hour or numerical fields")
        if (any(abs(numbers[i]) >= 10 for i in (1, 2, 4, 5, 6, 7)) or
                abs(numbers[3]) >= 2 or abs(numbers[8]) >= 1 or
                any(value < 0 for value in numbers[9:])):
            raise ValueError("C04 parameter or uncertainty is outside the supported domain")
        if start <= calendar <= end:
            selected.append(line)
            selected_mjds.append(mjd)
    first, last = (start - mjd_zero).days, (end - mjd_zero).days
    if last > 100000:
        raise ValueError("Requested coverage exceeds the supported MJD domain")
    if selected_mjds != list(range(first, last + 1)):
        raise ValueError("Requested window has missing, duplicate or unordered daily records")
    payload = header + b"".join(selected)
    manifest = {
        "schema_version": 1,
        "snapshot_id": snapshot_id,
        "format": "iers-20-c04-0h-dxdy",
        "data_file": "eop.c04",
        "sha256": hashlib.sha256(payload).hexdigest(),
        "source_url": source_url,
        "source_sha256": hashlib.sha256(raw).hexdigest(),
        "retrieved_on": retrieved_on,
        "first_mjd": first,
        "last_mjd": last,
        "reference_frame": "ITRF2020",
        "interpolation": "c04-hermite-tai-v1",
    }
    # Existing snapshots are never replaced, including partially created output directories.
    output.mkdir(parents=True, exist_ok=False)
    (output / "eop.c04").write_bytes(payload)
    (output / "manifest.json").write_text(json.dumps(manifest, indent=2) + "\n", encoding="utf-8")
    return manifest


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--input", type=Path, required=True)
    parser.add_argument("--output-dir", type=Path, required=True)
    parser.add_argument("--start", type=date.fromisoformat, required=True)
    parser.add_argument("--end", type=date.fromisoformat, required=True,
                        help="Last record's UTC midnight, inclusive (not the entire last day)")
    parser.add_argument("--snapshot-id", required=True)
    parser.add_argument("--source-url", required=True)
    parser.add_argument("--retrieved-on", required=True)
    args = parser.parse_args()
    manifest = make_snapshot(args.input, args.output_dir, args.start, args.end, args.snapshot_id,
                             args.source_url, args.retrieved_on)
    print(json.dumps(manifest, indent=2))


if __name__ == "__main__":
    main()
