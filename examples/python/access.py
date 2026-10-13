"""Geometric station-to-sensor Access, using the packaged historical EOP sample."""
from pathlib import Path
import math
import orbitforge as of
from orbitforge import units as u
from orbitforge.sensors import Cone, Pointing

manifest = Path(of.__file__).parent / "data/eop/iers_c04_2000_01_01_2000_01_02/manifest.json"
project = of.Project("Ground access", of.Epoch(0), of.Epoch(7_200_000_000_000))
project.earth_orientation = of.load_eop_snapshot(manifest).data_info
with project.edit():
    satellite = project.objects.satellite("Observer", orbit=of.orbits.TwoBody(
        initial=of.OrbitInput().initial))
    station = project.objects.ground_station("Hong Kong", location=of.Wgs84Location(
        longitude=u.deg(114.2), latitude=u.deg(22.3), ellipsoid_height=u.m(350)),
        minimum_elevation=u.deg(5))
    sensor = project.objects.sensor("Camera", parent=satellite,
        field_of_view=Cone(half_angle=u.deg(45)), pointing=Pointing.track(station))

definition = of.ReportDefinition()
definition.id = definition.name = "Access windows"
definition.kind = of.ReportKind.ACCESS_INTERVALS
definition.object_ids = [station.id, sensor.id]
definition.step_nanoseconds = 1_000_000_000
windows = of.generate_report(project, definition, manifest)
for row in range(windows.table.row_count):
    print(windows.cell_text(row, 2), windows.cell_text(row, 3), windows.cell_text(row, 4))
session = of.AccessSession(project, station.id, sensor.id, manifest)
print("Elevation at start (deg):", math.degrees(session.at(project.first).observation.elevation_rad))
