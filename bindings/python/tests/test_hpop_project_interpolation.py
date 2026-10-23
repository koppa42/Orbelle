from dataclasses import replace
from pathlib import Path
import math
import tempfile
import unittest

import orbelle as ob
from orbelle import units as u


class HpopProjectInterpolationTests(unittest.TestCase):
    def make(self):
        project = ob.Project("Interpolated", ob.Epoch(0), ob.Epoch(600*10**9))
        initial = ob.CartesianState(ob.Epoch(0), ob.OriginId.EARTH, ob.FrameId.GCRF,
                                     [7e6, 0, 0], [0, math.sqrt(3.986004415e14/7e6), 0])
        orbit = ob.orbits.Hpop(initial=initial, gravity_degree=0, interpolation="hermite",
                              interpolation_order=5, report_ephemeris_on_fixed_step=True)
        satellite = project.objects.satellite("HPOP", orbit=replace(orbit, step_seconds=5))
        return project, satellite, initial

    def test_settings_save_identity_and_old_analysis_are_consistent(self):
        project, satellite, initial = self.make()
        analysis = project.prepare()
        time = ob.Epoch(123_123_456_789)
        actual = analysis.state(satellite, at=time)
        before = analysis.states(satellite, times=[time])
        self.assertEqual(satellite.orbit.hpop.interpolation_order, 5)
        native = satellite.orbit.hpop._native()
        self.assertEqual(native.interpolation.method, ob.OrbitInterpolationMethod.HERMITE)
        self.assertTrue(native.report_ephemeris_on_fixed_step)
        satellite.orbit = replace(satellite.orbit, step_seconds=10)
        self.assertNotEqual(project.prepare().states(satellite, times=[time]).input_sha256, before.input_sha256)
        satellite.orbit = replace(satellite.orbit, hpop=replace(satellite.orbit.hpop, report_ephemeris_on_fixed_step=False))
        accepted = project.prepare().states(satellite, times=[time])
        satellite.orbit = replace(satellite.orbit, step_seconds=5)
        self.assertEqual(project.prepare().states(satellite, times=[time]).input_sha256, accepted.input_sha256)
        self.assertEqual(analysis.state(satellite, at=time).position_m, actual.position_m)
        with tempfile.TemporaryDirectory() as directory:
            path = Path(directory)/"interpolation.orbelle"
            project.save(path)
            reopened = ob.Project.open(path)
            restored = reopened.objects[satellite.id]
            self.assertEqual(restored.orbit.hpop, satellite.orbit.hpop)
            self.assertEqual(reopened.prepare().states(restored, times=[time]).position.values, accepted.position.values)
        if ob.two_body_available():
            truth = ob.make_two_body_session(initial, 3.986004415e14, "cspice").sample([time]).states[0]
            for a, b in zip(actual.position_m, truth.position_m):
                self.assertAlmostEqual(a, b, delta=.001)
            for a, b in zip(actual.velocity_mps, truth.velocity_mps):
                self.assertAlmostEqual(a, b, delta=1e-6)

    def test_first_report_can_cancel_and_retry_without_a_partial_cache(self):
        project, satellite, _ = self.make()
        analysis = project.prepare()
        calls = 0
        def cancel():
            nonlocal calls
            calls += 1
            return calls == 10
        with self.assertRaises(ob.AnalysisCancelled):
            analysis.report(ob.ReportKind.CARTESIAN_STATE, objects=satellite, step=u.s(60), cancelled=cancel)
        self.assertEqual(calls, 10)
        report = analysis.report(ob.ReportKind.CARTESIAN_STATE, objects=satellite, step=u.s(60))
        self.assertEqual(len(report.table.rows), 11)
        current = analysis.state(satellite, at=ob.Epoch(173_123_456_789))
        fresh = project.prepare().state(satellite, at=current.epoch)
        self.assertEqual(current.position_m, fresh.position_m)

    def test_invalid_configuration_fails_before_project_mutation(self):
        project, satellite, _ = self.make()
        revision = project.revision
        for values in ({"interpolation": "unknown"}, {"interpolation_order": 6},
                       {"interpolation_order": 31}, {"interpolation_order": True},
                       {"report_ephemeris_on_fixed_step": 1}):
            with self.subTest(values=values), self.assertRaises((TypeError, ValueError)):
                satellite.orbit = replace(satellite.orbit, hpop=replace(satellite.orbit.hpop, **values))
        self.assertEqual(project.revision, revision)
        valid = replace(satellite.orbit.hpop, interpolation="lagrange", interpolation_order=0)
        self.assertEqual(valid._native().interpolation.order, 0)
