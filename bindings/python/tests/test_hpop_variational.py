from concurrent.futures import ThreadPoolExecutor
from pathlib import Path
import math
import json
import tempfile
import unittest
import orbelle as ob

ROOT = Path(__file__).resolve().parents[3]

def orbit():
    value = ob.OrbitInput()
    value.propagator = ob.OrbitPropagator.HPOP
    value.initial = ob.CartesianState(ob.Epoch(0), ob.OriginId.EARTH, ob.FrameId.GCRF,
                                      (7e6, 1e6, 2e5), (-1000, 7200, 1200))
    config = ob.HpopOptions()
    config.gravity_degree = config.gravity_order = 0
    config.relative_tolerance = 1e-13
    config.position_tolerance_m = 1e-6
    config.velocity_tolerance_mps = 1e-9
    value.hpop = config
    return value

class HpopVariationalTests(unittest.TestCase):
    def test_gauss_jackson_long_arc_covariance_and_samples(self):
        rows = [list(map(float, line.split())) for line in
                (ROOT / "tests/reference/propagators/universal_kepler_stm.txt").read_text().splitlines()
                if line and not line.startswith("#")]
        covariance = ob.HpopCovarianceInput()
        covariance.state = [[(10000.0 if i < 3 else 1.0) if i == j else 0.0
                             for j in range(6)] for i in range(6)]
        for reference in (row for row in rows if row[0] in (0, 7)):
            phi = [reference[15+6*i:21+6*i] for i in range(6)]
            expected = [[sum(phi[i][k] * covariance.state[k][k] * phi[j][k] for k in range(6))
                         for j in range(6)] for i in range(6)]
            for regular in (False, True):
                value = orbit()
                value.initial = ob.CartesianState(
                    ob.Epoch(0), ob.OriginId.EARTH, ob.FrameId.GCRF, reference[3:6], reference[6:9])
                config = value.hpop
                config.integrator = ob.HpopIntegrator.GAUSS_JACKSON
                config.fixed_step_seconds = 60
                config.time_regularization = regular
                config.steps_per_orbit = 120
                value.hpop = config
                end = ob.Epoch(round(reference[2]*1e9))
                direct = ob.propagate_hpop_covariance(value, end, covariance)
                samples = ob.sample_hpop_covariance(
                    value, [end, ob.Epoch(end.tai_nanoseconds//3), ob.Epoch(0), end], covariance)
                self.assertEqual(samples[0].covariance.state, samples[-1].covariance.state)
                for result in (direct, samples[0]):
                    for i in range(6):
                        for j in range(6):
                            error = abs(result.covariance.state[i][j]-expected[i][j])
                            error /= math.sqrt(expected[i][i])*math.sqrt(expected[j][j])
                            self.assertLess(error, 3e-9)

    def test_installed_api_against_independent_orekit_stm(self):
        rows = [list(map(float, line.split())) for line in
                (ROOT / "tests/reference/propagators/universal_kepler_stm.txt").read_text().splitlines()
                if line and not line.startswith("#")]
        reference = next(row for row in rows if row[0] == 14)
        for regular in (False, True):
            for vop in (False, True):
                value = orbit()
                value.initial = ob.CartesianState(ob.Epoch(0), ob.OriginId.EARTH, ob.FrameId.GCRF,
                                                  reference[3:6], reference[6:9])
                config = value.hpop
                config.time_regularization, config.vop = regular, vop
                value.hpop = config
                result = ob.propagate_hpop_variational(value, ob.Epoch(int(reference[2]*1e9)))
                self.assertEqual(result.state.epoch.tai_nanoseconds, int(reference[2]*1e9))
                for i in range(6):
                    for j in range(6):
                        scale = (1e6 if j < 3 else 1e3) / (1e6 if i < 3 else 1e3)
                        self.assertLess(abs(result.state_transition[i][j]-reference[15+6*i+j])*scale, 5e-9)
                self.assertEqual(result.drag_sensitivity, [0.0]*6)
                self.assertEqual(result.srp_sensitivity, [0.0]*6)

    def test_owned_results_concurrency_and_input_snapshot(self):
        value = orbit()
        def run():
            return ob.propagate_hpop_variational(value, ob.Epoch(600*10**9))
        reference = run()
        with ThreadPoolExecutor(max_workers=3) as pool:
            results = list(pool.map(lambda _: run(), range(3)))
        for result in results:
            self.assertEqual(result.state_transition, reference.state_transition)
        matrix = reference.state_transition
        matrix[0][0] = -100
        self.assertNotEqual(reference.state_transition[0][0], -100)
        copy = reference.state
        with self.assertRaises(AttributeError):
            copy.position_m = (1,2,3)
        position = copy.position_m
        position[0] = 1
        self.assertNotEqual(reference.state.position_m[0], 1)
        def change_input():
            value.propagator = ob.OrbitPropagator.TWO_BODY
            return False
        result = ob.propagate_hpop_variational(value, ob.Epoch(600*10**9), cancelled=change_input)
        self.assertEqual(result.state_transition, reference.state_transition)

    @unittest.skipUnless(ob.ephemeris_available() and ob.time_frames_available(), "requires providers")
    def test_independent_gravity_prepares_its_own_tide_dependencies(self):
        eop = ROOT / "data/samples/eop/iers_c04_2016_12_28_2017_01_04/manifest.json"
        spice = ROOT / "data/samples/spice/de440s_2016_leap/manifest.json"
        epoch = ob.Epoch(json.loads(spice.read_text())["first_tai_ns"] + 3600*10**9)
        with tempfile.TemporaryDirectory() as folder:
            data = ob.DataSnapshot.load(eop_manifest=eop).with_ephemeris("de440s", spice, Path(folder))
            try:
                value = orbit()
                value.initial = ob.CartesianState(epoch, ob.OriginId.EARTH, ob.FrameId.GCRF,
                                                  (7e6, 1e6, 2e5), (-1000, 7200, 1200))
                config = value.hpop
                tides = config.solid_tides
                tides.mode = ob.SolidTideMode.FULL
                tides.truncate_to_gravity = True
                config.solid_tides = tides
                config.integrator = ob.HpopIntegrator.RK4
                config.fixed_step_seconds = 1
                value.hpop = config
                options = ob.HpopVariationalOptions()
                options.gravity = ob.HpopGravityTruncation(2, 0)
                end = ob.Epoch(epoch.tai_nanoseconds + 20*10**9)
                result = ob.propagate_hpop_variational(value, end, options=options, data=data)
                nominal = ob.propagate_hpop_ephemeris(value, end, data=data)[-1][-1]
                self.assertEqual(result.state.position_m, nominal.position_m)
                self.assertEqual(result.state.velocity_mps, nominal.velocity_mps)
                with self.assertRaises(ob.AnalysisCapabilityError):
                    ob.propagate_hpop_variational(value, end, options=options,
                        data=ob.DataSnapshot.load(eop_manifest=eop))
            finally:
                data = None

    def test_validation_cancellation_and_no_stale_result(self):
        value = orbit()
        for callback, error in ((lambda: True, ob.AnalysisCancelled), (lambda: 1, TypeError)):
            with self.assertRaises(error):
                ob.propagate_hpop_variational(value, ob.Epoch(1), cancelled=callback)
        options = ob.HpopVariationalOptions()
        options.absolute_tolerance = math.nan
        with self.assertRaises(ValueError):
            ob.propagate_hpop_variational(value, ob.Epoch(1), options=options)
        options.absolute_tolerance = 1e-14
        options.gravity = ob.HpopGravityTruncation(1,0)
        with self.assertRaises(ValueError):
            ob.propagate_hpop_variational(value, ob.Epoch(1), options=options)
        with self.assertRaises(ValueError):
            ob.propagate_hpop_variational(ob.OrbitInput(), ob.Epoch(1))
        result = ob.propagate_hpop_variational(value, ob.Epoch(0))
        self.assertEqual(result.state_transition, [[float(i==j) for j in range(6)] for i in range(6)])

if __name__ == "__main__":
    unittest.main()
