"""Prepare matched coverage inputs and compare actual STK Engine exports.

Uses scientific ITRF states, never desktop/display tracks. Sharing ephemeris and
point coordinates isolates coverage geometry/event detection from propagation,
EOP and grid-generation differences. STK still computes all accesses and FOMs.
"""
from __future__ import annotations

import argparse
import csv
import hashlib
import json
import math
from pathlib import Path

REPO = Path(__file__).resolve().parents[2]


def read(path):
    return json.loads(Path(path).read_text(encoding="utf-8-sig"))


def write(path, value):
    Path(path).write_text(json.dumps(value, indent=2, ensure_ascii=False, allow_nan=False) + "\n",
                          encoding="utf-8")


def sha256(path):
    return hashlib.sha256(Path(path).read_bytes()).hexdigest()


def rotate(vector, angle):
    x, y, z = vector
    return [x * math.cos(angle) - y * math.sin(angle),
            x * math.sin(angle) + y * math.cos(angle), z]


def candidate_grid(grid):
    """Shared input lattice, including points outside each polygon.

    This only constructs input coordinates; STK independently clips them to its
    AreaTarget. Comparing retained sets also checks the region boundary mask.
    """
    span = grid.east_rad - grid.west_rad
    if span <= 0:
        span += 2 * math.pi
    height = grid.north_rad - grid.south_rad
    bands = math.ceil(height / grid.resolution_rad)
    result = []
    for row in range(bands):
        south = grid.south_rad + height * (row / bands)
        north = grid.north_rad if row + 1 == bands else grid.south_rad + height * ((row + 1) / bands)
        latitude = (south + north) / 2
        columns = max(1, math.ceil(span * math.cos(latitude) / grid.resolution_rad))
        for col in range(columns):
            west = grid.west_rad + span * (col / columns)
            east = grid.west_rad + span * ((col + 1) / columns)
            result.append((math.degrees(latitude), math.degrees(math.remainder((west + east) / 2, 2 * math.pi))))
    return result


