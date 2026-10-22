"""Load the packaged historical EOP snapshot and transform a fixed ground state."""
import math
from pathlib import Path

import orbelle as of


time = of.make_builtin_time_provider()
frames = of.make_frame_provider()
manifest = Path(of.__file__).parent / "data/eop/iers_c04_2016_12_28_2017_01_04/manifest.json"
eop = of.load_eop_snapshot(manifest)
epoch = time.from_utc(of.UtcDateTime(2016, 12, 31, 23, 59, 60))
transform = frames.gcrf_to_itrf_state(epoch, eop.at(epoch))
ground = of.CartesianState(epoch, of.OriginId.EARTH, of.FrameId.ITRF,
                           [6378137, 0, 0], [0, 0, 0])
inertial = transform.inverse(ground)
print(f"Snapshot: {eop.data_info.id}")
print(f"GCRF position (m): {inertial.position_m}")
print(f"GCRF velocity (m/s): {inertial.velocity_mps}")
print(f"Speed (m/s): {math.hypot(*inertial.velocity_mps):.6f}")
