import math
import unittest
import orbelle as ob


@unittest.skipUnless(ob.two_body_available(), 'CSPICE disabled')
class KeplerianTests(unittest.TestCase):
    def test_elliptical_periapsis_and_owned_roundtrip(self):
        mu = 3.986004418e14
        elements = ob.KeplerianElements(ob.Epoch(123), 10_000_000, .3, 0, 0, 0, 0)
        state = ob.to_cartesian(elements, mu)
        self.assertAlmostEqual(state.position_m[0], 7_000_000, delta=1e-6)
        self.assertAlmostEqual(state.velocity_mps[1], math.sqrt(mu * 1.3 / 7_000_000), delta=1e-8)
        back = ob.to_cartesian(ob.to_keplerian(state, mu), mu)
        self.assertEqual(back.epoch.tai_nanoseconds, 123)
        for a, b in zip(back.position_m, state.position_m):
            self.assertAlmostEqual(a, b, delta=.001)

    def test_parabolic_axis_is_not_silently_accepted(self):
        with self.assertRaises(ValueError):
            ob.to_cartesian(ob.KeplerianElements(ob.Epoch(0), 7e6, 1, 0, 0, 0, 0), 3.986004418e14)
