from concurrent.futures import ThreadPoolExecutor
from datetime import date, timedelta
from pathlib import Path
import gc
import hashlib
import json
import shutil
import sqlite3
import tempfile
import time
import unittest

# Native value/format compatibility tests; public editing is in test_objects.py.
from orbitforge import _native as of

SAMPLES = Path(of.__file__).parent / 'data/eop'
C04 = SAMPLES / 'iers_c04_2000_01_01_2000_01_02'
FINALS = SAMPLES / 'iers_finals2000a_2026_09_01_2026_11_01'


@unittest.skipUnless(of.data_catalog_available(), 'Data catalog disabled')
class DataCatalogTests(unittest.TestCase):
    def setUp(self):
        self.temp = tempfile.TemporaryDirectory()
        self.addCleanup(self.temp.cleanup)
        self.root = Path(self.temp.name)

    def project(self, provider):
        info = provider.data_info
        project = of.Project('Pinned', info.first_epoch, info.last_epoch)
        project.earth_orientation = info
        return project

    def test_full_c04_and_runtime_time_data_survive_reopening(self):
        original = Path(of.__file__).parent / 'notices/iers_leap_seconds_2026_07.dat'
        source = self.root / 'Leap_Second.dat'
        source.write_bytes(original.read_bytes().replace(b'28 June 2027', b'28 June 2028'))
        catalog = of.DataCatalog(self.root / 'catalog')
        time_file = catalog.install_time(source)
        time_provider = of.load_time_snapshot(time_file)
        self.assertEqual(time_provider.to_utc(time_provider.from_utc(of.UtcDateTime(2028, 1, 1))).year, 2028)
        entry = catalog.import_c04(C04 / 'eop.c04', 'https://example.test/c04', '2026-10-10', time_file)
        provider = catalog.load_eop(entry.lock_sha256)
        self.assertEqual(provider.data_info.time_data.sha256, time_provider.data_info.sha256)
        lock = self.project(provider).data_lock
        del catalog
        reopened = of.DataCatalog(self.root / 'catalog')
        self.assertEqual(reopened.resolve_eop(lock).data_info.time_data.sha256, time_provider.data_info.sha256)
        self.assertEqual(Path(reopened.time_file(time_provider.data_info.sha256)), Path(time_file))

    def test_acquisition_receipts_are_persistent_local_metadata(self):
        store = self.root / 'catalog'
        catalog = of.DataCatalog(store)
        entry = catalog.install_eop(C04 / 'manifest.json')
        lock = self.project(catalog.load_eop(entry.lock_sha256)).data_lock
        self.assertEqual(entry.acquisition, of.DataAcquisition.unknown)
        self.assertIsNone(entry.acquired_at_unix_seconds)
        catalog.record_acquisition(entry.lock_sha256, of.DataAcquisition.bundled)
        self.assertEqual(catalog.snapshots[0].acquisition, of.DataAcquisition.bundled)
        self.assertIsNone(catalog.snapshots[0].acquired_at_unix_seconds)
        before = int(time.time())
        catalog.record_acquisition(entry.lock_sha256, of.DataAcquisition.downloaded)
        receipt = catalog.snapshots[0]
        self.assertGreaterEqual(receipt.acquired_at_unix_seconds, before)
        self.assertLessEqual(receipt.acquired_at_unix_seconds, int(time.time()))
        del catalog
        reopened = of.DataCatalog(store)
        # Startup re-registration of bundled bytes must not erase a real receipt.
        reopened.record_acquisition(entry.lock_sha256, of.DataAcquisition.bundled)
        again = reopened.install_eop(C04 / 'manifest.json')
        self.assertEqual(again.acquisition, of.DataAcquisition.downloaded)
        self.assertEqual(again.acquired_at_unix_seconds, receipt.acquired_at_unix_seconds)
        self.assertEqual(again.lock_sha256, entry.lock_sha256)
        self.assertEqual(self.project(reopened.resolve_eop(lock)).data_lock.sha256, lock.sha256)
        reopened.record_acquisition(entry.lock_sha256, of.DataAcquisition.imported)
        self.assertEqual(reopened.snapshots[0].acquisition, of.DataAcquisition.imported)
        with self.assertRaises(RuntimeError):
            reopened.record_acquisition('a' * 64, of.DataAcquisition.downloaded)
        del reopened

    def test_schema_one_migration_does_not_invent_acquisition_dates(self):
        store = self.root / 'catalog'
        catalog = of.DataCatalog(store)
        entry = catalog.install_eop(C04 / 'manifest.json')
        del catalog
        with sqlite3.connect(store / 'catalog.sqlite') as db:
            db.execute('DROP TABLE acquisitions')
            db.execute('PRAGMA user_version=1')
        db.close()
        upgraded = of.DataCatalog(store)
        self.assertEqual(upgraded.snapshots[0].lock_sha256, entry.lock_sha256)
        self.assertEqual(upgraded.snapshots[0].acquisition, of.DataAcquisition.unknown)
        self.assertIsNone(upgraded.snapshots[0].acquired_at_unix_seconds)
        self.assertTrue(upgraded.load_eop(entry.lock_sha256).data_info.id)
        del upgraded
        with sqlite3.connect(store / 'catalog.sqlite') as db:
            self.assertEqual(db.execute('PRAGMA user_version').fetchone()[0], 2)
        db.close()

    def test_shared_content_preserves_two_independent_scenario_locks(self):
        source = self.root / 'source'
        shutil.copytree(C04, source)
        path = source / 'manifest.json'
        catalog = of.DataCatalog(self.root / '中心')
        first = catalog.install_eop(path)
        original = self.project(catalog.load_eop(first.lock_sha256))
        changed = json.loads(path.read_text())
        changed['snapshot_id'] += '-independent'
        path.write_text(json.dumps(changed))
        second = catalog.install_eop(path)
        newer = self.project(catalog.load_eop(second.lock_sha256))
        self.assertNotEqual(first.lock_sha256, second.lock_sha256)
        self.assertEqual(len(list((Path(catalog.root) / 'blobs/sha256').iterdir())), 1)
        self.assertEqual(len(catalog.snapshots), 2)
        self.assertEqual(catalog.install_eop(path).lock_sha256, second.lock_sha256)
        catalog.set_setting('default-eop', second.lock_sha256)
        path.write_text('removed source')
        reopened = of.DataCatalog(catalog.root)
        self.assertEqual(reopened.setting('default-eop'), second.lock_sha256)
        for project in [original, newer]:
            restored = reopened.resolve_eop(project.data_lock)
            self.assertEqual(restored.data_info.id, project.earth_orientation.id)
            self.assertEqual(project.to_scenario().data_lock.sha256, project.data_lock.sha256)
        del reopened, catalog

    def test_parallel_imports_publish_once_and_keep_no_temporary_files(self):
        store = self.root / 'catalog'
        of.DataCatalog(store)
        def install(_):
            catalog = of.DataCatalog(store)
            return catalog.install_eop(C04 / 'manifest.json').lock_sha256
        with ThreadPoolExecutor(max_workers=4) as pool:
            keys = list(pool.map(install, range(8)))
        self.assertEqual(len(set(keys)), 1)
        catalog = of.DataCatalog(store)
        self.assertEqual(len(catalog.snapshots), 1)
        self.assertEqual(list((store / 'staging').iterdir()), [])
        del catalog

    def test_complete_finals_import_preserves_every_byte_and_hash(self):
        catalog = of.DataCatalog(self.root / 'catalog')
        raw = FINALS / 'finals2000A.data'
        entry = catalog.import_finals(raw, 'https://example.org/source', '2026-10-10')
        manifest = json.loads(Path(entry.manifest).read_text())
        content = Path(entry.manifest).with_name(manifest['data_file']).read_bytes()
        self.assertEqual(content, raw.read_bytes())
        self.assertEqual(manifest['data_file'], 'finals2000A.all')
        self.assertEqual(manifest['sha256'], hashlib.sha256(raw.read_bytes()).hexdigest())
        self.assertEqual(manifest['source_sha256'], hashlib.sha256(raw.read_bytes()).hexdigest())
        self.assertEqual(catalog.import_finals(raw, 'https://mirror.example.org/source',
                                              '2026-10-11').lock_sha256, entry.lock_sha256)
        self.assertEqual(len(list((Path(catalog.root) / 'blobs/sha256').iterdir())), 1)
        self.assertEqual(len(catalog.snapshots), 1)
        self.assertEqual(list((Path(catalog.root) / 'staging').iterdir()), [])
        del catalog

    def history(self):
        # Synthetic dates exercise the complete-file parser before the C04 1984 cutoff.
        rows = (FINALS / 'finals2000A.data').read_bytes().splitlines()[:4]
        for i in range(len(rows)):
            day = date(1973, 1, 2) + timedelta(days=i)
            mjd = (day - date(1858, 11, 17)).days
            rows[i] = f'{day.year % 100:2}{day.month:2}{day.day:2} {mjd:8.2f}'.encode() + rows[i][15:]
        return rows

    def test_full_history_retains_partial_prediction_tail_and_pins_source_changes(self):
        catalog = of.DataCatalog(self.root / 'catalog')
        rows = self.history()
        rows[-1] = rows[-1][:95] + b' ' * 39 + rows[-1][134:]
        raw = self.root / 'finals2000A.all'
        raw.write_bytes(b'\r\n'.join(rows) + b'\r\n')
        entry = catalog.import_finals(raw, 'https://example.org/source', '2026-10-10')
        provider = catalog.load_eop(entry.lock_sha256)
        clock = of.make_builtin_time_provider()
        self.assertEqual(clock.to_utc(entry.first).year, 1973)
        self.assertEqual(clock.to_utc(entry.last).day, 4)
        self.assertEqual(Path(entry.manifest).with_name('finals2000A.all').read_bytes(), raw.read_bytes())
        with self.assertRaises((RuntimeError, ValueError)):
            provider.at(clock.from_utc(of.UtcDateTime(1973, 1, 5)))
        pinned = self.project(provider).data_lock
        rows[-1] = rows[-1][:18] + b' 0.123456' + rows[-1][27:]
        raw.write_bytes(b'\r\n'.join(rows) + b'\r\n')
        newer = catalog.import_finals(raw, 'https://example.org/source', '2026-10-11')
        self.assertNotEqual(entry.lock_sha256, newer.lock_sha256)
        self.assertEqual(catalog.resolve_eop(pinned).data_info.sha256, provider.data_info.sha256)
        self.assertEqual(len(catalog.snapshots), 2)
        del catalog

    def test_incomplete_interior_corrupt_values_and_dates_do_not_install(self):
        catalog = of.DataCatalog(self.root / 'catalog')
        rows = self.history()
        raw = self.root / 'bad.all'
        damaged = [
            [rows[0], rows[1][:95] + b' ' * 39 + rows[1][134:], *rows[2:]],
            [rows[0][:18] + b'      nan' + rows[0][27:], *rows[1:]],
            [rows[0][:97] + b' ' * 9 + rows[0][106:], *rows[1:]],
            [rows[0], rows[0], *rows[2:]],
            [b'731231' + rows[0][6:], *rows[1:]],
            [*rows[:-1], rows[-1][:20]],
        ]
        for records in damaged:
            raw.write_bytes(b'\n'.join(records) + b'\n')
            with self.assertRaises((RuntimeError, ValueError)):
                catalog.import_finals(raw, 'https://example.org/source', '2026-10-10')
            self.assertEqual(len(catalog.snapshots), 0)
            self.assertEqual(list((Path(catalog.root) / 'staging').iterdir()), [])
        del catalog

    def test_invalid_content_never_replaces_existing_and_resolution_revalidates(self):
        catalog = of.DataCatalog(self.root / 'catalog')
        entry = catalog.install_eop(C04 / 'manifest.json')
        lock = self.project(catalog.load_eop(entry.lock_sha256)).data_lock
        blob = next((Path(catalog.root) / 'blobs/sha256').iterdir())
        blob.write_bytes(b'damaged')
        with self.assertRaises((RuntimeError, ValueError)):
            catalog.resolve_eop(lock)
        with self.assertRaises((RuntimeError, ValueError)):
            catalog.install_eop(C04 / 'manifest.json')
        self.assertEqual(blob.read_bytes(), b'damaged')
        self.assertEqual(len(catalog.snapshots), 1)
        with self.assertRaises(RuntimeError):
            catalog.resolve_eop(of.DataLock())
        del catalog

    def test_published_snapshot_recovers_after_missing_database_commit(self):
        store = self.root / 'catalog'
        catalog = of.DataCatalog(store)
        entry = catalog.install_eop(C04 / 'manifest.json')
        del catalog
        gc.collect()
        with sqlite3.connect(store / 'catalog.sqlite') as db:
            db.execute('DELETE FROM dependencies')
            db.execute('DELETE FROM snapshots')
        db.close()
        recovered = of.DataCatalog(store)
        self.assertEqual([row.lock_sha256 for row in recovered.snapshots], [entry.lock_sha256])
        self.assertTrue(recovered.load_eop(entry.lock_sha256).data_info.id)
        del recovered

    def test_manifest_tampering_and_newer_catalog_schema_are_rejected(self):
        store = self.root / 'catalog'
        catalog = of.DataCatalog(store)
        entry = catalog.install_eop(C04 / 'manifest.json')
        manifest = Path(entry.manifest)
        manifest.write_text(manifest.read_text() + ' ')
        with self.assertRaises(RuntimeError):
            catalog.load_eop(entry.lock_sha256)
        del catalog
        with sqlite3.connect(store / 'catalog.sqlite') as db:
            db.execute('PRAGMA user_version=99')
        db.close()
        with self.assertRaises(RuntimeError):
            of.DataCatalog(store)
