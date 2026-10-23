import math
import unittest
import orbelle as ob


class J2Tests(unittest.TestCase):
    def test_mean_state_session_and_options_snapshot(self):
        elements = ob.J2MeanElements(ob.Epoch(0), 7e6, .01, math.radians(98.5),
                                     math.radians(25), math.radians(70), math.radians(10))
        options = ob.J2SecularOptions()
        session = ob.make_j2_session(elements, "J2", options, input_revision=4)
        options.apsidal_precession = False
        state = session.sample([ob.Epoch(86400 * 10**9)]).states[0]
        for actual, expected in zip(state.position_m, [6248041.05824, 2877011.06403, 1075696.94996]):
            self.assertAlmostEqual(actual, expected, delta=.001)
        self.assertIn("apsis=1", session.provenance.algorithm)
        self.assertEqual(session.provenance.input_revision, 4)
        auxiliary = ob.j2_mean_ellipse_state(elements)
        recovered = ob.j2_mean_ellipse_elements(auxiliary)
        self.assertAlmostEqual(recovered.mean_anomaly_rad, elements.mean_anomaly_rad, delta=1e-12)
        self.assertEqual(ob.central_body().gravitational_parameter_m3_s2, 3.986004418e14)

    def test_distinct_mean_type_and_invalid_orbits(self):
        elements = ob.KeplerianElements(ob.Epoch(0), 7e6, .01, .5, 0, 0, 0)
        with self.assertRaises(TypeError):
            ob.make_j2_session(elements, "wrong-element-definition")
        with self.assertRaises(ValueError):
            ob.make_j2_session(ob.J2MeanElements(ob.Epoch(0), -7e6, 1.5, .5, 0, 0, 0), "open")


if __name__ == "__main__":
    unittest.main()
