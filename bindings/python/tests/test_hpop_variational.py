from concurrent.futures import ThreadPoolExecutor
from pathlib import Path
import math
import json
import tempfile
import unittest
import orbelle as of

ROOT = Path(__file__).resolve().parents[3]

def orbit():
    value = of.OrbitInput()
    value.propagator = of.OrbitPropagator.HPOP
    value.initial = of.CartesianState(of.Epoch(0), of.OriginId.EARTH, of.FrameId.GCRF,
                                      (7e6, 1e6, 2e5), (-1000, 7200, 1200))
    config = of.HpopOptions()
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
        covariance = of.HpopCovarianceInput()
        covariance.state = [[(10000.0 if i < 3 else 1.0) if i == j else 0.0
                             for j in range(6)] for i in range(6)]
        for reference in (row for row in rows if row[0] in (0, 7)):
            phi = [reference[15+6*i:21+6*i] for i in range(6)]
            expected = [[sum(phi[i][k] * covariance.state[k][k] * phi[j][k] for k in range(6))
                         for j in range(6)] for i in range(6)]
            for regular in (False, True):
                value = orbit()
                value.initial = of.CartesianState(
                    of.Epoch(0), of.OriginId.EARTH, of.FrameId.GCRF, reference[3:6], reference[6:9])
                config = value.hpop
                config.integrator = of.HpopIntegrator.GAUSS_JACKSON
                config.fixed_step_seconds = 60
                config.time_regularization = regular
                config.steps_per_orbit = 120
                value.hpop = config
                end = of.Epoch(round(reference[2]*1e9))
                direct = of.propagate_hpop_covariance(value, end, covariance)
                samples = of.sample_hpop_covariance(
                    value, [end, of.Epoch(end.tai_nanoseconds//3), of.Epoch(0), end], covariance)
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
                value.initial = of.CartesianState(of.Epoch(0), of.OriginId.EARTH, of.FrameId.GCRF,
                                                  reference[3:6], reference[6:9])
                config = value.hpop
                config.time_regularization, config.vop = regular, vop
                value.hpop = config
                result = of.propagate_hpop_variational(value, of.Epoch(int(reference[2]*1e9)))
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
            return of.propagate_hpop_variational(value, of.Epoch(600*10**9))
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
            value.propagator = of.OrbitPropagator.TWO_BODY
            return False
        result = of.propagate_hpop_variational(value, of.Epoch(600*10**9), cancelled=change_input)
        self.assertEqual(result.state_transition, reference.state_transition)

    @unittest.skipUnless(of.ephemeris_available() and of.time_frames_available(), "requires providers")
    def test_independent_gravity_prepares_its_own_tide_dependencies(self):
        eop = ROOT / "data/samples/eop/iers_c04_2016_12_28_2017_01_04/manifest.json"
        spice = ROOT / "data/samples/spice/de440s_2016_leap/manifest.json"
        epoch = of.Epoch(json.loads(spice.read_text())["first_tai_ns"] + 3600*10**9)
        with tempfile.TemporaryDirectory() as folder:
            data = of.DataSnapshot.load(eop_manifest=eop).with_ephemeris("de440s", spice, Path(folder))
            try:
                value = orbit()
                value.initial = of.CartesianState(epoch, of.OriginId.EARTH, of.FrameId.GCRF,
                                                  (7e6, 1e6, 2e5), (-1000, 7200, 1200))
                config = value.hpop
                tides = config.solid_tides
                tides.mode = of.SolidTideMode.FULL
                tides.truncate_to_gravity = True
                config.solid_tides = tides
                config.integrator = of.HpopIntegrator.RK4
                config.fixed_step_seconds = 1
                value.hpop = config
                options = of.HpopVariationalOptions()
                options.gravity = of.HpopGravityTruncation(2, 0)
                end = of.Epoch(epoch.tai_nanoseconds + 20*10**9)
                result = of.propagate_hpop_variational(value, end, options=options, data=data)
                nominal = of.propagate_hpop_ephemeris(value, end, data=data)[-1][-1]
                self.assertEqual(result.state.position_m, nominal.position_m)
                self.assertEqual(result.state.velocity_mps, nominal.velocity_mps)
                with self.assertRaises(of.AnalysisCapabilityError):
                    of.propagate_hpop_variational(value, end, options=options,
                        data=of.DataSnapshot.load(eop_manifest=eop))
            finally:
                data = None

    def test_validation_cancellation_and_no_stale_result(self):
        value = orbit()
        for callback, error in ((lambda: True, of.AnalysisCancelled), (lambda: 1, TypeError)):
            with self.assertRaises(error):
                of.propagate_hpop_variational(value, of.Epoch(1), cancelled=callback)
        options = of.HpopVariationalOptions()
        options.absolute_tolerance = math.nan
        with self.assertRaises(ValueError):
            of.propagate_hpop_variational(value, of.Epoch(1), options=options)
        options.absolute_tolerance = 1e-14
        options.gravity = of.HpopGravityTruncation(1,0)
        with self.assertRaises(ValueError):
            of.propagate_hpop_variational(value, of.Epoch(1), options=options)
        with self.assertRaises(ValueError):
            of.propagate_hpop_variational(of.OrbitInput(), of.Epoch(1))
        result = of.propagate_hpop_variational(value, of.Epoch(0))
        self.assertEqual(result.state_transition, [[float(i==j) for j in range(6)] for i in range(6)])

if __name__ == "__main__":
    unittest.main()
