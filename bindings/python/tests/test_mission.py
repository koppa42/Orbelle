from dataclasses import FrozenInstanceError, replace
from pathlib import Path
import math
import tempfile
import unittest

import orbelle as of
from orbelle import units as u
from orbelle.missions import Coast, Impulse


class MissionTests(unittest.TestCase):
    def make(self, propagation="two_body", **kwargs):
        project = of.Project("Transfer", of.Epoch(0), of.Epoch(7_200_000_000_000))
        orbit = of.orbits.Mission(
            initial=of.orbits.Keplerian(of.Epoch(0), u.km(7000), 0, u.deg(28)),
            segments=[Coast(u.ns(600_000_000_001), "Coast"),
                      Impulse((100, -2, 3), name="Raise"), Coast(u.ns(6_599_999_999_999))],
            propagation=propagation, **kwargs)
        return project, project.objects.satellite("Maneuvering", orbit=orbit)

    def test_simple_sequence_burn_and_query_order(self):
        project, satellite = self.make()
        analysis = project.prepare()
        later = analysis.state(satellite, at=project.last)
        burns = analysis.maneuvers(satellite)
        self.assertEqual(len(burns), 1)
        burn = burns.events[0]
        self.assertEqual(burn.name, "Raise")
        self.assertEqual(burn.segment_index, 1)
        self.assertEqual(burn.before.epoch.tai_nanoseconds, 600_000_000_001)
        self.assertEqual(burn.before.position_m, burn.after.position_m)
        self.assertAlmostEqual(math.hypot(*burn.delta_v_gcrf_mps), math.sqrt(10013), delta=1e-10)
        self.assertEqual(analysis.state(satellite, at=burn.before.epoch).velocity_mps, burn.after.velocity_mps)
        self.assertEqual(analysis.state(satellite, at=project.last).position_m, later.position_m)
        self.assertEqual(burns.input_sha256, analysis.states(satellite, step=u.s(60)).input_sha256)
        self.assertEqual(burns.provenance.object_id, satellite.id)
        with self.assertRaises(of.AnalysisCancelled):
            analysis.maneuvers(satellite, cancelled=lambda: True)
        with self.assertRaises(AttributeError):
            burn.name = "Mutated"
        self.assertEqual(len(analysis.maneuvers(satellite)), 1)

    def test_roundtrip_immutable_plans_reports_and_old_analysis(self):
        project, satellite = self.make("hpop", hpop=of.orbits.HpopSettings(gravity_degree=0, gravity_order=0))
        analysis = project.prepare()
        state = analysis.state(satellite, at=project.last)
        with tempfile.TemporaryDirectory() as directory:
            path = Path(directory) / "mission.orbelle"
            project.save(path, analysis=analysis)
            loaded = of.Project.open(path)
            restored = loaded.objects[satellite.id]
            self.assertEqual(restored.orbit.mission, satellite.orbit.mission)
            self.assertEqual(loaded.prepare().state(restored, at=project.last).position_m, state.position_m)
        with self.assertRaises(FrozenInstanceError):
            satellite.orbit.mission.segments[1].name = "Changed"
        plan = satellite.orbit.mission
        satellite.orbit = replace(satellite.orbit, mission=replace(plan,
            segments=(plan.segments[0], Impulse((200, 0, 0)), plan.segments[2])))
        self.assertEqual(analysis.state(satellite, at=project.last).position_m, state.position_m)
        self.assertNotEqual(project.prepare().state(satellite, at=project.last).position_m, state.position_m)
        report = analysis.report(of.ReportKind.CARTESIAN_STATE, objects=satellite, step=u.s(600))
        self.assertEqual(report.table.row_count, 13)

    def test_invalid_inputs_and_coverage_fail_without_mutation(self):
        project, satellite = self.make()
        revision = project.revision
        for call in (lambda: Coast(u.s(0)), lambda: Impulse((math.nan, 0, 0)),
                     lambda: Impulse((1, 2)), lambda: Impulse((0, 0, 0), frame="vvlh"),
                     lambda: of.orbits.Mission(initial=satellite.orbit.initial, segments=[])):
            with self.assertRaises((ValueError, TypeError)):
                call()
        with self.assertRaisesRegex(of.ProjectError, "cover the scenario"):
            satellite.orbit = of.orbits.Mission(initial=satellite.orbit.initial, segments=[Coast(u.s(10))])
        self.assertEqual(project.revision, revision)
        other, other_satellite = self.make()
        with self.assertRaises(of.ProjectError):
            project.prepare().maneuvers(other_satellite)
        satellite.orbit = of.orbits.TwoBody(initial=satellite.orbit.initial)
        self.assertEqual(len(project.prepare().maneuvers(satellite)), 0)

    def test_hpop_coast_eop_dependency_even_in_gcrf(self):
        project, satellite = self.make("hpop")
        with self.assertRaisesRegex(ValueError, "HPOP.*EOP"):
            project.prepare()
        eop = Path(of.__file__).parent / "data/eop/iers_c04_2000_01_01_2000_01_02/manifest.json"
        data = of.DataSnapshot.load(eop_manifest=eop)
        burns = project.prepare(data=data).maneuvers(satellite)
        self.assertEqual(burns.data_sha256, data.data_lock.sha256)


if __name__ == "__main__":
    unittest.main()
