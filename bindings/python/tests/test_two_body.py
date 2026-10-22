import concurrent.futures
import math
import unittest

import orbelle as of


class TwoBodyTests(unittest.TestCase):
    @staticmethod
    def initial():
        return of.CartesianState(of.Epoch(0), of.OriginId.EARTH, of.FrameId.GCRF,
                                 [3, 0, 0], [0, 2, 0])

    def test_capability_and_parabolic_solution(self):
        if not of.two_body_available():
            with self.assertRaises(RuntimeError):
                of.make_two_body_session(self.initial(), 6, "satellite")
            return
        session = of.make_two_body_session(self.initial(), 6, "parabolic", input_revision=7)
        result = session.sample([of.Epoch(0), of.Epoch(4_000_000_000)])
        self.assertEqual(result.provenance.object_id, "parabolic")
        self.assertEqual(result.provenance.input_revision, 7)
        self.assertEqual(result.provenance.algorithm, "two-body-cspice-n0067-prop2b-tai-v1")
        self.assertEqual(result.states[0].position_m, [3, 0, 0])
        for actual, expected in zip(result.states[1].position_m, [0, 6, 0]):
            self.assertAlmostEqual(actual, expected, delta=1e-8)
        for actual, expected in zip(result.states[1].velocity_mps, [-1, 1, 0]):
            self.assertAlmostEqual(actual, expected, delta=1e-8)

    def test_invalid_inputs_are_explicit(self):
        if not of.two_body_available():
            self.skipTest("CSPICE was not built")
        for mu in (0, -1, math.inf, math.nan):
            with self.assertRaises(ValueError):
                of.make_two_body_session(self.initial(), mu, "bad")
        wrong_frame = of.CartesianState(of.Epoch(0), of.OriginId.EARTH, of.FrameId.ITRF,
                                       [3, 0, 0], [0, 2, 0])
        with self.assertRaises(ValueError):
            of.make_two_body_session(wrong_frame, 6, "bad-frame")
        with self.assertRaises(ValueError):
            of.make_two_body_session(self.initial(), 6, "")

    def test_threaded_queries_and_owned_results(self):
        if not of.two_body_available():
            self.skipTest("CSPICE was not built")
        session = of.make_two_body_session(self.initial(), 6, "parallel")
        epochs = [of.Epoch(i*1_000_000_000) for i in range(-10, 11)]
        reference = session.sample(epochs)
        with concurrent.futures.ThreadPoolExecutor(max_workers=4) as pool:
            results = list(pool.map(lambda _: session.sample(epochs), range(8)))
        del session
        for result in results:
            self.assertEqual([s.position_m for s in result.states],
                             [s.position_m for s in reference.states])
            self.assertEqual([s.velocity_mps for s in result.states],
                             [s.velocity_mps for s in reference.states])


if __name__ == "__main__":
    unittest.main()
