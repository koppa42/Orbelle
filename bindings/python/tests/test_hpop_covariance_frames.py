from concurrent.futures import ThreadPoolExecutor
from pathlib import Path
import math
import unittest

import orbelle as of

ROOT = Path(__file__).resolve().parents[3]


def coordinates(index):
    f, v = of.HpopCovarianceFrame, of.HpopCovarianceVelocity
    values = ((f.GCRF, v.PROJECTED), (f.LVLH, v.PROJECTED), (f.LVLH, v.ROTATING),
              (f.FRENET, v.PROJECTED), (f.FRENET, v.ROTATING))
    return of.HpopCovarianceCoordinates(*values[index])


def orbit():
    value = of.OrbitInput()
    value.propagator = of.OrbitPropagator.HPOP
    value.initial = of.CartesianState(of.Epoch(0), of.OriginId.EARTH, of.FrameId.GCRF,
                                      (7e6, 0, 0), (0, 7500, 0))
    options = value.hpop
    options.gravity_degree = options.gravity_order = 0
    value.hpop = options
    return value


def covariance():
    value = of.HpopCovarianceInput()
    value.state = [[(1e6 if i < 3 else .25) if i == j else 0.0
                    for j in range(6)] for i in range(6)]
    k = of.HpopConsiderParameter()
    k.variance = 1e-4
    k.state_cross_covariance = [.2, -.1, .3, .001, -.0002, .0003]
    value.srp = k
    return value


def joint(value):
    matrix = [row + [0., 0.] for row in value.state] + [[0.]*8 for _ in range(2)]
    for k, parameter in ((6, value.drag), (7, value.srp)):
        if parameter is not None:
            matrix[k][k] = parameter.variance
            for i, x in enumerate(parameter.state_cross_covariance):
                matrix[k][i] = matrix[i][k] = x
    matrix[6][7] = matrix[7][6] = value.drag_srp_covariance
    return matrix


class HpopCovarianceFramesTests(unittest.TestCase):
    def assert_matrix(self, actual, expected, tolerance=5e-13):
        for i in range(len(expected)):
            for j in range(len(expected)):
                if expected[i][i] == 0 or expected[j][j] == 0:
                    self.assertEqual(actual[i][j], 0)
                else:
                    error = abs(actual[i][j] - expected[i][j])
                    self.assertLessEqual(error/math.sqrt(expected[i][i])/math.sqrt(expected[j][j]),
                                         tolerance)

    def test_installed_api_against_200_orekit_and_high_precision_references(self):
        path = ROOT / "tests/reference/propagators/hpop_covariance_frames.txt"
        rows = [list(map(float, line.split())) for line in path.read_text().splitlines()
                if line and not line.startswith("#")]
        self.assertEqual(len(rows), 200)
        for row in rows:
            case, source, target, mask = map(int, row[:4])
            with self.subTest(case=case):
                state = of.CartesianState(of.Epoch(0), of.OriginId.EARTH, of.FrameId.GCRF,
                                          row[4:7], row[7:10])
                acceleration = row[10:13]
                reference_jacobian = [row[13+6*i:19+6*i] for i in range(6)]
                initial = [row[49+8*i:57+8*i] for i in range(8)]
                expected = [row[113+8*i:121+8*i] for i in range(8)]
                p = of.HpopCovarianceInput()
                p.coordinates = coordinates(source)
                p.state = [r[:6] for r in initial[:6]]
                for bit, index, name in ((1, 6, "drag"), (2, 7, "srp")):
                    if mask & bit:
                        parameter = of.HpopConsiderParameter()
                        parameter.variance = initial[index][index]
                        parameter.state_cross_covariance = initial[index][:6]
                        setattr(p, name, parameter)
                p.drag_srp_covariance = initial[6][7]
                actual = of.convert_hpop_covariance(state, p, coordinates(target),
                                                     acceleration_mps2=acceleration)
                self.assert_matrix(joint(actual), expected)
                j = of.hpop_covariance_frame_jacobian(state, coordinates(source), coordinates(target),
                                                      acceleration_mps2=acceleration)
                scale = max(1, *(abs(reference_jacobian[i][k]) * (1000 if i >= 3 and k < 3 else 1)
                                 for i in range(6) for k in range(6)))
                self.assertLessEqual(max(abs(j[i][k]-reference_jacobian[i][k]) *
                                         (1000 if i >= 3 and k < 3 else 1)
                                         for i in range(6) for k in range(6))/scale, 3e-13)
                self.assertEqual(actual.coordinates.frame, coordinates(target).frame)
                self.assertEqual(actual.coordinates.velocity, coordinates(target).velocity)
                self.assertEqual(p.coordinates.frame, coordinates(source).frame)

    def test_local_initial_covariance_forward_backward_and_owned_metadata(self):
        value = orbit()
        acceleration = [-3.986004415e14/(7e6*7e6), 0, 0]
        for seconds in (-60, 60):
            endpoint = of.Epoch(seconds*10**9)
            baseline = of.propagate_hpop_covariance(value, endpoint, covariance())
            for frame in (1, 2, 3, 4):
                local = of.convert_hpop_covariance(value.initial, covariance(), coordinates(frame),
                                                    acceleration_mps2=acceleration)
                actual = of.propagate_hpop_covariance(value, endpoint, local)
                self.assert_matrix(joint(actual.covariance), joint(baseline.covariance), 2e-13)
                self.assertEqual(actual.variational.state.position_m, baseline.variational.state.position_m)
                self.assertEqual(actual.covariance.coordinates.frame, of.HpopCovarianceFrame.GCRF)
                copy = actual.covariance
                copy.coordinates = coordinates(4)
                self.assertEqual(actual.covariance.coordinates.frame, of.HpopCovarianceFrame.GCRF)

    def test_snapshot_cancel_concurrency_and_conversion_rejection(self):
        value = orbit()
        local = covariance()
        local.coordinates = coordinates(2)
        endpoint = of.Epoch(30*10**9)
        baseline = of.propagate_hpop_covariance(value, endpoint, local)
        with ThreadPoolExecutor(max_workers=3) as pool:
            results = list(pool.map(lambda _: of.propagate_hpop_covariance(value, endpoint, local), range(3)))
        for actual in results:
            self.assertEqual(actual.covariance.state, baseline.covariance.state)
        def mutate():
            local.coordinates = coordinates(4)
            local.state = [[0.]*6 for _ in range(6)]
            return False
        actual = of.propagate_hpop_covariance(value, endpoint, local, cancelled=mutate)
        self.assertEqual(actual.covariance.state, baseline.covariance.state)
        with self.assertRaises(of.AnalysisCancelled):
            of.propagate_hpop_covariance(value, endpoint, covariance(), cancelled=lambda: True)
        for target in (2, 4):
            with self.assertRaises(ValueError):
                of.convert_hpop_covariance(value.initial, covariance(), coordinates(target))
        with self.assertRaises(ValueError):
            of.convert_hpop_covariance(value.initial, covariance(), coordinates(1),
                                       acceleration_mps2=[math.nan, 0, 0])
        local = covariance()
        local.coordinates = coordinates(1)
        with self.assertRaises(ValueError):
            of.transport_hpop_covariance(local, baseline.variational)
        self.assertGreater(of.propagate_hpop_covariance(value, endpoint, local).covariance.state[0][0], 0)


if __name__ == "__main__":
    unittest.main()
