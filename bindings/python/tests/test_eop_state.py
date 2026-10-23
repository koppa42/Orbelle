from concurrent.futures import ThreadPoolExecutor
import gc
import hashlib
import json
import math
from pathlib import Path
import shutil
import tempfile
import unittest

import orbelle as ob


ROOT = Path(__file__).resolve().parents[3]
SAMPLE = Path(ob.__file__).parent / "data/eop/iers_c04_2016_12_28_2017_01_04"


class EopStateApiTests(unittest.TestCase):
    def test_import_is_persistent_idempotent_and_independent_of_source(self):
        source = self.relocated()
        store = source.parent / 'installed'
        saved = ob.import_eop_snapshot(source / 'manifest.json', store)
        self.assertEqual(saved, ob.import_eop_snapshot(source / 'manifest.json', store))
        before = ob.load_eop_snapshot(saved).at(self.epoch)
        (source / 'eop.c04').write_text('changed', encoding='utf-8')
        after = ob.load_eop_snapshot(saved).at(self.epoch)
        self.assertEqual(before.sample.ut1_minus_tai_seconds, after.sample.ut1_minus_tai_seconds)
        with self.assertRaises(ob.EopDataError):
            ob.import_eop_snapshot(source / 'manifest.json', store)
        self.assertEqual(len(list(store.iterdir())), 1)

    def setUp(self):
        self.time = ob.make_builtin_time_provider()
        self.frames = ob.make_frame_provider()
        self.eop = ob.load_eop_snapshot(SAMPLE / "manifest.json")
        self.epoch = self.time.from_utc(ob.UtcDateTime(2017, 1, 1))

    def relocated(self):
        parent = ROOT / "out/tests/python-eop"
        parent.mkdir(parents=True, exist_ok=True)
        temporary = tempfile.TemporaryDirectory(dir=parent)
        self.addCleanup(temporary.cleanup)
        destination = Path(temporary.name) / "relocated snapshot"
        shutil.copytree(SAMPLE, destination)
        return destination

    def test_installed_data_and_owned_provider_survive_relocation_and_file_change(self):
        destination = self.relocated()
        provider = ob.load_eop_snapshot(destination / "manifest.json")
        info = provider.data_info
        epoch = info.first_epoch
        before = provider.at(epoch)
        # The loaded immutable provider owns its nodes and no longer reads the source file.
        with (destination / "eop.c04").open("ab") as stream:
            stream.write(b"# changed after loading\n")
        self.assertEqual(provider.at(epoch).sample.ut1_minus_tai_seconds,
                         before.sample.ut1_minus_tai_seconds)
        with self.assertRaises(ob.EopDataError):
            ob.load_eop_snapshot(destination / "manifest.json")
        del provider
        gc.collect()
        self.assertEqual(info.sha256, hashlib.sha256((SAMPLE / "eop.c04").read_bytes()).hexdigest())
        self.assertEqual(info.reference_frame, "ITRF2020")
        self.assertIn("subdaily-tides=none", before.sample.source_id)

    def test_leap_coverage_and_provenance(self):
        midnight = self.epoch
        leap = self.time.from_utc(ob.UtcDateTime(2016, 12, 31, 23, 59, 60))
        self.assertEqual(midnight.tai_nanoseconds - leap.tai_nanoseconds, 1_000_000_000)
        self.assertAlmostEqual(self.eop.at(midnight).sample.ut1_minus_tai_seconds, -36.4087130, delta=1e-12)
        self.assertLess(abs(self.eop.at(midnight).sample.ut1_minus_tai_seconds -
                            self.eop.at(leap).sample.ut1_minus_tai_seconds), 1e-6)
        for ns in (self.eop.data_info.first_epoch.tai_nanoseconds - 1,
                   self.eop.data_info.last_epoch.tai_nanoseconds + 1):
            with self.assertRaises(ob.EopCoverageError):
                self.eop.at(ob.Epoch(ns))
        destination = self.relocated()
        manifest_path = destination / "manifest.json"
        manifest = json.loads(manifest_path.read_text())
        manifest["first_mjd"] += 1
        manifest_path.write_text(json.dumps(manifest))
        clipped = ob.load_eop_snapshot(manifest_path)
        first = clipped.data_info.first_epoch
        self.assertNotEqual(self.eop.at(first).sample.source_id, clipped.at(first).sample.source_id)

    def test_finite_extrapolation_across_leap_second_and_velocity_transform(self):
        destination = self.relocated()
        path = destination / 'manifest.json'
        manifest = json.loads(path.read_text(encoding='utf-8'))
        manifest['last_mjd'] = 57753  # 2016-12-31, immediately before the leap second.
        path.write_text(json.dumps(manifest), encoding='utf-8')
        strict = ob.load_eop_snapshot(path)
        extended = ob.load_eop_snapshot(path, extrapolation_days=2)
        self.assertEqual(extended.data_info.sha256, strict.data_info.sha256)
        self.assertEqual(extended.data_info.last_epoch.tai_nanoseconds,
                         strict.data_info.last_epoch.tai_nanoseconds)
        self.assertEqual(extended.data_info.extrapolation_days, 2)
        leap = self.time.from_utc(ob.UtcDateTime(2016, 12, 31, 23, 59, 60))
        midnight = self.time.from_utc(ob.UtcDateTime(2017, 1, 1))
        a, b = extended.at(leap), extended.at(midnight)
        self.assertAlmostEqual(b.sample.ut1_minus_tai_seconds - a.sample.ut1_minus_tai_seconds,
                               a.rates.ut1_minus_tai_seconds_per_second, delta=1e-13)
        self.assertIn('extrapolation=endpoint-linear-tai-v1', b.sample.source_id)
        with self.assertRaises(ob.EopCoverageError):
            strict.at(midnight)
        for limit in (-1, 31):
            with self.assertRaises(ValueError):
                ob.load_eop_snapshot(path, extrapolation_days=limit)
        # Independently compare transformed velocity to a central position difference.
        def fixed_position(offset):
            t = ob.Epoch(midnight.tai_nanoseconds + round(offset * 1e9))
            r = [7e6, 7500 * offset, 0]
            return self.frames.gcrf_to_itrf(t, extended.at(t).sample).apply_position(r)
        before, after = fixed_position(-0.1), fixed_position(0.1)
        state = ob.CartesianState(midnight, ob.OriginId.EARTH, ob.FrameId.GCRF,
                                  [7e6, 0, 0], [0, 7500, 0])
        result = self.frames.gcrf_to_itrf_state(midnight, b).apply(state)
        for i in range(3):
            self.assertAlmostEqual((after[i] - before[i]) / 0.2, result.velocity_mps[i], delta=0.001)

    def test_fixed_ground_state_and_roundtrip(self):
        transform = self.frames.gcrf_to_itrf_state(self.epoch, self.eop.at(self.epoch))
        ground = ob.CartesianState(self.epoch, ob.OriginId.EARTH, ob.FrameId.ITRF,
                                   [6378137, 0, 0], [0, 0, 0])
        inertial = transform.inverse(ground)
        self.assertAlmostEqual(math.hypot(*inertial.velocity_mps), 465.1011, delta=0.001)
        self.assertGreater(inertial.position_m[0] * inertial.velocity_mps[1] -
                           inertial.position_m[1] * inertial.velocity_mps[0], 0)
        back = transform.apply(inertial)
        for a, b in zip(back.position_m, ground.position_m):
            self.assertAlmostEqual(a, b, delta=5e-8)
        for a, b in zip(back.velocity_mps, ground.velocity_mps):
            self.assertAlmostEqual(a, b, delta=5e-10)
        with self.assertRaises(ValueError):
            transform.apply(ground)
        for epoch, velocity in ((ob.Epoch(self.epoch.tai_nanoseconds+1), [0, 0, 0]),
                                (self.epoch, [float("nan"), 0, 0])):
            with self.assertRaises(ValueError):
                transform.apply(ob.CartesianState(epoch, ob.OriginId.EARTH, ob.FrameId.GCRF,
                                                   [7e6, 0, 0], velocity))

    def test_path_escape_unknown_schema_and_invalid_data(self):
        destination = self.relocated()
        path = destination / "manifest.json"
        original = json.loads(path.read_text())
        for update in ({"data_file": "../outside.c04"}, {"schema_version": 1.5},
                       {"unexpected": True}, {"interpolation": "unknown"}):
            path.write_text(json.dumps(original | update))
            with self.assertRaises(ob.EopDataError):
                ob.load_eop_snapshot(path)
        # A valid hash alone is insufficient: malformed domain values are rejected too.
        lines = (destination / "eop.c04").read_bytes().splitlines(keepends=True)
        i = next(i for i, line in enumerate(lines) if line.strip() and not line.startswith(b"#"))
        fields = lines[i].split()
        fields[13] = b"-0.5"
        lines[i] = b" ".join(fields) + b"\n"
        payload = b"".join(lines)
        (destination / "eop.c04").write_bytes(payload)
        original["sha256"] = hashlib.sha256(payload).hexdigest()
        path.write_text(json.dumps(original))
        with self.assertRaises(ob.EopDataError):
            ob.load_eop_snapshot(path)

    def test_readonly_providers_can_be_shared_between_threads(self):
        first, last = self.eop.data_info.first_epoch, self.eop.data_info.last_epoch
        epochs = [ob.Epoch(first.tai_nanoseconds + (last.tai_nanoseconds-first.tai_nanoseconds)*i//15)
                  for i in range(16)]
        def calculate(epoch):
            transform = self.frames.gcrf_to_itrf_state(epoch, self.eop.at(epoch))
            return transform.rotation.matrix, transform.rotation_rate_per_second
        expected = list(map(calculate, epochs))
        with ThreadPoolExecutor(max_workers=4) as pool:
            self.assertEqual(list(pool.map(calculate, epochs)), expected)


if __name__ == "__main__":
    unittest.main()
