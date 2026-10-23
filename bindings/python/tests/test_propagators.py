import math
from concurrent.futures import ThreadPoolExecutor
from pathlib import Path
import tempfile
from threading import Event
import unittest

# Native value/format tests; public editing is in test_objects.py.
from orbelle import _native as native


TLE = (
    '1 00005U 58002B   00179.78495062  .00000023  00000-0  28098-4 0  4753',
    '2 00005  34.2682 348.7242 1859667 331.7664  19.3264 10.82419157413667',
)


class PropagatorTests(unittest.TestCase):
    def test_sgp4_creation_while_parent_tle_is_replaced(self):
        def renumber(line):
            line = line[:2] + '00006' + line[7:68]
            checksum = sum(int(c) if c.isdigit() else int(c == '-') for c in line) % 10
            return line + str(checksum)

        original, other = native.Tle(*TLE), native.Tle(*(renumber(line) for line in TLE))
        source = native.OrbitInput()
        source.tle = original
        time = native.make_builtin_time_provider()
        epoch = time.from_utc(original.epoch_utc)
        expected = native.make_sgp4_session(original, time, 'expected').sample([epoch]).states[0]
        stop = Event()

        def replace_input():
            while not stop.is_set():
                source.tle = other
                source.tle = original

        with ThreadPoolExecutor(max_workers=1) as pool:
            mutation = pool.submit(replace_input)
            try:
                for _ in range(100):
                    session = native.make_sgp4_session(source.tle, time, 'concurrent')
                    self.assertEqual(session.sample([epoch]).states[0].position_m, expected.position_m)
            finally:
                stop.set()
            mutation.result()

    def test_sgp4_teme_reference_and_owned_result(self):
        time = native.make_builtin_time_provider()
        tle = native.Tle(*TLE)
        self.assertEqual(tle.catalog_id, '00005')
        self.assertEqual(tle.epoch_utc.nanosecond, 733568000)
        self.assertAlmostEqual(tle.period_seconds, 86400 / 10.82419157)
        epoch = time.from_utc(tle.epoch_utc)
        session = native.make_sgp4_session(tle, time, 'Vanguard', input_revision=3)
        result = session.sample([epoch, native.Epoch(epoch.tai_nanoseconds + 259200000000000)])
        del session, time
        self.assertEqual(result.provenance.input_revision, 3)
        state = result.states[-1]
        self.assertEqual(state.frame, native.FrameId.TEME)
        for actual, expected in zip(state.position_m, [-9060473.73569, 4658709.52502, 813686.73153]):
            self.assertAlmostEqual(actual, expected, delta=.001)
        with self.assertRaises(ValueError):
            native.make_sgp4_session(native.Tle(TLE[0][:-1] + '0', TLE[1]), native.make_builtin_time_provider(), 'bad')

    def test_j4_mean_ellipse_and_disabled_periodic_terms(self):
        initial = native.J4MeanElements(native.Epoch(0), 7e6, .01, *map(math.radians, [98.5, 25, 70, 10]))
        ellipse = native.j4_mean_ellipse_state(initial)
        recovered = native.j4_mean_ellipse_elements(ellipse)
        self.assertAlmostEqual(recovered.mean_anomaly_rad, initial.mean_anomaly_rad, places=12)
        self.assertLess(native.j4_earth().j4, 0)
        state = native.make_j4_session(initial, 'j4').sample([native.Epoch(86400000000000)]).states[0]
        for actual, expected in zip(state.position_m, [6248081.888094468, 2876670.719466801, 1076354.259731579]):
            self.assertAlmostEqual(actual, expected, delta=.001)

    def test_tle_project_roundtrip_requires_explicit_time(self):
        time = native.make_builtin_time_provider()
        tle = native.Tle(*TLE)
        first = time.from_utc(tle.epoch_utc)
        project = native.Project('TLE', first, native.Epoch(first.tai_nanoseconds + 60000000000))
        orbit = native.OrbitInput()
        orbit.propagator = native.OrbitPropagator.SGP4
        orbit.tle = tle
        project.add_satellite(orbit)
        with self.assertRaisesRegex(ValueError, 'time provider'):
            project.to_scenario()
        expected = project.to_scenario(time).sample([first])[0].states[0]
        with tempfile.TemporaryDirectory() as directory:
            path = Path(directory) / 'satellite.orbelle'
            project.save(path)
            restored = native.load_project(path)
            self.assertEqual(restored.satellites[0].orbit.tle.line1, TLE[0])
            actual = restored.to_scenario(time).sample([first])[0].states[0]
            self.assertEqual(actual.position_m, expected.position_m)
            self.assertEqual(actual.velocity_mps, expected.velocity_mps)

    def test_teme_frame_roundtrip_is_explicit(self):
        time, frames = native.make_builtin_time_provider(), native.make_frame_provider()
        epoch = time.from_utc(native.UtcDateTime(2004, 4, 6, 7, 51, 28, 386000000))
        eop = native.EarthOrientationState(
            native.EarthOrientationSample(epoch, -32.439961, -0.140682 * math.pi / 648000,
                                       .333309 * math.pi / 648000, 0, 0, 'example'),
            native.EarthOrientationRates(0, 0, 0, 0, 0))
        state = native.CartesianState(epoch, native.OriginId.EARTH, native.FrameId.TEME,
                                  [5094180.16210, 6127644.65950, 6380344.53270],
                                  [-4746.131487, 785.818041, 5531.931288])
        converted = native.transform_state(state, native.FrameId.GCRF, frames, eop)
        self.assertGreater(abs(converted.position_m[0] - state.position_m[0]), 100)
        back = native.transform_state(converted, native.FrameId.TEME, frames, eop)
        for a, b in zip(back.position_m, state.position_m):
            self.assertAlmostEqual(a, b, delta=1e-8)
