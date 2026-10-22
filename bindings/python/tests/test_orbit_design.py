"""Public design API and its use with the same J2 propagator as the desktop."""
import math
import unittest
import orbelle as of


class OrbitDesignTest(unittest.TestCase):
    def test_nine_types_and_mean_ellipse_contract(self):
        for name in ("CIRCULAR", "CRITICALLY_INCLINED", "CRITICALLY_INCLINED_SUN_SYNC",
                     "GEOSYNCHRONOUS", "MOLNIYA", "ORBIT_DESIGNER",
                     "REPEATING_GROUND_TRACE", "REPEATING_SUN_SYNC", "SUN_SYNCHRONOUS"):
            with self.subTest(name=name):
                request = of.EarthOrbitDesignRequest()
                request.type = getattr(of.EarthOrbitType, name)
                design = of.design_earth_orbit(request)
                self.assertGreater(design.elements.semi_major_axis_m * (1-design.elements.eccentricity), 6378136.6)
                self.assertEqual(design.secular_j2, name not in ("CIRCULAR", "ORBIT_DESIGNER", "GEOSYNCHRONOUS"))

    def test_repeat_sun_sync_is_preserved_by_actual_propagation(self):
        request = of.EarthOrbitDesignRequest()
        request.type = of.EarthOrbitType.REPEATING_SUN_SYNC
        request.repeat_revolutions, request.repeat_days = 233, 16
        design = of.design_earth_orbit(request)
        e = design.elements
        mean = of.J2MeanElements(e.epoch, e.semi_major_axis_m, 0., e.inclination_rad,
                                e.raan_rad, 0., e.true_anomaly_rad)
        states = of.make_j2_session(mean, "designed").sample([
            e.epoch, of.Epoch(round(design.repeat_cycle_seconds * 1e9))]).states
        angle = 7.292115e-5 * design.repeat_cycle_seconds
        x, y, z = states[1].position_m
        fixed = [math.cos(angle)*x+math.sin(angle)*y, -math.sin(angle)*x+math.cos(angle)*y, z]
        for actual, expected in zip(fixed, states[0].position_m):
            self.assertAlmostEqual(actual, expected, delta=0.001)
        self.assertAlmostEqual(design.raan_rate_rad_s*86400*180/math.pi, 360/365.2422, delta=1e-12)

    def test_impossible_design_and_bad_integer_are_rejected(self):
        request = of.EarthOrbitDesignRequest()
        request.type = of.EarthOrbitType.REPEATING_SUN_SYNC
        request.repeat_days = 0
        with self.assertRaises(ValueError):
            of.design_earth_orbit(request)
        with self.assertRaises(TypeError):
            request.repeat_days = 1.5
        request.type = of.EarthOrbitType.SUN_SYNCHRONOUS
        request.altitude_m = 50000000
        with self.assertRaises(ValueError):
            of.design_earth_orbit(request)


if __name__ == "__main__":
    unittest.main()