def prepare(args):
    import orbelle as ob
    from orbelle import _native
    from orbelle import units as u

    config = read(args.inputs)
    if config["schema_version"] != 1:
        raise ValueError("Unsupported input schema")
    args.output.mkdir(parents=True, exist_ok=False)
    time = ob.make_builtin_time_provider()
    # This fixed fixture deliberately stays within the bundled C04 snapshot.
    if config["start_utc"] != "1 Jan 2000 12:00:00.000":
        raise ValueError("This fixture requires the documented UTC epoch")
    first = time.from_utc(ob.UtcDateTime(2000, 1, 1, 12, 0, 0, 0))
    duration = config["duration_s"]
    project = ob.Project("STKCoverageComparison", first,
                         ob.Epoch(first.tai_nanoseconds + int(duration * 1e9)))
    satellites, sensors = [], []
    for item in config["satellites"]:
        satellite = project.objects.satellite(item["name"], orbit=ob.orbits.TwoBody(
            initial=ob.orbits.Cartesian(first, rotate(item["position_m"], item["rotation_z_rad"]),
                                       rotate(item["velocity_mps"], item["rotation_z_rad"]))))
        satellites.append(satellite)
        sensors.append(project.objects.sensor("Cone" + item["name"], parent=satellite,
            field_of_view=ob.sensors.Cone(u.deg(config["cone_half_angle_deg"]))))
    regions = []
    for item in config["regions"]:
        if "bounds_deg" in item:
            boundary = ob.regions.Rectangle(*(u.deg(x) for x in item["bounds_deg"]))
        else:
            boundary = ob.regions.Polygon(tuple(ob.regions.Vertex(u.deg(lon), u.deg(lat))
                                               for lon, lat in item["vertices_deg"]))
        regions.append(project.objects.region(item["name"], boundary=boundary,
                                               grid_spacing=u.deg(config["resolution_deg"])))
    analysis = project.prepare(data=ob.DataSnapshot.load(eop_manifest=args.eop))
    data = {"schema_version": 1, "config": config, "inputs_sha256": sha256(args.inputs),
            "eop_manifest_sha256": sha256(args.eop), "orbelle_version": ob.__version__,
            "native_binary_sha256": sha256(_native.__file__),
            "ephemeris_step_s": args.ephemeris_step, "search_step_s": args.search_step,
            "boundary_tolerance_s": args.boundary_tolerance, "first_tai_ns": first.tai_nanoseconds,
            "comparison_scope": "Common scientific ITRF ephemeris and geodetic grid; coverage only",
            "satellites": [], "cases": []}
    for item, satellite in zip(config["satellites"], satellites, strict=True):
        states = list(analysis.states(satellite, step=u.s(args.ephemeris_step), frame=ob.FrameId.ITRF))
        name = item["name"] + ".e"
        lines = ["stk.v.11.0", "BEGIN Ephemeris", f"NumberOfEphemerisPoints {len(states)}",
                 "ScenarioEpoch " + config["start_utc"], "InterpolationMethod Lagrange",
                 "InterpolationSamplesM1 7", "CentralBody Earth", "CoordinateSystem Fixed",
                 "DistanceUnit Meters", "EphemerisTimePosVel"]
        for state in states:
            values = [state.epoch.seconds_since(first), *state.position_m, *state.velocity_mps]
            lines.append(" ".join(format(v, ".17g") for v in values))
        lines.append("END Ephemeris")
        (args.output / name).write_text("\n".join(lines) + "\n", encoding="ascii")
        checks = []
        for offset in range(0, duration, 60):
            seconds = offset + args.ephemeris_step / 2
            state = analysis.state(satellite, at=ob.Epoch(first.tai_nanoseconds + round(seconds * 1e9)),
                                   frame=ob.FrameId.ITRF)
            checks.append({"time_s": seconds, "position_m": list(state.position_m),
                           "velocity_mps": list(state.velocity_mps)})
        data["satellites"].append({"name": item["name"], "ephemeris_file": name,
                                   "sha256": sha256(args.output / name), "checks": checks})
    grid = ob.CoverageGrid()
    grid.resolution_rad = math.radians(config["resolution_deg"])
    grid.minimum_elevation_rad = math.radians(config["minimum_elevation_deg"])
    for item, region in zip(config["regions"], regions, strict=True):
        variants = [(mode, assets, seconds) for seconds in (duration, config["short_duration_s"])
                    for mode, assets in [("satellites", satellites), ("cones", sensors)]]
        for mode, assets, seconds in variants:
            suffix = "" if seconds == duration else "_1h"
            case = {"name": item["name"] + "_" + mode + suffix, "region": item, "mode": mode,
                    "duration_s": seconds, "points": []}
            for ends in (True, False):
                grid.include_end_gaps = ends
                result = analysis.report(ob.ReportKind.COVERAGE_GRID, objects=assets,
                    region=region, coverage=grid, step=u.s(args.search_step),
                    event_search_mode=ob.AccessSearchMode.EXTREMUM_ASSISTED,
                    first=first, last=ob.Epoch(first.tai_nanoseconds + int(seconds * 1e9)),
                    boundary_tolerance=u.s(args.boundary_tolerance))
                stats = result.coverage_statistics
                if ends:
                    for point in stats.points:
                        cell, s = point.cell, point.statistics
                        case["points"].append({"lat_deg": math.degrees(cell.location.latitude_rad),
                            "lon_deg": math.degrees(cell.location.longitude_rad), "area_m2": cell.area_m2,
                            "windows": [[w.first.seconds_since(first), w.last.seconds_since(first)]
                                        for w in point.windows], "covered_s": s.covered_seconds,
                            "max_gap_s": s.maximum_gap_seconds, "mean_gap_s": s.mean_gap_seconds,
                            "gap_count": s.gap_count})
                    case["summary"] = {"ever_percent": 100 * stats.ever_covered_area_m2 / stats.total_area_m2,
                        "mean_percent": 100 * stats.mean_coverage_fraction,
                        "maximum_gap_s": stats.maximum_gap_seconds, "area_m2": stats.total_area_m2}
                else:
                    for point, actual in zip(case["points"], stats.points, strict=True):
                        s = actual.statistics
                        point.update(internal_max_gap_s=s.maximum_gap_seconds,
                                     internal_mean_gap_s=s.mean_gap_seconds,
                                     internal_gap_count=s.gap_count)
            name = case["name"] + ".pnt"
            lines = ["stk.v.11.0", "BEGIN PointList"]
            candidates = candidate_grid(stats.grid)
            lines.extend(f"{lat:.17g} {lon:.17g}" for lat, lon in candidates)
            lines.append("END PointList")
            (args.output / name).write_text("\n".join(lines) + "\n", encoding="ascii")
            case["point_file"] = name
            case["candidate_points"] = len(candidates)
            case["point_sha256"] = sha256(args.output / name)
            data["cases"].append(case)
            print(case["name"], len(case["points"]), case["summary"], flush=True)
    write(args.output / "orbelle.json", data)
    project.save(args.output / "comparison.orbelle")


