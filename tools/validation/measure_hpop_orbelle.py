"""Optional Release HPOP benchmark against shared STK Cartesian cases.

Resource loading, analysis setup, first propagation and output sampling are timed
separately. Requires an installed Orbelle wheel or --native built by this checkout.
STK itself is driven separately by measure_hpop_stk.ps1; no CI license dependency.
"""
from __future__ import annotations
import argparse
import hashlib
import importlib.util
import json
import os
from pathlib import Path
import statistics
import sys
import time
import types

ROOT = Path(__file__).resolve().parents[2]
DLL_HANDLES = []


def sha256(path):
    with Path(path).open("rb") as stream:
        return hashlib.file_digest(stream, "sha256").hexdigest()


def load_native(extension):
    if extension is None:
        from orbelle import _native
        return _native
    extension = extension.resolve()
    if sys.platform == "win32":
        for path in (extension.parent, ROOT / "out/dependencies/installed/x64-windows/bin"):
            if path.is_dir():
                DLL_HANDLES.append(os.add_dll_directory(str(path)))
    package = types.ModuleType("orbelle")
    package.__path__ = []
    sys.modules["orbelle"] = package
    spec = importlib.util.spec_from_file_location("orbelle._native", extension)
    module = importlib.util.module_from_spec(spec)
    sys.modules["orbelle._native"] = module
    spec.loader.exec_module(module)
    return module


def timed(fn):
    start = time.perf_counter()
    value = fn()
    return value, (time.perf_counter() - start) * 1000


def state_record(state):
    return {"epoch_tai_ns": state.epoch.tai_nanoseconds,
            "position_m": list(state.position_m), "velocity_mps": list(state.velocity_mps)}


