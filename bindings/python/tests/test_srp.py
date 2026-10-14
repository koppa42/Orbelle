"""Spherical SRP units, resource dependence and immutable public API round trips."""
import json
import tempfile
import unittest
from dataclasses import FrozenInstanceError, replace
from pathlib import Path

import orbitforge as of
from orbitforge import units as u

SAMPLE = Path(__file__).resolve().parents[3] / "data/samples/spice/de440s_2016_leap"


class SrpTests(unittest.TestCase):
    def test_settings_are_validated_frozen_and_use_si(self):
        settings = of.orbits.SrpSettings(enabled=True, cr=1.7, shadow_model="none")
        with self.assertRaises(FrozenInstanceError):
            settings.cr = 2
        for kwargs in ({"enabled": 1}, {"earth_shadow": 0}, {"moon_shadow": "true"},
                       {"shadow_model": "wrong"}, {"cr": 101}, {"cr": float("nan")},
                       {"area_mass_ratio_m2_per_kg": -1}, {"atmospheric_altitude_m": -1},
                       {"pressure_at_reference_pa": 0}, {"reference_distance_m": 0}):
            with self.assertRaises((TypeError, ValueError)):
                of.orbits.SrpSettings(**kwargs)
        native = settings._native()
        self.assertEqual(native.shadow_model, of.SrpShadowModel.NONE)
        result = of.evaluate_srp((0, 0, 0), (149597870700, 0, 0), native)
        self.assertEqual(result.lighting_ratio, 1)
        self.assertAlmostEqual(result.acceleration_mps2[0], -4.56e-6 * .02 * 1.7, delta=1e-21)
        native.shadow_model = of.SrpShadowModel.DUAL_CONE
        with self.assertRaises(ValueError):
            of.evaluate_srp((7e6, 0, 0), (149597870700, 0, 0), native)

    def test_input_only_files_keep_disabled_srp_drafts_and_current_format(self):
        initial = of.orbits.Cartesian(of.Epoch(0), (7e6, 0, 0), (0, 6200, 5300))
        srp = of.orbits.SrpSettings(cr=1.25, area_mass_ratio_m2_per_kg=.03,
                                   atmospheric_altitude_m=23000, moon_shadow=False,
                                   sun_radius_m=696000000)
        orbit = of.orbits.Hpop(initial=initial, gravity_degree=0, srp=srp)
        with self.assertRaises(TypeError):
            of.orbits.Hpop(initial=initial, srp=of.SrpOptions())
        project = of.Project("SRP", of.Epoch(0), of.Epoch(600 * 10**9))
        sat = project.objects.satellite("Sat", orbit=orbit)
        before = project.prepare().states(sat, step=u.s(60))
        with tempfile.TemporaryDirectory() as folder:
            path = Path(folder) / "srp.orbitforge"
            project.save(path)
            self.assertEqual(int.from_bytes(path.read_bytes()[8:12], "little"), 18)
            restored = of.Project.open(path)
            self.assertEqual(restored.objects[sat.id].orbit.hpop.srp, srp)
        sat.orbit = replace(orbit, hpop=replace(orbit.hpop, srp=replace(srp, cr=1.5)))
        after = project.prepare().states(sat, step=u.s(60))
        self.assertEqual(before.input_sha256, after.input_sha256)
        self.assertEqual(before.position.values, after.position.values)

    @unittest.skipUnless(of.ephemeris_available(), "requires CSPICE")
    def test_srp_only_prepare_report_save_reopen_and_immutable_analysis(self):
        metadata = json.loads((SAMPLE / "manifest.json").read_text())
        first = of.Epoch(metadata["first_tai_ns"])
        last = of.Epoch(first.tai_nanoseconds + 600 * 10**9)
        with tempfile.TemporaryDirectory() as folder:
            root = Path(folder)
            data = of.DataSnapshot.builtin_time().with_ephemeris(
                "de440s", SAMPLE / "manifest.json", root / "runtime")
            project = of.Project("SRP only", first, last)
            orbit = of.orbits.Hpop(initial=of.orbits.Cartesian(first, (7e6, 0, 0), (0, 6200, 5300)),
                                  gravity_degree=0, srp=of.orbits.SrpSettings(enabled=True, cr=1.7))
            sat = project.objects.satellite("Sat", orbit=orbit)
            with self.assertRaisesRegex(of.AnalysisCapabilityError, "Missing ephemeris"):
                project.prepare()
            analysis = project.prepare(data=data)
            before = analysis.states(sat, step=u.s(60))
            report = analysis.report(of.ReportKind.CARTESIAN_STATE, objects=sat, step=u.s(60),
                                     units=of.ReportUnits.SI, time_scale=of.ReportTimeScale.TAI)
            self.assertEqual(len(report.table.rows), 11)
            path = root / "srp.orbitforge"
            project.save(path, analysis=analysis)
            restored = of.Project.open(path, data=data)
            self.assertEqual(restored.objects[sat.id].orbit.hpop.srp, orbit.hpop.srp)
            self.assertEqual(restored.prepare(data=data).states(restored.objects[sat.id], step=u.s(60))
                             .position.values, before.position.values)
            sat.orbit = replace(orbit, hpop=replace(orbit.hpop, srp=replace(orbit.hpop.srp, cr=3.4)))
            doubled = project.prepare(data=data).states(sat, step=u.s(60))
            self.assertNotEqual(doubled.input_sha256, before.input_sha256)
            self.assertNotEqual(doubled.position.values[-1], before.position.values[-1])
            self.assertEqual(analysis.states(sat, step=u.s(60)).position.values, before.position.values)
            sat.orbit = replace(orbit, hpop=replace(orbit.hpop, srp=replace(orbit.hpop.srp, enabled=False)))
            self.assertNotEqual(project.prepare().states(sat, step=u.s(60)).position.values[-1],
                                before.position.values[-1])
            del analysis, data, restored


if __name__ == "__main__":
    unittest.main()
