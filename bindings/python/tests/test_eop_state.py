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
SAMPLE = Path(of.__file__).parent / "data/eop/iers_c04_2016_12_28_2017_01_04"


class EopStateApiTests(unittest.TestCase):
    def test_import_is_persistent_idempotent_and_independent_of_source(self):
        source = self.relocated()
        store = source.parent / 'installed'
        saved = of.import_eop_snapshot(source / 'manifest.json', store)
        self.assertEqual(saved, of.import_eop_snapshot(source / 'manifest.json', store))
        before = of.load_eop_snapshot(saved).at(self.epoch)
        (source / 'eop.c04').write_text('changed', encoding='utf-8')
        after = of.load_eop_snapshot(saved).at(self.epoch)
        self.assertEqual(before.sample.ut1_minus_tai_seconds, after.sample.ut1_minus_tai_seconds)
        with self.assertRaises(of.EopDataError):
            of.import_eop_snapshot(source / 'manifest.json', store)
        self.assertEqual(len(list(store.iterdir())), 1)

    def setUp(self):
        self.time = of.make_builtin_time_provider()
        self.frames = of.make_frame_provider()
        self.eop = of.load_eop_snapshot(SAMPLE / "manifest.json")
        self.epoch = self.time.from_utc(of.UtcDateTime(2017, 1, 1))

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
        provider = of.load_eop_snapshot(destination / "manifest.json")
        info = provider.data_info
        epoch = info.first_epoch
        before = provider.at(epoch)
        # The loaded immutable provider owns its nodes and no longer reads the source file.
        with (destination / "eop.c04").open("ab") as stream:
            stream.write(b"# changed after loading\n")
        self.assertEqual(provider.at(epoch).sample.ut1_minus_tai_seconds,
                         before.sample.ut1_minus_tai_seconds)
        with self.assertRaises(of.EopDataError):
            of.load_eop_snapshot(destination / "manifest.json")
        del provider
        gc.collect()
        self.assertEqual(info.sha256, hashlib.sha256((SAMPLE / "eop.c04").read_bytes()).hexdigest())
        self.assertEqual(info.reference_frame, "ITRF2020")
        self.assertIn("subdaily-tides=none", before.sample.source_id)

    def test_leap_coverage_and_provenance(self):
        midnight = self.epoch
        leap = self.time.from_utc(of.UtcDateTime(2016, 12, 31, 23, 59, 60))
        self.assertEqual(midnight.tai_nanoseconds - leap.tai_nanoseconds, 1_000_000_000)
        self.assertAlmostEqual(self.eop.at(midnight).sample.ut1_minus_tai_seconds, -36.4087130, delta=1e-12)
        self.assertLess(abs(self.eop.at(midnight).sample.ut1_minus_tai_seconds -
                            self.eop.at(leap).sample.ut1_minus_tai_seconds), 1e-6)
        for ns in (self.eop.data_info.first_epoch.tai_nanoseconds - 1,
                   self.eop.data_info.last_epoch.tai_nanoseconds + 1):
            with self.assertRaises(of.EopCoverageError):
                self.eop.at(of.Epoch(ns))
        destination = self.relocated()
        manifest_path = destination / "manifest.json"
        manifest = json.loads(manifest_path.read_text())
        manifest["first_mjd"] += 1
        manifest_path.write_text(json.dumps(manifest))
        clipped = of.load_eop_snapshot(manifest_path)
        first = clipped.data_info.first_epoch
        self.assertNotEqual(self.eop.at(first).sample.source_id, clipped.at(first).sample.source_id)

    def test_fixed_ground_state_and_roundtrip(self):
        transform = self.frames.gcrf_to_itrf_state(self.epoch, self.eop.at(self.epoch))
        ground = of.CartesianState(self.epoch, of.OriginId.EARTH, of.FrameId.ITRF,
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
        for epoch, velocity in ((of.Epoch(self.epoch.tai_nanoseconds+1), [0, 0, 0]),
                                (self.epoch, [float("nan"), 0, 0])):
            with self.assertRaises(ValueError):
                transform.apply(of.CartesianState(epoch, of.OriginId.EARTH, of.FrameId.GCRF,
                                                   [7e6, 0, 0], velocity))

    def test_path_escape_unknown_schema_and_invalid_data(self):
        destination = self.relocated()
        path = destination / "manifest.json"
        original = json.loads(path.read_text())
        for update in ({"data_file": "../outside.c04"}, {"schema_version": 1.5},
                       {"unexpected": True}, {"interpolation": "unknown"}):
            path.write_text(json.dumps(original | update))
            with self.assertRaises(of.EopDataError):
                of.load_eop_snapshot(path)
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
        with self.assertRaises(of.EopDataError):
            of.load_eop_snapshot(path)

    def test_readonly_providers_can_be_shared_between_threads(self):
        first, last = self.eop.data_info.first_epoch, self.eop.data_info.last_epoch
        epochs = [of.Epoch(first.tai_nanoseconds + (last.tai_nanoseconds-first.tai_nanoseconds)*i//15)
                  for i in range(16)]
        def calculate(epoch):
            transform = self.frames.gcrf_to_itrf_state(epoch, self.eop.at(epoch))
            return transform.rotation.matrix, transform.rotation_rate_per_second
        expected = list(map(calculate, epochs))
        with ThreadPoolExecutor(max_workers=4) as pool:
            self.assertEqual(list(pool.map(calculate, epochs)), expected)


if __name__ == "__main__":
    unittest.main()
