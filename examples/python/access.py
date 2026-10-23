"""Station-to-sensor access, AER and reports from one fixed analysis."""
from pathlib import Path
import orbelle as ob
from orbelle import units as u
from orbelle.sensors import Cone, Pointing

manifest = Path(ob.__file__).parent / "data/eop/iers_c04_2000_01_01_2000_01_02/manifest.json"
project = ob.Project("Ground access", ob.Epoch(0), ob.Epoch(7_200_000_000_000))
data = ob.DataSnapshot.load(eop_manifest=manifest)
with project.edit():
    satellite = project.objects.satellite("Observer", orbit=ob.orbits.TwoBody(
        initial=ob.OrbitInput().initial))
    station = project.objects.ground_station("Hong Kong", location=ob.Wgs84Location(
        longitude=u.deg(114.2), latitude=u.deg(22.3), ellipsoid_height=u.m(350)),
        minimum_elevation=u.deg(5))
    sensor = project.objects.sensor("Camera", parent=satellite,
        field_of_view=Cone(half_angle=u.deg(45)), pointing=Pointing.track(station))

analysis = project.prepare(data=data)
windows = analysis.access(station, sensor, search_step=u.s(60))
for window in windows:
    print(window.start.tai_nanoseconds, window.end.tai_nanoseconds,
          window.duration.nanoseconds / 1e9, "seconds")
    print("maximum elevation (rad):", window.extrema.maximum_elevation.value,
          "at TAI ns:", window.extrema.maximum_elevation.epoch.tai_nanoseconds)
    print("minimum range (m):", window.extrema.minimum_range.value,
          "at TAI ns:", window.extrema.minimum_range.epoch.tai_nanoseconds)
print("total visible seconds:", windows.statistics.total_duration_s)
print(f"{len(windows)} windows; data SHA-256: {windows.data_sha256}")
# windows.to_csv("access.csv")       # Export these results without recomputing.
# table = windows.to_pandas()        # Optional pandas, with exact TAI nanoseconds.

aer = analysis.aer(station, sensor, step=u.s(30))
print(f"{len(aer)} AER samples; first range: {aer[0].range_m / 1000:.3f} km")
assert aer.epoch.tai_nanoseconds[-1] == project.last.tai_nanoseconds
# aer.to_csv("aer.csv")
# elevation_deg = aer.elevation.to_numpy(unit="deg")  # Optional numpy.

report = analysis.report(ob.ReportKind.AER, station=station, target=sensor, step=u.s(30))
assert report.table.row_count == len(aer)
assert report.input_revision == analysis.revision
print(f"Report: {report.column_title(5)} = {report.cell_text(0, 5)}")
# report.export_csv("aer-report.csv")  # Display units and time scale, no recomputation.
