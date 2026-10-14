"""A two-impulse Hohmann transfer, with no external data needed for GCRF queries."""
import math
import orbitforge as of
from orbitforge import units as u
from orbitforge.missions import Coast, Impulse

mu = of.central_body().gravitational_parameter_m3_s2
r1, r2 = 7e6, 8e6
a = (r1 + r2) / 2
transfer_ns = round(math.pi * math.sqrt(a**3 / mu) * 1e9)
dv1 = math.sqrt(mu * (2/r1 - 1/a)) - math.sqrt(mu/r1)
dv2 = math.sqrt(mu/r2) - math.sqrt(mu * (2/r2 - 1/a))
project = of.Project("Hohmann transfer", of.Epoch(0), of.Epoch(transfer_ns + 3_600_000_000_000))
satellite = project.objects.satellite("Transfer", orbit=of.orbits.Mission(
    initial=of.orbits.Keplerian(project.first, u.km(7000), 0, u.deg(0)),
    segments=[Impulse((dv1, 0, 0), name="Raise apoapsis"),
              Coast(u.ns(transfer_ns), name="Transfer arc"),
              Impulse((dv2, 0, 0), name="Circularize"),
              Coast(u.s(3600), name="Final orbit")]))
analysis = project.prepare()
for burn in analysis.maneuvers(satellite).events:
    print(burn.name, burn.before.epoch.tai_nanoseconds, burn.cumulative_delta_v_mps)
final = analysis.state(satellite, at=project.last)
assert abs(math.hypot(*final.position_m) - r2) < 1e-3
print(f"Final radius: {math.hypot(*final.position_m)/1000:.6f} km")
