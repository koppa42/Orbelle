"""Geometric station-to-sensor Access, using the packaged historical EOP sample."""
from pathlib import Path
import orbitforge as of
from orbitforge import units as u
from orbitforge.sensors import Cone, Pointing

manifest = Path(of.__file__).parent / "data/eop/iers_c04_2000_01_01_2000_01_02/manifest.json"
project = of.Project("Ground access", of.Epoch(0), of.Epoch(7_200_000_000_000))
data = of.DataSnapshot.load(eop_manifest=manifest)
with project.edit():
    satellite = project.objects.satellite("Observer", orbit=of.orbits.TwoBody(
        initial=of.OrbitInput().initial))
    station = project.objects.ground_station("Hong Kong", location=of.Wgs84Location(
        longitude=u.deg(114.2), latitude=u.deg(22.3), ellipsoid_height=u.m(350)),
        minimum_elevation=u.deg(5))
    sensor = project.objects.sensor("Camera", parent=satellite,
        field_of_view=Cone(half_angle=u.deg(45)), pointing=Pointing.track(station))

analysis = project.prepare(data=data)
windows = analysis.access(station, sensor, search_step=u.s(1))
for window in windows:
    print(window.start.tai_nanoseconds, window.end.tai_nanoseconds,
          window.duration.nanoseconds / 1e9, "seconds")
print(f"{len(windows)} windows; data SHA-256: {windows.data_sha256}")
# windows.to_csv("access.csv")       # Export these results without recomputing.
# table = windows.to_pandas()        # Optional pandas, with exact TAI nanoseconds.
