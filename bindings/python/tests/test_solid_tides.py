"""Solid tide modes use shared propagation, explicit inputs and immutable snapshots."""
import json
import tempfile
import unittest
from dataclasses import FrozenInstanceError, replace
from pathlib import Path
import orbelle as ob
from orbelle import units as u

ROOT = Path(__file__).resolve().parents[3]
EOP = ROOT / "data/samples/eop/iers_c04_2016_12_28_2017_01_04/manifest.json"
SPICE = ROOT / "data/samples/spice/de440s_2016_leap/manifest.json"


class SolidTideTests(unittest.TestCase):
    def test_modes_are_frozen_validated_and_roundtrip_native(self):
        settings = ob.orbits.SolidTideSettings()
        with self.assertRaises(FrozenInstanceError):
            settings.mode = "full"
        for kwargs in ({"mode": "unknown"}, {"truncate_to_gravity": 1}, {"include_time_dependent_terms": 1}):
            with self.assertRaises((TypeError, ValueError)):
                ob.orbits.SolidTideSettings(**kwargs)
        for mode in ("none", "permanent", "full"):
            value = ob.orbits.SolidTideSettings(mode, False, True)
            self.assertEqual(ob.orbits.SolidTideSettings._read(value._native()), value)
        self.assertEqual(settings._native().mode, ob.SolidTideMode.NONE)
        with self.assertRaises(TypeError):
            ob.orbits.HpopSettings(solid_tides=ob.SolidTideOptions())

    def test_truncated_point_mass_drafts_survive_files_without_force_inputs_or_identity_changes(self):
        first, last = ob.Epoch(0), ob.Epoch(600 * 10**9)
        project = ob.Project("Tide draft", first, last)
        orbit = ob.orbits.Hpop(initial=ob.orbits.Cartesian(first, (7e6, 0, 0), (0, 6200, 5300)),
                              gravity_degree=0)
        sat = project.objects.satellite("Sat", orbit=orbit)
        before = project.prepare().states(sat, step=u.s(60))
        for mode in ("permanent", "full"):
            tides = ob.orbits.SolidTideSettings(mode, True, True)
            sat.orbit = replace(orbit, hpop=replace(orbit.hpop, solid_tides=tides))
            after = project.prepare().states(sat, step=u.s(60))
            self.assertEqual(before.input_sha256, after.input_sha256)
            self.assertEqual(before.position.values, after.position.values)
            with tempfile.TemporaryDirectory() as folder:
                path = Path(folder) / "tides.orbelle"
                project.save(path)
                restored = ob.Project.open(path)
                self.assertEqual(restored.objects[sat.id].orbit.hpop.solid_tides, tides)

    @unittest.skipUnless(ob.time_frames_available(), "requires ERFA")
    def test_inactive_truncation_drafts_do_not_invalidate_an_analysis(self):
        data = ob.DataSnapshot.load(eop_manifest=ROOT / "data/samples/eop/iers_c04_2000_01_01_2000_01_02/manifest.json")
        first, last = ob.Epoch(0), ob.Epoch(600 * 10**9)
        project = ob.Project("Permanent tides", first, last)
        orbit = ob.orbits.Hpop(initial=ob.orbits.Cartesian(first, (7e6, 0, 0), (0, 6200, 5300)), gravity_degree=2)
        sat = project.objects.satellite("Sat", orbit=orbit)
        for mode in ("none", "permanent"):
            sat.orbit = replace(orbit, hpop=replace(orbit.hpop, solid_tides=ob.orbits.SolidTideSettings(mode, True)))
            before = project.prepare(data=data)
            states = before.states(sat, step=u.s(60))
            sat.orbit = replace(orbit, hpop=replace(orbit.hpop, solid_tides=ob.orbits.SolidTideSettings(mode, False, True)))
            after = project.prepare(data=data).states(sat, step=u.s(60))
            self.assertEqual(states.input_sha256, after.input_sha256)
            self.assertEqual(states.position.values, after.position.values)
            with tempfile.TemporaryDirectory() as folder:
                path = Path(folder) / "inactive.orbelle"
                project.save(path, analysis=before)
                reopened = ob.Project.open(path, data=data)
                self.assertFalse(reopened.objects[sat.id].orbit.hpop.solid_tides.truncate_to_gravity)

    @unittest.skipUnless(ob.ephemeris_available() and ob.time_frames_available(), "requires CSPICE/ERFA")
    def test_tides_without_third_bodies_change_force_identity_reports_and_saved_analysis(self):
        metadata = json.loads(SPICE.read_text())
        first = ob.Epoch(metadata["first_tai_ns"] + 3600 * 10**9)
        last = ob.Epoch(first.tai_nanoseconds + 1200 * 10**9)
        with tempfile.TemporaryDirectory() as folder:
            root = Path(folder)
            data = ob.DataSnapshot.load(eop_manifest=EOP).with_ephemeris("de440s", SPICE, root / "runtime")
            without_frames = ob.DataSnapshot.builtin_time().with_ephemeris("de440s", SPICE, root / "runtime")
            without_ephemeris = ob.DataSnapshot.load(eop_manifest=EOP)
            project = ob.Project("Solid tides", first, last)
            orbit = ob.orbits.Hpop(initial=ob.orbits.Cartesian(first, (7e6, 0, 0), (0, 6200, 5300)),
                                  gravity_degree=0, solid_tides=ob.orbits.SolidTideSettings("full", False))
            sat = project.objects.satellite("Sat", orbit=orbit)
            with self.assertRaises((ob.AnalysisCapabilityError, ValueError)):
                project.prepare(data=without_frames)
            with self.assertRaisesRegex(ob.AnalysisCapabilityError, "Missing ephemeris"):
                project.prepare(data=without_ephemeris)
            analysis = project.prepare(data=data)
            before = analysis.states(sat, step=u.s(60))
            report = analysis.report(ob.ReportKind.CARTESIAN_STATE, objects=sat, step=u.s(60),
                                     units=ob.ReportUnits.SI, time_scale=ob.ReportTimeScale.TAI)
            self.assertEqual(tuple(report.table.rows[-1][4:7]), before.position.values[-1])
            identities, endpoints = set(), set()
            for mode, secondary in (("none", False), ("permanent", False), ("full", False), ("full", True)):
                tides = ob.orbits.SolidTideSettings(mode, False, secondary)
                sat.orbit = replace(orbit, hpop=replace(orbit.hpop, solid_tides=tides))
                result = project.prepare(data=data)
                states = result.states(sat, step=u.s(60))
                identities.add(states.input_sha256)
                endpoints.add(tuple(states.position.values[-1]))
                path = root / f"{mode}-{secondary}.orbelle"
                project.save(path, analysis=result)
                reopened = ob.Project.open(path, data=data)
                self.assertEqual(reopened.objects[sat.id].orbit.hpop.solid_tides, tides)
                self.assertEqual(reopened.prepare(data=data).states(reopened.objects[sat.id], step=u.s(60))
                                 .position.values, states.position.values)
                self.assertEqual(analysis.states(sat, step=u.s(60)).position.values, before.position.values)
            self.assertEqual(len(identities), 4)
            self.assertEqual(len(endpoints), 4)
            self.assertEqual(sat.orbit.hpop.third_bodies, ())
            del analysis, data, without_frames, result, reopened


if __name__ == "__main__":
    unittest.main()
