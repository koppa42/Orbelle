"""J4, native TEME SGP4 and numerical HPOP; explicit data, no network access."""
import math
from pathlib import Path
import orbitforge as of
from orbitforge import units as u

time = of.make_builtin_time_provider()
tle = of.Tle(
    '1 00005U 58002B   00179.78495062  .00000023  00000-0  28098-4 0  4753',
    '2 00005  34.2682 348.7242 1859667 331.7664  19.3264 10.82419157413667',
)
epoch = time.from_utc(tle.epoch_utc)
sgp4 = of.make_sgp4_session(tle, time, 'Vanguard')
print('SGP4 / TEME:', sgp4.sample([epoch]).states[0].position_m)

elements = of.J4MeanElements(epoch, 7e6, .01, math.radians(98.5), 0, 0, 0)
j4 = of.make_j4_session(elements, 'design-orbit')
print('J4 / GCRF:', j4.sample([epoch]).states[0].position_m)

# The installed small EOP fixture covers this example; select your own data for a real mission.
data = of.DataSnapshot.load(eop_manifest=Path(of.__file__).parent /
                           'data/eop/iers_c04_2016_12_28_2017_01_04/manifest.json')
epoch = time.from_utc(of.UtcDateTime(2017, 1, 1, 0, 0, 0))
project = of.Project('HPOP example', epoch, of.Epoch(epoch.tai_nanoseconds + 3_600_000_000_000))
satellite = project.objects.satellite('Numerical', orbit=of.orbits.Hpop(
    initial=of.orbits.Keplerian(epoch, u.km(7000), 0.01, u.deg(98.5)),
    gravity_degree=8, max_step_seconds=60))
trajectory = project.prepare(data=data).states(satellite, step=u.s(30))
print('HPOP / EGM96 8x8 / GCRF:', trajectory.position.values[-1])
