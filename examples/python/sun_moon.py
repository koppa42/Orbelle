"""Explicit offline historical SPICE snapshot; velocities are per TDB second."""
import math
from pathlib import Path
import tempfile

import orbelle as of


sample = Path(of.__file__).parent / "data/spice/de440s_2016_leap/manifest.json"
epoch = of.make_builtin_time_provider().from_utc(of.UtcDateTime(2017, 1, 1))
with tempfile.TemporaryDirectory(prefix="orbelle-example-") as workspace:
    ephemeris = of.load_spice_snapshot(sample, workspace)
    for name, target in (("Sun", 10), ("Moon", 301)):
        state = ephemeris.geometric_state(target, 399, epoch)
        print(f"{name}, observer Earth, SPICE J2000 axes:")
        print(f"  distance (m): {math.hypot(*state.position_m):.3f}")
        print(f"  position (m): {state.position_m}")
        print(f"  velocity (m/TDB s): {state.velocity_mps}")
    del ephemeris  # Release CSPICE handles before removing the caller-owned workspace.
