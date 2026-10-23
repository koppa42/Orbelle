from concurrent.futures import ThreadPoolExecutor
import math
import unittest
import orbelle as ob


def orbit(method=ob.HpopIntegrator.RKF78):
    value = ob.OrbitInput()
    value.propagator = ob.OrbitPropagator.HPOP
    value.initial = ob.CartesianState(ob.Epoch(0), ob.OriginId.EARTH, ob.FrameId.GCRF,
                                      (7e6, 0, 0), (0, math.sqrt(3.986004415e14/7e6), 0))
    value.object_id = "sampled-hpop"
    config = ob.HpopOptions()
    config.gravity_degree = config.gravity_order = 0
    config.integrator = method
    config.fixed_step_seconds = 10
    config.max_step_seconds = 30
    value.hpop = config
    return value


def interpolation():
    value = ob.OrbitInterpolationOptions()
    value.method = ob.OrbitInterpolationMethod.HERMITE
    value.order = 7
    return value


class HpopEphemerisTests(unittest.TestCase):
    def test_exact_fixed_outputs_and_owned_accepted_nodes(self):
        for method in (ob.HpopIntegrator.RK4, ob.HpopIntegrator.RKF78, ob.HpopIntegrator.RKV89,
                       ob.HpopIntegrator.BULIRSCH_STOER, ob.HpopIntegrator.GAUSS_JACKSON):
            for sign in (-1, 1):
                with self.subTest(method=method, sign=sign):
                    value = orbit(method)
                    config = value.hpop
                    config.step_control = ob.HpopStepControl.FIXED
                    value.hpop = config
                    end = ob.Epoch(sign*123_123_456_789)
                    accepted = ob.propagate_hpop_ephemeris(value, end)
                    self.assertEqual(len(accepted), 1)
                    self.assertEqual(len(accepted[0]), 14)
                    reported = ob.propagate_hpop_ephemeris(value, end, reporting_step_nanoseconds=7_100_000_000)
                    expected = sorted([sign*i*7_100_000_000 for i in range(18)] + [end.tai_nanoseconds])
                    self.assertEqual([s.epoch.tai_nanoseconds for s in reported[0]], expected)
                    value.initial = ob.CartesianState(ob.Epoch(0), ob.OriginId.EARTH, ob.FrameId.GCRF,
                                                      (8e6, 0, 0), (0, 7000, 0))
                    index = 0 if sign > 0 else -1
                    self.assertEqual(accepted[0][index].position_m, [7e6, 0, 0])

    def test_shared_bounded_session_and_source_identity(self):
        value = orbit()
        config = interpolation()
        session = ob.make_hpop_ephemeris_session(value, ob.Epoch(600*10**9), config,
                   reporting_step_nanoseconds=30*10**9, input_revision=7)
        self.assertEqual(session.provenance.object_id, value.object_id)
        self.assertEqual(session.provenance.input_revision, 7)
        self.assertIn("report-ns=30000000000", session.provenance.algorithm)
        self.assertIn("method=hermite;interpolation-order=7", session.provenance.algorithm)
        times = [ob.Epoch(173_123_456_789), ob.Epoch(599_000_000_001)]
        expected = session.sample(times).states
        config.order = 29
        value.hpop = ob.HpopOptions()
        with ThreadPoolExecutor(max_workers=3) as pool:
            results = list(pool.map(lambda _: session.sample(times).states, range(3)))
        for result in results:
            self.assertEqual(result[0].position_m, expected[0].position_m)
        for ns in (-1, 600*10**9+1):
            with self.assertRaises(ob.PropagationError): session.sample([ob.Epoch(ns)])
        if ob.two_body_available():
            truth = ob.make_two_body_session(orbit().initial, 3.986004415e14, "cspice").sample(times).states
            for actual, reference in zip(expected, truth):
                for a, b in zip(actual.position_m, reference.position_m):
                    self.assertAlmostEqual(a, b, delta=.001)
                for a, b in zip(actual.velocity_mps, reference.velocity_mps):
                    self.assertAlmostEqual(a, b, delta=1e-6)

    def test_cancelled_or_invalid_preparation_does_not_publish(self):
        value = orbit()
        with self.assertRaises(ob.AnalysisCancelled):
            ob.propagate_hpop_ephemeris(value, ob.Epoch(600*10**9), cancelled=lambda: True)
        with self.assertRaises(TypeError):
            ob.propagate_hpop_ephemeris(value, ob.Epoch(600*10**9), cancelled=lambda: 1)
        with self.assertRaises(ob.AnalysisCancelled):
            ob.make_hpop_ephemeris_session(value, ob.Epoch(600*10**9), interpolation(), cancelled=lambda: True)
        for period in (-1, 86_400*10**9+1):
            with self.assertRaises(ValueError):
                ob.propagate_hpop_ephemeris(value, ob.Epoch(1), reporting_step_nanoseconds=period)
        with self.assertRaises(ValueError):
            ob.make_hpop_ephemeris_session(value, ob.Epoch(1), interpolation())
        with self.assertRaises(ValueError):
            ob.propagate_hpop_ephemeris(ob.OrbitInput(), ob.Epoch(1))
        result = ob.propagate_hpop_ephemeris(value, ob.Epoch(600*10**9))
        def change_input():
            value.propagator = ob.OrbitPropagator.TWO_BODY
            value.hpop = ob.HpopOptions()
            return False
        frozen = ob.propagate_hpop_ephemeris(value, ob.Epoch(600*10**9), cancelled=change_input)
        self.assertEqual(frozen[-1][-1].position_m, result[-1][-1].position_m)
