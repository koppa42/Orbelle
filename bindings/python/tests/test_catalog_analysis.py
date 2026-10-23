from concurrent.futures import ThreadPoolExecutor
from datetime import date, timedelta
from pathlib import Path
import json
import shutil
import tempfile
import threading
import unittest

import orbelle as ob
from orbelle import _native, units as u

SAMPLES = Path(_native.__file__).parent / "data/eop"
C04 = SAMPLES / "iers_c04_2000_01_01_2000_01_02"
FINALS = SAMPLES / "iers_finals2000a_2026_09_01_2026_11_01"


@unittest.skipUnless(ob.data_catalog_available(), "Data catalog disabled")
class CatalogAnalysisTests(unittest.TestCase):
    def setUp(self):
        self.temp = tempfile.TemporaryDirectory()
        self.addCleanup(self.temp.cleanup)
        self.root = Path(self.temp.name)
        self.catalog = ob.DataCatalog(self.root / "catalog")
        self.addCleanup(self.close_catalog)
        self.clock = ob.make_builtin_time_provider()
        self.entry = self.catalog.install_eop(C04 / "manifest.json")
        self.catalog.activate_eop(self.entry.lock_sha256)

    def close_catalog(self):
        self.catalog = None

    def at(self, day, hour=0):
        return self.clock.from_utc(ob.UtcDateTime(2000, 1, day, hour))

    def install_finals(self):
        # Synthetic overlapping dates isolate selection policy, not EOP accuracy.
        rows = (FINALS / "finals2000A.data").read_bytes().splitlines()[:4]
        for index, row in enumerate(rows):
            day = date(2000, 1, 1) + timedelta(days=index)
            mjd = (day - date(1858, 11, 17)).days
            rows[index] = f"{day.year % 100:2}{day.month:2}{day.day:2} {mjd:8.2f}".encode() + row[15:]
        raw = self.root / "finals.all"
        raw.write_bytes(b"\n".join(rows) + b"\n")
        entry = self.catalog.import_finals(raw, "https://example.test/finals", "2026-10-13")
        self.catalog.activate_eop(entry.lock_sha256)
        return entry

    def revised_c04(self):
        source = self.root / "revised"
        shutil.copytree(C04, source)
        manifest = source / "manifest.json"
        value = json.loads(manifest.read_text())
        value["snapshot_id"] += "-revised"
        manifest.write_text(json.dumps(value))
        return self.catalog.install_eop(manifest)

    def test_priority_full_coverage_before_extrapolation_and_persistent_policy(self):
        finals = self.install_finals()
        self.assertEqual(self.catalog.preferred_eop, ob.EopProduct.C04)
        first, last = self.at(1, 1), self.at(1, 2)
        original = self.catalog.snapshot(first, last)
        self.assertEqual(original.eop_info.id, self.entry.snapshot_id)
        self.catalog.set_extrapolation_days(ob.EopProduct.C04, 3)
        # An unused extrapolation preference is not a different data identity.
        self.assertEqual(self.catalog.snapshot(first, last).data_lock.sha256, original.data_lock.sha256)
        selected = self.catalog.snapshot(self.at(2), self.at(3))
        self.assertEqual(selected.eop_info.id, finals.snapshot_id)
        self.assertEqual(selected.eop_info.extrapolation_days, 0)
        self.catalog.preferred_eop = ob.EopProduct.FINALS2000A
        self.assertEqual(self.catalog.snapshot(first, last).eop_info.id, finals.snapshot_id)
        reopened = ob.DataCatalog(self.catalog.root)
        self.assertEqual(reopened.preferred_eop, ob.EopProduct.FINALS2000A)
        self.assertEqual(reopened.extrapolation_days(ob.EopProduct.C04), 3)

    def test_finite_extrapolation_boundary_and_diagnostics(self):
        self.catalog.set_extrapolation_days(ob.EopProduct.C04, 3)
        end = ob.Epoch(self.entry.last.tai_nanoseconds + 3 * 86400 * 10**9)
        data = self.catalog.snapshot(self.entry.last, end)
        self.assertEqual(data.eop_info.extrapolation_days, 3)
        with self.assertRaises(ob.EopCoverageError) as error:
            self.catalog.snapshot(self.entry.last, ob.Epoch(end.tai_nanoseconds + 1))
        self.assertIn("TAI ns", str(error.exception))
        self.assertIn("original", str(error.exception))
        self.assertIn("extrapolation days=3", str(error.exception))
        for days in [-1, 31]:
            with self.assertRaises(ValueError):
                self.catalog.set_extrapolation_days(ob.EopProduct.C04, days)
        with self.assertRaises(ValueError):
            self.catalog.snapshot(self.entry.last, self.entry.first)
        self.catalog.set_setting("extrapolation-c04-days", "invalid")
        self.assertEqual(self.catalog.extrapolation_days(ob.EopProduct.C04), 0)

    def test_catalog_prepare_fixes_resources_and_requires_new_data_consent(self):
        project = ob.Project("Selection", self.at(1, 1), self.at(1, 2))
        station = project.objects.ground_station("Station", location=ob.Wgs84Location(u.deg(0), u.deg(0)))
        project.earth_orientation = self.catalog.load_eop(self.entry.lock_sha256).data_info
        old = project.prepare(data=self.catalog)
        revision = project.revision
        newer = self.revised_c04()
        self.catalog.activate_eop(newer.lock_sha256)
        with self.assertRaises(ob.DataChangedError) as changed:
            project.prepare(data=self.catalog)
        plan = changed.exception.plan
        self.assertTrue(plan.data_changes)
        self.catalog.activate_eop(self.entry.lock_sha256)
        approved = plan.prepare(accept_data_changes=True)
        self.assertEqual(approved.data.eop_info.id, newer.snapshot_id)
        self.assertEqual(old.data.eop_info.id, self.entry.snapshot_id)
        self.assertEqual(project.revision, revision)
        with self.assertRaises(ob.DataChangedError):
            plan.prepare()
        trajectory = approved.states(station, step=u.s(60), frame=ob.FrameId.GCRF)
        expected = trajectory.position.values
        Path(newer.manifest).unlink()
        self.catalog = None
        self.assertEqual(approved.states(station, step=u.s(60)).position.values, expected)
        station.name = "Renamed"
        with self.assertRaises(ob.RevisionConflictError):
            plan.prepare(accept_data_changes=True)

    def test_broken_current_pointer_does_not_reactivate_older_cached_file(self):
        newer = self.revised_c04()
        self.catalog.activate_eop(newer.lock_sha256)
        Path(newer.manifest).write_text("damaged")
        with self.assertRaises(ob.EopCoverageError) as error:
            self.catalog.snapshot(self.at(1), self.at(2))
        self.assertIn("SHA256", str(error.exception))
        finals = self.install_finals()
        data = self.catalog.snapshot(self.at(1), self.at(2))
        self.assertEqual(data.eop_info.id, finals.snapshot_id)
        self.assertTrue(any("SHA256" in note for note in data.selection_notes))

    def test_time_only_and_pure_inertial_paths_do_not_require_eop(self):
        self.catalog.set_setting("current-c04", "invalid")
        data = self.catalog.snapshot(self.at(1), self.at(2), earth_orientation=False)
        self.assertIsNone(data.eop_info)
        self.assertEqual([item.role for item in data.data_lock.references], ["utc-time"])
        project = ob.Project("No external dependency", ob.Epoch(0), ob.Epoch(10))
        self.assertFalse(project.prepare().data.data_lock.references)
        with self.assertRaises(ob.TimeDataRangeError):
            self.catalog.snapshot(ob.Epoch(10**18), ob.Epoch(10**18), earth_orientation=False)
        self.catalog.set_setting("current-utc", "a" * 64)
        with self.assertRaises(RuntimeError):
            self.catalog.snapshot(self.at(1), self.at(2), earth_orientation=False)
        with self.assertRaises(TypeError):
            project.prepare(data="catalog")

    def test_atomic_utc_eop_updates_and_snapshot_ownership(self):
        original = Path(_native.__file__).parent / "notices/iers_leap_seconds_2026_07.dat"
        source = self.root / "later-time.dat"
        source.write_bytes(original.read_bytes().replace(b"28 June 2027", b"28 June 2028"))
        time_file = self.catalog.install_time(source)
        time_hash = ob.load_time_snapshot(time_file).data_info.sha256
        newer = self.catalog.install_eop(C04 / "manifest.json", time_file)
        # Installing is not activation; mixed UTC/EOP cannot be activated individually.
        with self.assertRaises(RuntimeError):
            self.catalog.activate_eop(newer.lock_sha256)
        old_settings = {"current-utc": "", "current-c04": self.entry.lock_sha256}
        new_settings = {"current-utc": time_hash, "current-c04": newer.lock_sha256}
        stop = threading.Event()
        started = threading.Event()

        def update():
            writer = ob.DataCatalog(self.catalog.root)
            started.set()
            while not stop.is_set():
                writer.set_settings(new_settings)
                writer.set_settings(old_settings)

        with ThreadPoolExecutor(max_workers=1) as pool:
            task = pool.submit(update)
            try:
                self.assertTrue(started.wait(5))
                for _ in range(20):
                    data = self.catalog.snapshot(self.at(1), self.at(2))
                    self.assertEqual(data.eop_info.time_data.sha256, data.time_info.sha256)
            finally:
                stop.set()
            task.result(timeout=10)
        self.catalog.set_settings(new_settings)
        fixed = self.catalog.snapshot(self.at(1), self.at(2))
        self.catalog.set_settings(old_settings)
        self.assertEqual(fixed.time_info.sha256, time_hash)


if __name__ == "__main__":
    unittest.main()
