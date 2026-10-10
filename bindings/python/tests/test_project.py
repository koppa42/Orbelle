from pathlib import Path
import hashlib
import json
import struct
import tempfile
import unittest
import zlib
import orbitforge as of


class ProjectTests(unittest.TestCase):
    def bound_project(self):
        project = self.make_project()
        manifest = Path(of.__file__).parent / 'data/eop/iers_c04_2000_01_01_2000_01_02/manifest.json'
        project.earth_orientation = of.load_eop_snapshot(manifest).data_info
        return project

    def test_data_lock_has_independent_sha256_and_is_preserved_in_runtime_scenarios(self):
        project = self.bound_project()
        lock = project.data_lock
        self.assertEqual(lock.sha256, hashlib.sha256(lock.canonical_bytes()).hexdigest())
        self.assertEqual([r.role for r in lock.references], ['earth-orientation', 'utc-time'])
        self.assertEqual(lock.references[0].content_sha256, project.earth_orientation.sha256)
        self.assertEqual(lock.references[1].content_sha256, project.earth_orientation.time_data.sha256)
        self.assertEqual(of.DataLock(list(reversed(lock.references))).sha256, lock.sha256)
        original = project.to_scenario()
        self.assertEqual(original.data_lock.sha256, lock.sha256)
        changed = original.with_data_lock(of.DataLock())
        self.assertEqual(changed.revision, original.revision + 1)
        self.assertEqual(original.data_lock.sha256, lock.sha256)
        self.assertEqual(project.renamed_satellite('二体', 'New name').data_lock.sha256, lock.sha256)
        with tempfile.TemporaryDirectory() as directory:
            path = Path(directory) / 'bound.orbitforge'
            project.save(path)
            restored = of.load_project(path)
            self.assertEqual(restored.data_lock.sha256, lock.sha256)
            self.assertEqual(restored.to_scenario().data_lock.sha256, lock.sha256)

    def test_content_hash_matches_the_actual_pinned_file(self):
        directory = Path(of.__file__).parent / 'data/eop/iers_c04_2000_01_01_2000_01_02'
        manifest = json.loads((directory / 'manifest.json').read_text(encoding='utf-8'))
        raw = (directory / manifest['data_file']).read_bytes()
        self.assertEqual(self.bound_project().data_lock.references[0].content_sha256,
                         hashlib.sha256(raw).hexdigest())

    def test_missing_inconsistent_or_wrong_digest_locks_fail_even_with_valid_crcs(self):
        project = self.bound_project()
        with tempfile.TemporaryDirectory() as directory:
            path = Path(directory) / 'bound.orbitforge'
            project.save(path)
            original = path.read_bytes()
            self.assertEqual(struct.unpack_from('<I', original, 8)[0], 3)
            def rewrite(tag_to_change, mutate):
                sections = []
                offset = 24
                while offset < len(original):
                    tag, version, size, _ = struct.unpack_from('<IIQI', original, offset)
                    body = original[offset + 20:offset + 20 + size]
                    offset += 20 + size
                    if tag.to_bytes(4, 'little') == tag_to_change:
                        body = mutate(body)
                    if body is not None:
                        sections.append(struct.pack('<IIQI', tag, version, len(body), zlib.crc32(body)) + body)
                payload = b''.join(sections)
                path.write_bytes(original[:12] + struct.pack('<QI', len(payload), zlib.crc32(payload)) + payload)
            rewrite(b'LOCK', lambda body: None)
            with self.assertRaisesRegex(of.ProjectError, 'Missing scenario data lock'):
                of.load_project(path)
            rewrite(b'LOCK', lambda body: body[:-1] + (b'0' if body[-1:] != b'0' else b'1'))
            with self.assertRaisesRegex(of.ProjectError, 'SHA256 mismatch'):
                of.load_project(path)
            digest = project.earth_orientation.sha256.encode('ascii')
            changed = (b'0' if digest[:1] != b'0' else b'1') + digest[1:]
            rewrite(b'DATA', lambda body: body.replace(digest, changed, 1))
            with self.assertRaisesRegex(of.ProjectError, 'references disagree'):
                of.load_project(path)

    def test_names_copies_and_deletions_preserve_stable_ids_and_old_values(self):
        original = self.make_project()
        renamed = original.renamed_satellite('二体', '观测卫星')
        self.assertEqual(original.satellites[0].name, '二体')
        self.assertEqual(renamed.satellites[0].orbit.object_id, '二体')
        copied = renamed.with_satellite_copy('二体', 'copy-1', '观测卫星 副本')
        self.assertEqual(len(copied.satellites), 3)
        states = copied.to_scenario().sample([copied.first, copied.last])
        self.assertEqual(states[0].states[1].position_m, states[2].states[1].position_m)
        reduced = copied.without_satellite('二体')
        self.assertEqual([s.orbit.object_id for s in reduced.satellites], ['J2', 'copy-1'])
        self.assertEqual(copied.to_scenario().without_object('二体').object_count, 2)
        with self.assertRaises(of.ProjectError):
            copied.with_satellite_copy('copy-1', 'J2', 'duplicate ID')
        with self.assertRaises(of.ProjectError):
            copied.renamed_satellite('copy-1', ' ')
        with tempfile.TemporaryDirectory() as directory:
            path = Path(directory) / 'names.orbitforge'
            copied.save(path)
            self.assertEqual(of.load_project(path).satellites[0].name, '观测卫星')

    def test_version_one_fixture_is_still_readable(self):
        path = Path(__file__).resolve().parents[3] / 'tests/integration/fixtures/project-v1.orbitforge'
        project = of.load_project(path)
        self.assertEqual(len(project.satellites), 2)
        self.assertEqual(project.satellites[0].display_name, '')
        self.assertEqual(project.satellites[0].name, project.satellites[0].orbit.object_id)

    def make_project(self):
        project = of.Project('任务 Python', of.Epoch(123456789), of.Epoch(7200123456789))
        project.current = of.Epoch(600123456789)
        for name, kind in [('二体', of.OrbitPropagator.TWO_BODY), ('J2', of.OrbitPropagator.J2_SECULAR)]:
            orbit = of.OrbitInput()
            orbit.object_id, orbit.propagator = name, kind
            orbit.input_frame = of.FrameId.ITRF
            project.add_satellite(orbit)
        return project

    def test_file_round_trip_preserves_inputs_and_computed_states(self):
        project = self.make_project()
        before = project.to_scenario().sample([project.first, project.current, project.last])
        with tempfile.TemporaryDirectory() as directory:
            path = Path(directory) / '任务.orbitforge'
            project.save(path)
            restored = of.load_project(path)
            self.assertEqual(restored.name, project.name)
            self.assertEqual(restored.current.tai_nanoseconds, project.current.tai_nanoseconds)
            self.assertEqual(restored.satellites[1].orbit.propagator, of.OrbitPropagator.J2_SECULAR)
            self.assertEqual(restored.satellites[1].orbit.input_frame, of.FrameId.ITRF)
            after = restored.to_scenario().sample([project.first, project.current, project.last])
            for a, b in zip(before, after):
                self.assertEqual(a.provenance.algorithm, b.provenance.algorithm)
                for x, y in zip(a.states, b.states):
                    self.assertEqual(x.position_m, y.position_m)
                    self.assertEqual(x.velocity_mps, y.velocity_mps)
            second = Path(directory) / 'roundtrip.orbitforge'
            restored.save(second)
            self.assertEqual(path.read_bytes(), second.read_bytes())

    def test_save_failures_leave_original_and_never_accept_corruption(self):
        project = self.make_project()
        with tempfile.TemporaryDirectory() as directory:
            path = Path(directory) / 'project.orbitforge'
            project.save(path)
            original = path.read_bytes()
            project.name = 'Changed'
            with self.assertRaises(of.ProjectError):
                project.save(path)
            self.assertEqual(path.read_bytes(), original)
            project.save(path, overwrite=True)
            self.assertEqual(of.load_project(path).name, 'Changed')
            project.current = of.Epoch(-1)
            with self.assertRaises(of.ProjectError):
                project.save(path, overwrite=True)
            self.assertEqual(of.load_project(path).name, 'Changed')
            bad = bytearray(original)
            bad[-1] ^= 1
            path.write_bytes(bad)
            with self.assertRaises(of.ProjectError):
                of.load_project(path)

    def test_duplicate_insert_is_transactional_and_empty_project_is_valid(self):
        project = self.make_project()
        with self.assertRaises(of.ProjectError):
            project.add_satellite(project.satellites[0].orbit)
        self.assertEqual(len(project.satellites), 2)
        empty = of.Project('Empty', of.Epoch(0), of.Epoch(1))
        with tempfile.TemporaryDirectory() as directory:
            path = Path(directory) / 'empty.orbitforge'
            empty.save(path)
            self.assertEqual(of.load_project(path).to_scenario().object_count, 0)
