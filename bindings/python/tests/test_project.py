from pathlib import Path
import tempfile
import unittest
import orbitforge as of


class ProjectTests(unittest.TestCase):
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
