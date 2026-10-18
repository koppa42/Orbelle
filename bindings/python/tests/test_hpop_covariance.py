from concurrent.futures import ThreadPoolExecutor
from pathlib import Path
import json
import math
import tempfile
import unittest
import orbitforge as of

ROOT = Path(__file__).resolve().parents[3]

def covariance():
    result = of.HpopCovarianceInput()
    result.state = [[(10000.0 if i < 3 else 1.0) if i == j else 0.0
                     for j in range(6)] for i in range(6)]
    drag, srp = of.HpopConsiderParameter(), of.HpopConsiderParameter()
    drag.variance, srp.variance = 1e-4, 2e-4
    drag.state_cross_covariance = [0.1, 0, 0, 0, 0, 0]
    srp.state_cross_covariance = [0, -.1, 0, 0, 0, 0]
    result.drag, result.srp, result.drag_srp_covariance = drag, srp, 5e-5
    return result

def orbit():
    value = of.OrbitInput()
    value.propagator = of.OrbitPropagator.HPOP
    value.initial = of.CartesianState(of.Epoch(0), of.OriginId.EARTH, of.FrameId.GCRF,
                                      (7e6, 1e6, 2e5), (-1000, 7200, 1200))
    options = value.hpop
    options.gravity_degree = options.gravity_order = 0
    options.relative_tolerance = 1e-13
    options.position_tolerance_m, options.velocity_tolerance_mps = 1e-6, 1e-9
    value.hpop = options
    return value

