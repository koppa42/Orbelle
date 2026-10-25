"""Measure installed-wheel coverage on a deterministic Walker-like constellation."""
import argparse
import hashlib
import json
import math
import os
from pathlib import Path
import statistics
import time
from datetime import date


def peak_working_set_bytes():
    if os.name != "nt":
        return None
    import ctypes
    from ctypes import wintypes
    class Counters(ctypes.Structure):
        _fields_ = [("cb", wintypes.DWORD), ("faults", wintypes.DWORD)] + [
            (name, ctypes.c_size_t) for name in ("peak", "working", "peak_paged", "paged",
                                                "peak_nonpaged", "nonpaged", "pagefile", "peak_pagefile")]
    counters = Counters()
    counters.cb = ctypes.sizeof(counters)
    kernel = ctypes.WinDLL("kernel32", use_last_error=True)
    kernel.GetCurrentProcess.restype = wintypes.HANDLE
    psapi = ctypes.WinDLL("psapi", use_last_error=True)
    psapi.GetProcessMemoryInfo.argtypes = [wintypes.HANDLE, ctypes.POINTER(Counters), wintypes.DWORD]
    if not psapi.GetProcessMemoryInfo(kernel.GetCurrentProcess(), ctypes.byref(counters), counters.cb):
        raise ctypes.WinError(ctypes.get_last_error())
    return counters.peak


def main():
    import orbelle as ob
    from orbelle import units as u
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--satellites", type=int, default=32)
    parser.add_argument("--duration", type=int, default=3600)
    parser.add_argument("--resolution", type=float, default=10)
    parser.add_argument("--step", type=float, default=60)
    parser.add_argument("--cone", type=float)
    parser.add_argument("--global-grid", action="store_true")
    parser.add_argument("--memory-mib", type=int, default=512)
    parser.add_argument("--summary-only", action="store_true", help="Stream a digest without retaining Python copies of all points")
    parser.add_argument("--runs", type=int, default=3)
    parser.add_argument("--warmups", type=int, default=1)
    parser.add_argument("--start-date", type=date.fromisoformat,
                        help="UTC YYYY-MM-DD at midnight; default keeps the original 2000 epoch")
    parser.add_argument("--eop-manifest", type=Path)
    parser.add_argument("--progress", action="store_true")
    parser.add_argument("--output", type=Path, required=True)
    args = parser.parse_args()
    if args.runs < 1 or args.warmups < 0 or args.duration < 1 or args.satellites < 1:
        parser.error("runs, duration and satellites must be positive; warmups must be nonnegative")
    args.output.mkdir(parents=True, exist_ok=False)
    before = time.perf_counter()
    first = ob.Epoch(32_000_000_000)
    if args.start_date:
        first = ob.make_builtin_time_provider().from_utc(ob.UtcDateTime(
            args.start_date.year, args.start_date.month, args.start_date.day))
    project = ob.Project("Coverage scale", first, ob.Epoch(first.tai_nanoseconds + args.duration * 10**9))
    assets = []
    planes = max(1, round(math.sqrt(args.satellites)))
    per_plane = math.ceil(args.satellites / planes)
    with project.edit():
        for i in range(args.satellites):
            # Interleave planes so input order does not favour a single orbital plane.
            plane, slot = i % planes, i // planes
            sat = project.objects.satellite(f"Satellite{i}", orbit=ob.orbits.TwoBody(
                initial=ob.orbits.Keplerian(first, u.km(6928.137), 0.001, u.deg(53),
                    raan=u.deg(360 * plane / planes),
                    true_anomaly=u.deg(360 * (slot + plane / planes) / per_plane))))
            assets.append(sat if args.cone is None else project.objects.sensor(f"Sensor{i}",
                parent=sat, field_of_view=ob.sensors.Cone(u.deg(args.cone))))
        bounds = (-90, 90, -180, 180) if args.global_grid else (20, 50, 90, 130)
        region = project.objects.region("Region", boundary=ob.regions.Rectangle(*(u.deg(x) for x in bounds)),
                                        grid_spacing=u.deg(args.resolution))
    eop = args.eop_manifest or (Path(__file__).resolve().parents[2] /
        "data/samples/eop/iers_c04_2000_01_01_2000_01_02/manifest.json")
    analysis = project.prepare(data=ob.DataSnapshot.load(eop_manifest=eop))
    preparation_s = time.perf_counter() - before
    grid = ob.CoverageGrid()
    grid.resolution_rad = math.radians(args.resolution)
    grid.minimum_elevation_rad = math.radians(10)
    report = {"arguments": {k: str(v) if isinstance(v, (Path, date)) else v for k,v in vars(args).items()},
              "preparation_s": preparation_s, "runs_s": [], "version": ob.__version__,
              "first_tai_ns": first.tai_nanoseconds,
              "eop_manifest_sha256": hashlib.sha256(eop.read_bytes()).hexdigest()}
    results = []
    for run in range(args.runs + args.warmups):
        start = time.perf_counter()
        last_notice = start
        def progress(done, total):
            nonlocal last_notice
            now = time.perf_counter()
            if now - last_notice >= 15:
                print(f"run {run}: {done}/{total}; elapsed={now-start:.1f} s", flush=True)
                last_notice = now
        result = analysis.report(ob.ReportKind.COVERAGE_GRID, objects=assets, region=region,
            coverage=grid, step=u.s(args.step), event_search_mode=ob.AccessSearchMode.EXTREMUM_ASSISTED,
            boundary_tolerance=u.s(1e-6), coverage_memory_mib=args.memory_mib,
            progress=progress if args.progress else None)
        elapsed = time.perf_counter() - start
        stats = result.coverage_statistics
        def values():
            for p in stats.iter_points():
                yield {"lat": p.cell.location.latitude_rad, "lon": p.cell.location.longitude_rad,
                   "area": p.cell.area_m2, "covered_s": p.statistics.covered_seconds,
                   "max_gap_s": p.statistics.maximum_gap_seconds,
                   "windows": [[w.first.tai_nanoseconds, w.last.tai_nanoseconds] for w in p.windows]}
        point_count = stats.point_count
        mean, max_gap = stats.mean_coverage_fraction, stats.maximum_gap_seconds
        if args.summary_only:
            checksum = hashlib.sha256()
            for point in values():
                checksum.update(json.dumps(point, sort_keys=True).encode())
                checksum.update(b"\n")
            digest = checksum.hexdigest()
        else:
            points = list(values())
            digest = hashlib.sha256(json.dumps(points, sort_keys=True).encode()).hexdigest()
        results.append(digest)
        if run >= args.warmups:
            report["runs_s"].append(elapsed)
        report["input_sha256"] = result.input_sha256
        report["data_sha256"] = result.data_sha256
        print(f"run {run}: {elapsed:.6f} s; {point_count} points; mean={mean:.12g}", flush=True)
        del result, stats
    if len(set(results)) != 1:
        raise RuntimeError("Non-deterministic coverage results")
    report.update(median_s=statistics.median(report["runs_s"]), points=point_count, digest=results[0],
                  mean_coverage_fraction=mean, maximum_gap_s=max_gap,
                  peak_working_set_bytes=peak_working_set_bytes(),
                  native_sha256=hashlib.sha256(Path(ob._native.__file__).read_bytes()).hexdigest())
    (args.output / "profile.json").write_text(json.dumps(report, indent=2) + "\n", encoding="utf-8")
    if not args.summary_only:
        (args.output / "points.json").write_text(json.dumps(points) + "\n", encoding="utf-8")


if __name__ == "__main__":
    main()
