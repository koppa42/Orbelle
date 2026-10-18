"""Select the routine ASan/UBSan tier without filtering Release numerical tests.

Only these reviewed, long numerical comparisons are deferred to the manual full
run. New tests run by default; broad suite/module patterns must not be used here.
The selection is saved alongside JUnit results so its exact coverage is auditable.
"""

import argparse
import json
from pathlib import Path
import re
import subprocess


# Measured on Ubuntu 24.04, GCC 13, RelWithDebInfo, CI run 39092697858.
# Each case remains unchanged in every complete Release numerical regression.
FULL_ONLY = {
    "integration.WeatherFiles.HpopEphemerisSplitsTheLeapSecondWeatherJumpInBothDirections":
        "Long bidirectional leap-second trajectories; shorter weather boundary tests stay routine.",
    "core.Gravity.FullDegreeAndRectangularFieldsMatchIndependentOrekit":
        "Dense 2190-degree reference grid; full-degree poles and correction boundaries stay routine.",
    "core.All/SundmanBoundaries.PhysicalEndpointsMatchIndependentReferenceWithoutOutOfRangeForces/3":
        "Long fixed-step Bulirsch-Stoer convergence comparison; other methods and fixed-step boundary tests stay routine.",
    "core.SrpBoundary.EventsAndTrajectoriesMatchOrekitAndConvergeForEveryIntegrator":
        "All-integrator eclipse convergence sweep; event geometry and higher-order steps stay routine.",
    "integration.MonthlyWeather.HpopConvergesAcrossLeapAndForecastNodesAndReopensFrozenData":
        "Long forecast trajectory convergence; parsing, interpolation and project I/O stay routine.",
    "core.HpopVariational.CombinedCoastsMatchIndependentOrekitFieldDerivatives":
        "Long combined-force variational coasts; force partials and covariance cases stay routine.",
    "core.HpopRegularization.PublicStateQueriesMatchIndependentHipparchusAtLargeTaiEpochs":
        "Multi-integrator long-epoch comparison; regularized endpoint/interpolation cases stay routine.",
    "core.GravityDifferential.MatchesIndependentOrekitAnalyticHessians":
        "Dense high-degree Hessian reference grid; exact poles and correction boundaries stay routine.",
    "integration.WeatherFiles.InterpolatedTrajectoriesConvergeAndSurviveProjectReopen":
        "Long interpolation trajectory convergence; frozen weather and reopen cases stay routine.",
    "core.HpopVop.MatchesIndependentHipparchusWithPerturbationsAndRegularization":
        "Repeated perturbed VOP convergence; VOP state/variational interpolation cases stay routine.",
    "core.Hpop.MatchesIndependentOrekitHolmesFeatherstoneAndDp853Reference":
        "Long combined HPOP reference trajectories; force models and integrator cases stay routine.",
    "integration.HpopEphemerisSession.BothInterpolatorsMatchCspiceForCircularAndEccentricCoasts":
        "Repeated circular/eccentric ephemeris coasts; interpolation and session boundaries stay routine.",
}


def select_tests(names, profile):
    if profile not in {"routine", "full"}:
        raise ValueError(f"Unknown sanitizer profile: {profile!r}")
    if not names or any(not isinstance(name, str) or not name or "\n" in name for name in names):
        raise ValueError("Expected a nonempty CTest inventory with valid test names")
    if len(set(names)) != len(names):
        raise ValueError("CTest inventory contains duplicate names")
    # A stale policy or wrong build preset is an error, never a successful empty
    # selection or an opportunity to silently extend the exclusion patterns.
    missing = FULL_ONLY.keys() - set(names)
    if missing:
        raise ValueError(f"Sanitizer inventory is missing reviewed cases: {sorted(missing)}")
    deferred = {name: reason for name, reason in FULL_ONLY.items()} if profile == "routine" else {}
    selected = [name for name in names if name not in deferred]
    if not selected:
        raise ValueError("Sanitizer selection would run no tests")
    # Names contain only literal dots/slashes in today's list. Escaping and
    # anchoring every name also protects future parameterized test names.
    pattern = "^(" + "|".join(re.escape(name) for name in deferred) + ")$"
    return {"profile": profile, "available_count": len(names),
            "selected_count": len(selected), "selected": selected, "deferred": deferred,
            "ctest_args": ["-E", pattern] if deferred else []}


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--build", type=Path, required=True)
    parser.add_argument("--profile", choices=("routine", "full"), required=True)
    parser.add_argument("--output", type=Path, required=True)
    args = parser.parse_args()
    inventory = json.loads(subprocess.check_output(
        ["ctest", "--test-dir", str(args.build), "--show-only=json-v1"], text=True))
    selection = select_tests([test["name"] for test in inventory["tests"]], args.profile)
    args.output.parent.mkdir(parents=True, exist_ok=True)
    args.output.write_text(json.dumps(selection, indent=2) + "\n", encoding="utf-8")
    print(f"ASan/UBSan {args.profile}: {selection['selected_count']}/{selection['available_count']} "
          f"tests selected; {len(selection['deferred'])} deferred to manual full runs.")


if __name__ == "__main__":
    main()
