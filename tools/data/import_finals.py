"""Freeze a complete Bulletin A finals2000A interval; preserve original fixed-width rows."""
import argparse
from datetime import date
import hashlib
import json
import math
from pathlib import Path


def make_snapshot(source, output, start, end, source_url, retrieved_on):
    if not source_url.strip():
        raise ValueError("A source URL is required")
    raw = source.read_bytes()
    first, last = ((day - date(1858, 11, 17)).days for day in (start, end))
    if first < 45700 or last <= first or last > 100000:
        raise ValueError("Choose at least two complete daily records from 1984 onward")
    date.fromisoformat(retrieved_on)
    selected, flags = [], []
    for line in raw.splitlines(keepends=True):
        if not line.strip():
            continue
        mjd = float(line[7:15])
        if not first <= mjd <= last:
            continue
        if len(line.rstrip(b'\r\n')) < 134 or mjd != first + len(selected):
            raise ValueError("Incomplete, unordered or missing finals2000A row")
        day = date(int(line[:2]) + (1900 if mjd <= 51543 else 2000), int(line[2:4]), int(line[4:6]))
        if (day - date(1858, 11, 17)).days != mjd:
            raise ValueError("Calendar and MJD disagree")
        quality = bytes(line[index] for index in (16, 57, 95))
        if any(flag not in b'IP' for flag in quality):
            raise ValueError("All polar motion, UT1 and celestial pole flags are required")
        values = [float(line[a:b]) for a, b in ((18, 27), (37, 46), (58, 68), (97, 106), (116, 125))]
        errors = [float(line[a:b]) for a, b in ((27, 36), (46, 55), (68, 78), (106, 115), (125, 134))]
        if not all(math.isfinite(value) for value in values + errors) or min(errors) < 0:
            raise ValueError("Nonfinite value or invalid uncertainty")
        if abs(values[0]) >= 10 or abs(values[1]) >= 10 or abs(values[2]) >= 2 or max(abs(v) for v in values[3:]) >= 10000:
            raise ValueError("Unsupported parameter domain")
        selected.append(line)
        flags.append(quality.decode('ascii'))
    if len(selected) != last - first + 1:
        raise ValueError("Source does not contain the complete requested interval")
    payload = b''.join(selected)
    manifest = dict(schema_version=1, snapshot_id=f'iers-finals2000a-{retrieved_on}',
                    format='iers-finals2000a', data_file='finals2000A.data',
                    sha256=hashlib.sha256(payload).hexdigest(), source_url=source_url,
                    source_sha256=hashlib.sha256(raw).hexdigest(), retrieved_on=retrieved_on,
                    first_mjd=first, last_mjd=last, reference_frame='ITRS',
                    interpolation='finals2000a-hermite-tai-v1')
    output.mkdir(parents=True, exist_ok=False)
    (output / 'finals2000A.data').write_bytes(payload)
    (output / 'manifest.json').write_text(json.dumps(manifest, indent=2) + '\n', encoding='utf-8')
    (output / 'NOTICE.txt').write_text(
        f'IERS Rapid Service / Prediction Center, Bulletin A finals2000A.\nSource: {source_url}\n'
        f'Retrieved: {retrieved_on}; coverage: {start} through {end} UTC midnight inclusive.\n'
        'Original daily records include observation (I) and prediction (P) flags.\n'
        'Prediction is not final observed C04 data. Only complete x/y, UT1, dX/dY records are selected.\n'
        'Interpolation: cubic Hermite in TAI, shared adjacent secants for all five rates.\n'
        'dX/dY milliarcseconds are converted to radians. No subdaily tides or extrapolation.\n'
        'Format: https://maia.usno.navy.mil/ser7/readme.finals2000A\n', encoding='utf-8')
    return {'manifest': manifest, 'quality_flags': sorted(set(flags))}


if __name__ == '__main__':
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--input', type=Path, required=True)
    parser.add_argument('--output-dir', type=Path, required=True)
    parser.add_argument('--start', type=date.fromisoformat, required=True)
    parser.add_argument('--end', type=date.fromisoformat, required=True)
    parser.add_argument('--source-url', required=True)
    parser.add_argument('--retrieved-on', required=True)
    args = parser.parse_args()
    print(json.dumps(make_snapshot(args.input, args.output_dir, args.start, args.end,
                                   args.source_url, args.retrieved_on), indent=2))
