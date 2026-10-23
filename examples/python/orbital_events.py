"""Find physical apsis and GCRF node events without sampled-extremum estimates."""
import orbelle as ob
from orbelle import units as u

first = ob.Epoch(0)
project = ob.Project("Orbital events", first, ob.Epoch(12000 * 10**9))
satellite = project.objects.satellite("Eccentric LEO", orbit=ob.orbits.TwoBody(
    initial=ob.orbits.Cartesian(first, (7e6, 0, 0), (0, 8000, 2000))))
analysis = project.prepare()
events = analysis.report(ob.ReportKind.ORBITAL_EVENTS, objects=satellite,
    step=u.s(60), boundary_tolerance=u.s(.001))
for row in events.table.rows:
    print(f"{row[3]:16} {row[2].tai_nanoseconds / 1e9:12.6f} s  radius={row[4] / 1000:.3f} km")
print("Periapsis / apoapsis / ascending / descending counts:", events.orbital_event_statistics.counts)
# events.export_csv("events.csv")