def options_for(ob, forces, mu):
    options = ob.HpopOptions()
    options.gravity_model = "egm2008"
    options.gravity_mu = mu
    options.gravity_degree = options.gravity_order = 0 if forces == "point_mass" else 21
    options.include_secular_variations = False
    options.include_relativistic_accelerations = False
    options.ephemeris_model = "de440s"
    options.satellite_mass_kg = 1000
    options.integrator = ob.HpopIntegrator.RKF78
    options.vop = False
    options.step_control = ob.HpopStepControl.RELATIVE_ERROR
    options.relative_tolerance = 1e-13
    options.position_tolerance_m = 0.001
    options.velocity_tolerance_mps = 0.000001
    options.min_step_seconds = 1.0
    options.max_step_seconds = 86400.0
    options.fixed_step_seconds = 10
    options.time_regularization = False
    options.minimum_altitude_m = 10000
    options.report_ephemeris_on_fixed_step = True
    tides = ob.SolidTideOptions()
    tides.mode = ob.SolidTideMode.PERMANENT if forces == "full" else ob.SolidTideMode.NONE
    tides.truncate_to_gravity = True
    tides.include_time_dependent_terms = False
    options.solid_tides = tides
    bodies = list(options.third_bodies)
    for index, body in enumerate(bodies):
        body.enabled = forces == "full" and index in (0, 1)
        if body.enabled:
            body.source = ob.ThirdBodyGravitySource.USER_SPECIFIED
            body.user_mu = (1.32712200000000e20, 4.90280030555500e12)[index]
    options.third_bodies = bodies
    drag = ob.DragOptions()
    drag.enabled = forces == "full"
    drag.cd = 2.2
    drag.area_mass_ratio_m2_per_kg = 0.02
    drag.atmosphere = ob.AtmosphereModel.JACCHIA_ROBERTS
    drag.low_altitude_model = ob.LowAltitudeDensityModel.NONE
    drag.f107 = drag.f107_average = 150
    drag.kp = 3
    drag.weather_model = ""
    drag.use_apparent_sun_position = True
    drag.use_approximate_altitude = True
    options.drag = drag
    srp = ob.SrpOptions()
    srp.enabled = forces == "full"
    srp.cr = 1
    srp.area_mass_ratio_m2_per_kg = 0.02
    srp.shadow_model = ob.SrpShadowModel.DUAL_CONE
    srp.earth_shadow = srp.moon_shadow = True
    srp.sun_position_method = ob.SrpSunPositionMethod.APPARENT_TO_TRUE_CB
    srp.boundary_mitigation = False
    srp.atmospheric_altitude_m = 0
    srp.pressure_at_reference_pa = 4.56e-6
    srp.reference_distance_m = 149597870700
    srp.sun_radius_m, srp.earth_radius_m, srp.moon_radius_m = 695700000, 6378137, 1737400
    options.srp = srp
    interpolation = ob.OrbitInterpolationOptions()
    interpolation.method = ob.OrbitInterpolationMethod.LAGRANGE
    interpolation.order = 7
    interpolation.vop_mu = mu
    options.interpolation = interpolation
    options.validate()
    return options


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--output", type=Path, required=True)
    parser.add_argument("--gravity", type=Path, required=True)
    parser.add_argument("--ephemeris", type=Path, required=True, help="Existing DE440s manifest")
    parser.add_argument("--native", type=Path)
    parser.add_argument("--eop", type=Path, default=ROOT / "data/samples/eop/iers_finals2000a_2026_09_01_2026_11_01/manifest.json")
    parser.add_argument("--inputs", type=Path, default=ROOT / "tests/reference/propagators/hpop_performance.json")
    parser.add_argument("--cases", nargs="+", default=["scenario4", "leo", "gnss", "geo", "eccentric", "scenario4_week"])
    parser.add_argument("--forces", nargs="+", choices=["point_mass", "gravity21", "full"], default=["full"])
    parser.add_argument("--runs", type=int, default=3)
    args = parser.parse_args()
    if args.runs < 1:
        parser.error("--runs must be positive")
    inputs = json.loads(args.inputs.read_text(encoding="utf-8"))
    if inputs["schema_version"] != 1 or any(name not in inputs["cases"] for name in args.cases):
        parser.error("Unsupported input schema or case name")
    ob = load_native(args.native)
    epoch = ob.make_builtin_time_provider().from_utc(ob.UtcDateTime(*inputs["epoch_utc"]))
    if epoch.tai_nanoseconds != inputs["epoch_tai_ns"]:
        parser.error("UTC and TAI initial epochs disagree")
    args.output.mkdir(parents=True, exist_ok=False)
    report = {"inputs_sha256": sha256(args.inputs), "native_sha256": sha256(ob.__file__),
              "gravity_sha256": sha256(args.gravity), "eop_manifest_sha256": sha256(args.eop),
              "ephemeris_manifest_sha256": sha256(args.ephemeris), "cases": []}
    data, report["eop_load_ms"] = timed(lambda: ob.DataSnapshot.load(eop_manifest=args.eop))
    data, report["gravity_load_ms"] = timed(lambda: data.with_gravity("egm2008", args.gravity))
    data, report["ephemeris_load_ms"] = timed(lambda: data.with_ephemeris("de440s", args.ephemeris, args.output / "spice"))
    for name in args.cases:
        case = inputs["cases"][name]
        end = ob.Epoch(epoch.tai_nanoseconds + case["duration_s"] * 10**9)
        initial = ob.CartesianState(epoch, ob.OriginId.EARTH, ob.FrameId.GCRF, case["position_m"], case["velocity_mps"])
        epochs = [ob.Epoch(epoch.tai_nanoseconds + seconds * 10**9)
                  for seconds in range(0, case["duration_s"] + 1, inputs["output_step_s"])]
        for forces in args.forces:
            orbit = ob.OrbitInput()
            orbit.propagator, orbit.initial = ob.OrbitPropagator.HPOP, initial
            orbit.hpop = options_for(ob, forces, inputs["mu_m3_s2"])
            orbit.first, orbit.last, orbit.step_seconds = epoch, end, inputs["output_step_s"]
            orbit.object_id = name
            row = {"name": name, "forces": forces, "initial_gcrf": state_record(initial),
                   "duration_s": case["duration_s"], "sample_count": len(epochs), "runs": []}
            def prepare():
                project = ob.Project(name, epoch, end)
                document = ob.ProjectDocument(project)
                satellite = document.objects().create(ob.SatelliteDefinition("Satellite1", orbit, ob.OrbitDisplay()))
                return document, satellite, ob.AnalysisPlan(document, data).prepare()
            for run in range(args.runs):
                (document, satellite, analysis), setup_ms = timed(prepare)
                _, propagation_ms = timed(lambda: analysis.state(satellite, end, ob.FrameId.GCRF))
                series, sampling_ms = timed(lambda: analysis.states_at(satellite, epochs, ob.FrameId.GCRF))
                row["runs"].append({"setup_ms": setup_ms, "propagation_ms": propagation_ms, "sampling_ms": sampling_ms})
                if run == 0:
                    samples = [state_record(state) for state in series.states]
                    (args.output / f"{name}-{forces}-samples.json").write_text(json.dumps(samples), encoding="utf-8")
                print(f"{name}/{forces} run {run+1}: {propagation_ms:.3f} ms", flush=True)
            row["median_ms"] = statistics.median(run["propagation_ms"] for run in row["runs"])
            report["cases"].append(row)
            (args.output / "profile.json").write_text(json.dumps(report, indent=2) + "\n", encoding="utf-8")


if __name__ == "__main__":
    main()