def union(intervals):
    result = []
    for start, end in sorted(intervals):
        if not math.isfinite(start + end) or end < start:
            raise ValueError("Invalid reference access interval")
        if end == start:
            continue
        if result and start <= result[-1][1]:
            result[-1][1] = max(result[-1][1], end)
        else:
            result.append([start, end])
    return result


def coordinate_key(latitude, longitude):
    return round(float(latitude), 8), round(math.remainder(float(longitude), 360), 8)


def point_map(data, field):
    rows = list(zip(data["Latitude"], data["Longitude"], data[field], strict=True))
    result = {coordinate_key(lat, lon): float(value) for lat, lon, value in rows}
    if len(result) != len(rows):
        raise ValueError("Duplicate STK grid coordinates")
    return result


def error_stats(values):
    return {"maximum_absolute": max(map(abs, values), default=0),
            "rms": math.sqrt(sum(v * v for v in values) / len(values)) if values else 0,
            "count": len(values)}


def coverage_at(points, weights, seconds, cumulative=False):
    def visible(windows):
        if cumulative:
            return bool(windows) and windows[0][0] <= seconds
        return any(start <= seconds <= end for start, end in windows)
    return 100 * sum(area for point, area in zip(points, weights, strict=True)
                     if visible(point)) / sum(weights)


