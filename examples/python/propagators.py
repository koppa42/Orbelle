"""J4 mean orbit and native TEME SGP4; explicit time data, no network access."""
import math
import orbitforge as of

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