class HpopCovarianceTests(unittest.TestCase):
    def test_installed_covariance_against_independent_orekit_transition(self):
        rows = [list(map(float, line.split())) for line in
                (ROOT / "tests/reference/propagators/universal_kepler_stm.txt").read_text().splitlines()
                if line and not line.startswith("#")]
        ref = next(r for r in rows if r[0] == 14)
        value, initial = orbit(), covariance()
        value.initial = of.CartesianState(of.Epoch(0), of.OriginId.EARTH, of.FrameId.GCRF,
                                          ref[3:6], ref[6:9])
        result = of.propagate_hpop_covariance(value, of.Epoch(int(ref[2]*1e9)), initial)
        phi = [ref[15+6*i:21+6*i] for i in range(6)]
        expected = [[sum(phi[i][k]*initial.state[k][k]*phi[j][k] for k in range(6))
                     for j in range(6)] for i in range(6)]
        for i in range(6):
            for j in range(6):
                error = abs(result.covariance.state[i][j]-expected[i][j])
                self.assertLess(error/math.sqrt(expected[i][i])/math.sqrt(expected[j][j]), 3e-9)
            expected_cross = sum(initial.drag.state_cross_covariance[k]*phi[i][k] for k in range(6))
            self.assertAlmostEqual(result.covariance.drag.state_cross_covariance[i], expected_cross, delta=1e-7)
        self.assertEqual(result.covariance.drag.variance, initial.drag.variance)
        self.assertEqual(result.covariance.drag_srp_covariance, initial.drag_srp_covariance)
        transported = of.transport_hpop_covariance(initial, result.variational)
        self.assertEqual(transported.covariance.state, result.covariance.state)
        self.assertEqual(result.contributions.consider_parameters, [[0.0]*6 for _ in range(6)])

    def test_owned_results_input_snapshot_and_concurrent_calls(self):
        value, initial = orbit(), covariance()
        def run():
            return of.propagate_hpop_covariance(value, of.Epoch(600*10**9), initial)
        reference = run()
        with ThreadPoolExecutor(max_workers=3) as pool:
            values = list(pool.map(lambda _: run(), range(3)))
        for result in values:
            self.assertEqual(result.covariance.state, reference.covariance.state)
        copy = reference.covariance
        copy.state = [[0.0]*6 for _ in range(6)]
        self.assertGreater(reference.covariance.state[0][0], 0)
        copy.drag = None
        self.assertIsNotNone(reference.covariance.drag)
        with self.assertRaises(AttributeError):
            reference.covariance = initial
        def mutate_input():
            initial.state = [[0.0]*6 for _ in range(6)]
            initial.drag = None
            return False
        actual = of.propagate_hpop_covariance(value, of.Epoch(600*10**9), initial, cancelled=mutate_input)
        self.assertEqual(actual.covariance.state, reference.covariance.state)

    def test_joint_validation_cancellation_and_retry(self):
        value, initial = orbit(), covariance()
        of.validate_hpop_covariance(initial)
        with self.assertRaises(ValueError):
            of.validate_hpop_covariance(of.HpopCovarianceInput())
        invalid = covariance()
        parameter = invalid.drag
        parameter.state_cross_covariance = [100, 0, 0, 0, 0, 0]
        invalid.drag = parameter
        with self.assertRaises(ValueError):
            of.validate_hpop_covariance(invalid)
        for callback, error in ((lambda: True, of.AnalysisCancelled), (lambda: 1, TypeError)):
            with self.assertRaises(error):
                of.propagate_hpop_covariance(value, of.Epoch(100*10**9), initial, cancelled=callback)
        with self.assertRaises(ValueError):
            of.propagate_hpop_covariance(of.OrbitInput(), of.Epoch(0), initial)
        result = of.propagate_hpop_covariance(value, of.Epoch(0), initial)
        self.assertAlmostEqual(result.covariance.state[0][0], initial.state[0][0], delta=1e-10)

    @unittest.skipUnless(of.ephemeris_available() and of.time_frames_available(), "requires providers")
    def test_consider_covariance_composes_with_real_drag_and_srp_resources(self):
        eop = ROOT / "data/samples/eop/iers_c04_2016_12_28_2017_01_04/manifest.json"
        spice = ROOT / "data/samples/spice/de440s_2016_leap/manifest.json"
        epoch = of.Epoch(json.loads(spice.read_text())["first_tai_ns"] + 3600*10**9)
        with tempfile.TemporaryDirectory() as folder:
            data = of.DataSnapshot.load(eop_manifest=eop).with_ephemeris("de440s", spice, Path(folder))
            try:
                value, initial = orbit(), covariance()
                value.initial = of.CartesianState(epoch, of.OriginId.EARTH, of.FrameId.GCRF,
                                                  (7e6, 1e6, 2e5), (-1000, 7200, 1200))
                options = value.hpop
                drag, srp = options.drag, options.srp
                drag.enabled, drag.cd = True, 0.0
                drag.atmosphere = of.AtmosphereModel.NRLMSISE00
                srp.enabled, srp.cr = True, 0.0
                srp.shadow_model = of.SrpShadowModel.NONE
                options.drag, options.srp = drag, srp
                value.hpop = options
                midpoint = of.Epoch(epoch.tai_nanoseconds+300*10**9)
                end = of.Epoch(epoch.tai_nanoseconds+600*10**9)
                whole = of.propagate_hpop_covariance(value, end, initial, data=data)
                first = of.propagate_hpop_covariance(value, midpoint, initial, data=data)
                value.initial = first.variational.state
                split = of.propagate_hpop_covariance(value, end, first.covariance, data=data)
                for i in range(6):
                    for j in range(6):
                        error = abs(whole.covariance.state[i][j]-split.covariance.state[i][j])
                        self.assertLess(error/math.sqrt(whole.covariance.state[i][i]) /
                                        math.sqrt(whole.covariance.state[j][j]), 3e-9)
                self.assertGreater(max(abs(x) for r in whole.contributions.consider_parameters for x in r), 0)
                self.assertNotEqual(whole.variational.drag_sensitivity, [0.0]*6)
                self.assertNotEqual(whole.variational.srp_sensitivity, [0.0]*6)
            finally:
                data = None

if __name__ == "__main__":
    unittest.main()
