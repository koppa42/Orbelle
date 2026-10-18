from concurrent.futures import ThreadPoolExecutor
import gc
import hashlib
import json
import math
from pathlib import Path
import shutil
import tempfile
import unittest

import orbitforge as of

ROOT = Path(__file__).resolve().parents[3]
SAMPLE = Path(of.__file__).parent / "data/spice/de440s_2016_leap"


class EphemerisApiTests(unittest.TestCase):
    def setUp(self):
        self.assertTrue(of.ephemeris_available())
        parent = ROOT / "out/tests/python-spice"
        parent.mkdir(parents=True, exist_ok=True)
        self.temporary = tempfile.TemporaryDirectory(dir=parent)
        self.directory = Path(self.temporary.name)
        self.assertTrue(self.directory.resolve().is_relative_to(parent.resolve()))
        self.addCleanup(self.temporary.cleanup)
        self.workspace = self.directory / "workspace"
        self.provider = of.load_spice_snapshot(SAMPLE / "manifest.json", self.workspace)
        self.addCleanup(lambda: setattr(self, "provider", None))
        self.time = of.make_builtin_time_provider()
        self.epoch = self.time.from_utc(of.UtcDateTime(2016, 12, 31, 23, 59, 60, 123456789))

    def relocate(self):
        destination = self.directory / "relocated data"
        shutil.copytree(SAMPLE, destination)
        return destination

    def test_geometric_state_contract_and_leap_timing(self):
        state = self.provider.geometric_state(301, 399, self.epoch)
        self.assertEqual(state.epoch.tai_nanoseconds, self.epoch.tai_nanoseconds)
        self.assertEqual((state.target_naif_id, state.observer_naif_id), (301, 399))
        self.assertEqual(state.frame, of.FrameId.SPICE_J2000)
        self.assertEqual(state.velocity_time_scale, of.TimeScale.TDB)
        self.assertGreater(math.hypot(*state.position_m), 3.5e8)
        self.assertLess(math.hypot(*state.position_m), 4.1e8)
        self.assertLess(abs(self.provider.from_tdb(state.tdb).seconds_since(self.epoch)), 1e-6)
        midnight = self.time.from_utc(of.UtcDateTime(2017, 1, 1, 0, 0, 0, 123456789))
        self.assertAlmostEqual(self.provider.to_tdb(midnight).seconds_past_j2000 - state.tdb.seconds_past_j2000,
                               1.0, delta=1e-6)
        with self.assertRaises(ValueError):
            self.time.julian_date(self.epoch, of.TimeScale.TDB)
        with self.assertRaises(AttributeError):
            state.position_m = [0, 0, 0]

    def test_batch_missing_coverage_and_recovery(self):
        info = self.provider.data_info
        epochs = [info.first_epoch, self.epoch, info.last_epoch]
        batch = self.provider.geometric_states(10, 399, epochs)
        self.assertEqual([s.position_m for s in batch],
                         [self.provider.geometric_state(10, 399, t).position_m for t in epochs])
        self.assertEqual(self.provider.geometric_states(10, 399, []), [])
        with self.assertRaises(of.EphemerisCoverageError):
            self.provider.geometric_state(999999, 399, self.epoch)
        with self.assertRaises(of.EphemerisCoverageError):
            self.provider.geometric_states(10, 399, [self.epoch, of.Epoch(info.last_epoch.tai_nanoseconds+1)])
        with self.assertRaises(ValueError):
            self.provider.from_tdb(of.TdbTime(float("nan")))
        self.assertEqual(self.provider.geometric_state(10, 399, self.epoch).position_m, batch[1].position_m)

    def test_owned_snapshot_ignores_original_file_changes_and_cleans_up(self):
        relocated = self.relocate()
        workspace = self.directory / "relocated-runtime"
        provider = of.load_spice_snapshot(relocated / "manifest.json", workspace)
        state = provider.geometric_state(301, 399, self.epoch)
        info = provider.data_info
        (relocated / info.kernels[1].file).write_bytes(b"changed after loading")
        self.assertEqual(provider.geometric_state(301, 399, self.epoch).position_m, state.position_m)
        with self.assertRaises(of.EphemerisDataError):
            of.load_spice_snapshot(relocated / "manifest.json", workspace)
        del provider
        gc.collect()
        self.assertEqual(list(workspace.iterdir()), [])
        self.assertEqual(info.toolkit_version, "CSPICE_N0067")
        self.assertIn(info.manifest_sha256, state.source_id)

    def test_lsk_indirection_and_short_constants_are_rejected(self):
        relocated = self.relocate()
        path = relocated / "manifest.json"
        manifest = json.loads(path.read_text())
        original = (relocated / "naif0012.tls").read_bytes()
        for line in (b"DELTET/M = ( 6.239996 )", b"KERNELS_TO_LOAD = ( 'outside.bsp' )"):
            payload = original + b"\n\\begindata\n" + line + b"\n"
            (relocated / "naif0012.tls").write_bytes(payload)
            manifest["kernels"][0]["sha256"] = hashlib.sha256(payload).hexdigest()
            path.write_text(json.dumps(manifest))
            with self.assertRaises(of.EphemerisDataError):
                of.load_spice_snapshot(path, self.workspace)
        self.assertTrue(math.isfinite(self.provider.to_tdb(self.epoch).seconds_past_j2000))

    def test_threads_switch_distinct_time_contexts_without_contamination(self):
        relocated = self.relocate()
        path = relocated / "manifest.json"
        manifest = json.loads(path.read_text())
        # Deliberately synthetic timing coefficients, clearly marked in the test manifest.
        payload = (relocated / "naif0012.tls").read_bytes() + b"\n\\begindata\nDELTET/K = ( 0.003314 )\n"
        (relocated / "naif0012.tls").write_bytes(payload)
        manifest["snapshot_id"] = "synthetic-time-coefficients-for-concurrency-test"
        manifest["kernels"][0]["sha256"] = hashlib.sha256(payload).hexdigest()
        manifest["kernels"][0]["producer"] = "OrbitForge test fixture"
        path.write_text(json.dumps(manifest))
        second = of.load_spice_snapshot(path, self.workspace)
        def query(provider):
            state = provider.geometric_state(10, 399, self.epoch)
            partials = provider.apparent_sun_from_spacecraft_differential(self.epoch, [7e6, 0, 0], [0, 7500, 0])
            return (state.position_m, state.velocity_mps, state.tdb.seconds_past_j2000,
                    state.source_id, partials.position_m, partials.position_jacobian, partials.velocity_jacobian_s)
        expected = [query(self.provider), query(second)]
        self.assertNotEqual(expected[0][2], expected[1][2])
        providers = [self.provider, second]
        indices = [i % 2 for i in range(64)]
        with ThreadPoolExecutor(max_workers=8) as pool:
            actual = list(pool.map(lambda i: query(providers[i]), indices))
        self.assertEqual(actual, [expected[i] for i in indices])

    def test_apparent_sun_derivative_values_units_readonly_and_recovery(self):
        position, velocity = [7e6, 2e6, -1e6], [1000, 7500, -2000]
        result = self.provider.apparent_sun_from_spacecraft_differential(self.epoch, position, velocity)
        self.assertIsInstance(result, of.ApparentSunDifferential)
        self.assertEqual(result.position_m,
                         self.provider.apparent_sun_from_spacecraft(self.epoch, position, velocity))
        self.assertEqual((len(result.position_jacobian), len(result.velocity_jacobian_s)), (3, 3))
        self.assertTrue(all(math.isfinite(v) for row in result.position_jacobian for v in row))
        self.assertGreater(max(abs(v) for row in result.velocity_jacobian_s for v in row), 100)
        with self.assertRaises(AttributeError):
            result.position_jacobian = [[0]*3]*3
        copied = result.position_jacobian
        copied[0][0] = 123
        self.assertNotEqual(result.position_jacobian[0][0], 123)
        with self.assertRaises(ValueError):
            self.provider.apparent_sun_from_spacecraft_differential(self.epoch, [float("nan"), 0, 0], velocity)
        again = self.provider.apparent_sun_from_spacecraft_differential(self.epoch, position, velocity)
        self.assertEqual(again.position_jacobian, result.position_jacobian)
        self.assertEqual(again.velocity_jacobian_s, result.velocity_jacobian_s)


if __name__ == "__main__":
    unittest.main()
