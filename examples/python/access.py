"""Geometric station-to-sensor Access, using the packaged historical EOP sample."""
from pathlib import Path
import math
import orbitforge as of

manifest = Path(of.__file__).parent / "data/eop/iers_c04_2000_01_01_2000_01_02/manifest.json"
project = of.Project("Ground access", of.Epoch(0), of.Epoch(7_200_000_000_000))
project.earth_orientation = of.load_eop_snapshot(manifest).data_info
project.add_satellite(of.OrbitInput())
station = of.ProjectGroundStation()
station.object_id = "station"
station.location = of.GroundStationLocation(math.radians(114.2), math.radians(22.3), 350)
station.minimum_elevation_rad = math.radians(5)
project.add_ground_station(station)
sensor = of.ProjectSensor()
sensor.parent_satellite_id = "satellite-1"
sensor.pointing = of.SensorPointing.TARGETED
sensor.target_station_id = station.object_id
project.add_sensor(sensor)

definition = of.ReportDefinition()
definition.id = definition.name = "Access windows"
definition.kind = of.ReportKind.ACCESS_INTERVALS
definition.object_ids = [station.object_id, sensor.object_id]
definition.step_nanoseconds = 1_000_000_000
windows = of.generate_report(project, definition, manifest)
for row in range(windows.table.row_count):
    print(windows.cell_text(row, 2), windows.cell_text(row, 3), windows.cell_text(row, 4))
session = of.AccessSession(project, station.object_id, sensor.object_id, manifest)
print("Elevation at start (deg):", math.degrees(session.at(project.first).observation.elevation_rad))