def compare(args):
    actual, reference = read(args.orbelle), read(args.stk)
    if reference["orbelle_input_sha256"].lower() != sha256(args.orbelle):
        raise ValueError("STK did not use these shared inputs")
    args.output.mkdir(parents=True, exist_ok=False)
    output = {"schema_version": 1, "scope": actual["comparison_scope"],
              "stk_version": reference["version"], "orbelle_version": actual["orbelle_version"],
              "inputs_sha256": actual["inputs_sha256"], "orbelle_sha256": sha256(args.orbelle),
              "stk_sha256": sha256(args.stk), "ephemeris_step_s": actual["ephemeris_step_s"],
              "native_binary_sha256": actual.get("native_binary_sha256"),
              "search_step_s": actual["search_step_s"], "boundary_tolerance_s": actual["boundary_tolerance_s"],
              "duration_s": actual["config"]["duration_s"],
              "stk_earth": reference.get("earth"),
              "satellites": [], "cases": []}
    expected_satellites = {s["name"]: s for s in actual["satellites"]}
    for satellite in reference["satellites"]:
        position, velocity = [], []
        for expected, observed in zip(expected_satellites[satellite["name"]]["checks"],
                                      satellite["checks"], strict=True):
            if expected["time_s"] != observed["time_s"]:
                raise ValueError("Ephemeris check times disagree")
            position.append(math.dist(expected["position_m"], observed["position_m"]))
            velocity.append(math.dist(expected["velocity_mps"], observed["velocity_mps"]))
        output["satellites"].append({"name": satellite["name"], "position_m": error_stats(position),
                                     "velocity_mps": error_stats(velocity)})
    expected_cases = {c["name"]: c for c in actual["cases"]}
    if set(expected_cases) != {c["name"] for c in reference["cases"]}:
        raise ValueError("Coverage cases differ")
    for case in reference["cases"]:
        expected = expected_cases[case["name"]]
        duration = expected.get("duration_s", actual["config"]["duration_s"])
        maps = {name: point_map(values, "FOM Value") for name, values in case["foms"].items()}
        native_areas = point_map(case["grid"], "Area")
        keys = [coordinate_key(p["lat_deg"], p["lon_deg"]) for p in expected["points"]]
        if set(keys) != set(native_areas) or any(set(keys) != set(m) for m in maps.values()):
            raise ValueError("STK region contains a different set of shared grid points")
        intervals = {}
        for point in case["points"]:
            data = point["data"]
            intervals[coordinate_key(point["lat_deg"], point["lon_deg"])] = union(zip(
                map(float, data.get("Access Start", [])), map(float, data.get("Access End", [])), strict=True))
        differences = {name: [] for name in maps}
        boundary_errors, mismatches, rows = [], [], []
        fom_consistency = []
        for point, key in zip(expected["points"], keys, strict=True):
            windows = intervals[key]
            row = {"latitude_deg": point["lat_deg"], "longitude_deg": point["lon_deg"],
                   "orbelle_intervals": len(point["windows"]), "stk_intervals": len(windows)}
            if len(windows) != len(point["windows"]):
                mismatches.append({"lat_lon": key, "orbelle": point["windows"], "stk": windows})
            else:
                for old, new in zip(windows, point["windows"], strict=True):
                    boundary_errors.extend(new[i] - old[i] for i in (0, 1))
            for name, values in maps.items():
                delta = point[name] - values[key]
                differences[name].append(delta)
                row.update({"orbelle_" + name: point[name], "stk_" + name: values[key],
                            "delta_" + name: delta})
            fom_consistency.append(sum(b - a for a, b in windows) - maps["covered_s"][key])
            rows.append(row)
        weights = [p["area_m2"] for p in expected["points"]]
        native_weights = [native_areas[key] for key in keys]
        actual_windows = [p["windows"] for p in expected["points"]]
        reference_windows = [intervals[key] for key in keys]
        total_area = sum(weights)
        stk_mean = 100 * sum(w * maps["covered_s"][key] for w, key in zip(weights, keys, strict=True)) / total_area / duration
        stk_native_mean = 100 * sum(w * maps["covered_s"][key] for w, key in zip(native_weights, keys, strict=True)) / sum(native_weights) / duration
        stk_ever = coverage_at(reference_windows, weights, duration, cumulative=True)
        stk_native_ever = coverage_at(reference_windows, native_weights, duration, cumulative=True)
        series = []
        for seconds in range(0, duration + 1, 30):
            a = coverage_at(actual_windows, weights, seconds)
            b = coverage_at(reference_windows, weights, seconds)
            ae = coverage_at(actual_windows, weights, seconds, cumulative=True)
            be = coverage_at(reference_windows, weights, seconds, cumulative=True)
            bn = coverage_at(reference_windows, native_weights, seconds)
            ben = coverage_at(reference_windows, native_weights, seconds, cumulative=True)
            series.append({"time_s": seconds, "orbelle_percent": a, "stk_common_area_percent": b,
                           "delta_pp": a - b, "orbelle_cumulative_percent": ae,
                           "stk_common_area_cumulative_percent": be, "cumulative_delta_pp": ae - be,
                           "stk_native_area_percent": bn, "native_area_delta_pp": bn - b,
                           "stk_native_area_cumulative_percent": ben, "native_area_cumulative_delta_pp": ben - be})
        # Verify our independent interval aggregation against STK's native report.
        native_series_error = []
        for seconds, percent in zip(case["percent_coverage"]["Time"],
                                    case["percent_coverage"]["Percent Coverage"], strict=True):
            native_series_error.append(coverage_at(reference_windows, native_weights, float(seconds)) - float(percent))
        summary = {"name": case["name"], "points": len(keys), "duration_s": duration,
            "candidate_points": expected.get("candidate_points", len(keys)),
            "orbelle_mean_percent": expected["summary"]["mean_percent"], "stk_common_area_mean_percent": stk_mean,
            "mean_delta_pp": expected["summary"]["mean_percent"] - stk_mean,
            "orbelle_ever_percent": expected["summary"]["ever_percent"], "stk_common_area_ever_percent": stk_ever,
            "ever_delta_pp": expected["summary"]["ever_percent"] - stk_ever,
            "stk_native_area_mean_percent": stk_native_mean, "native_area_effect_pp": stk_native_mean - stk_mean,
            "stk_native_area_ever_percent": stk_native_ever, "native_area_ever_effect_pp": stk_native_ever - stk_ever,
            "orbelle_maximum_gap_s": expected["summary"]["maximum_gap_s"],
            "stk_maximum_gap_s": max(maps["max_gap_s"].values()),
            "differences_s": {name: error_stats(values) for name, values in differences.items()},
            "interval_boundary_error_s": error_stats(boundary_errors), "interval_count_mismatches": mismatches,
            "instantaneous_error_pp": error_stats([s["delta_pp"] for s in series]),
            "cumulative_error_pp": error_stats([s["cumulative_delta_pp"] for s in series]),
            "native_area_instantaneous_effect_pp": error_stats([s["native_area_delta_pp"] for s in series]),
            "native_area_cumulative_effect_pp": error_stats([s["native_area_cumulative_delta_pp"] for s in series]),
            "stk_fom_vs_access_union_s": error_stats(fom_consistency),
            "stk_native_percent_vs_access_union_pp": error_stats(native_series_error)}
        output["cases"].append(summary)
        for suffix, records in [("points", rows), ("time", series)]:
            with (args.output / f"{case['name']}-{suffix}.csv").open("w", newline="", encoding="utf-8") as stream:
                writer = csv.DictWriter(stream, fieldnames=list(records[0]))
                writer.writeheader()
                writer.writerows(records)
        print(case["name"], "mean delta pp", summary["mean_delta_pp"], "max revisit error s",
              summary["differences_s"]["max_gap_s"]["maximum_absolute"], "interval mismatches", len(mismatches))
    write(args.output / "comparison.json", output)


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("command", choices=["prepare", "compare"])
    parser.add_argument("--inputs", type=Path, default=REPO / "tests/reference/propagators/coverage_stk.json")
    parser.add_argument("--eop", type=Path, default=REPO / "data/samples/eop/iers_c04_2000_01_01_2000_01_02/manifest.json")
    parser.add_argument("--output", type=Path, required=True)
    parser.add_argument("--ephemeris-step", type=float, default=10)
    parser.add_argument("--search-step", type=float, default=30)
    parser.add_argument("--boundary-tolerance", type=float, default=1e-6)
    parser.add_argument("--orbelle", type=Path)
    parser.add_argument("--stk", type=Path)
    args = parser.parse_args()
    if args.command == "compare":
        if args.orbelle is None or args.stk is None:
            parser.error("compare requires --orbelle and --stk")
        compare(args)
    else:
        if not 0 < args.ephemeris_step <= 60 or not 0 < args.search_step <= 60:
            parser.error("ephemeris/search steps must be in (0, 60] seconds for this fixture")
        if not 1e-9 <= args.boundary_tolerance <= 1e-3:
            parser.error("boundary tolerance must be in [1e-9, 1e-3] seconds")
        prepare(args)


if __name__ == "__main__":
    main()
