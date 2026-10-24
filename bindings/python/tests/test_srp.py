"""Spherical SRP units, resource dependence and immutable public API round trips."""
import json
import tempfile
import unittest
from dataclasses import FrozenInstanceError, replace
from pathlib import Path

import orbelle as ob
from orbelle import units as u

SAMPLE = Path(__file__).resolve().parents[3] / "data/samples/spice/de440s_2016_leap"


class SrpTests(unittest.TestCase):
    def test_settings_are_validated_frozen_and_use_si(self):
        settings = ob.orbits.SrpSettings(enabled=True, cr=1.7, shadow_model="none")
        with self.assertRaises(FrozenInstanceError):
            settings.cr = 2
        for kwargs in ({"enabled": 1}, {"earth_shadow": 0}, {"moon_shadow": "true"}, {"boundary_mitigation": 1},
                       {"shadow_model": "wrong"}, {"sun_position_method": "wrong"}, {"cr": 101}, {"cr": float("nan")},
                       {"area_mass_ratio_m2_per_kg": -1}, {"atmospheric_altitude_m": -1},
                       {"pressure_at_reference_pa": 0}, {"reference_distance_m": 0}):
            with self.assertRaises((TypeError, ValueError)):
                ob.orbits.SrpSettings(**kwargs)
        native = settings._native()
        self.assertEqual(native.shadow_model, ob.SrpShadowModel.NONE)
        self.assertEqual(native.sun_position_method, ob.SrpSunPositionMethod.APPARENT_TO_TRUE_CB)
        for method in ("true", "apparent_to_true_cb", "apparent"):
            draft = replace(settings, sun_position_method=method)
            self.assertEqual(ob.orbits.SrpSettings._read(draft._native()), draft)
        result = ob.evaluate_srp((0, 0, 0), (149597870700, 0, 0), native)
        self.assertEqual(result.lighting_ratio, 1)
        self.assertAlmostEqual(result.acceleration_mps2[0], -4.56e-6 * .02 * 1.7, delta=1e-21)
        native.shadow_model = ob.SrpShadowModel.DUAL_CONE
        with self.assertRaises(ValueError):
            ob.evaluate_srp((7e6, 0, 0), (149597870700, 0, 0), native)

    def test_input_only_files_keep_disabled_srp_drafts_and_current_format(self):
        initial = ob.orbits.Cartesian(ob.Epoch(0), (7e6, 0, 0), (0, 6200, 5300))
        srp = ob.orbits.SrpSettings(cr=1.25, area_mass_ratio_m2_per_kg=.03,
                                   atmospheric_altitude_m=23000, moon_shadow=False,
                                   sun_radius_m=696000000, sun_position_method="apparent", boundary_mitigation=True)
        orbit = ob.orbits.Hpop(initial=initial, gravity_degree=0, srp=srp)
        with self.assertRaises(TypeError):
            ob.orbits.Hpop(initial=initial, srp=ob.SrpOptions())
        project = ob.Project("SRP", ob.Epoch(0), ob.Epoch(600 * 10**9))
        sat = project.objects.satellite("Sat", orbit=orbit)
        before = project.prepare().states(sat, step=u.s(60))
        with tempfile.TemporaryDirectory() as folder:
            path = Path(folder) / "srp.orbelle"
            project.save(path)
            self.assertEqual(int.from_bytes(path.read_bytes()[8:12], "little"), 51)
            restored = ob.Project.open(path)
            self.assertEqual(restored.objects[sat.id].orbit.hpop.srp, srp)
        sat.orbit = replace(orbit, hpop=replace(orbit.hpop, srp=replace(srp, cr=1.5)))
        after = project.prepare().states(sat, step=u.s(60))
        self.assertEqual(before.input_sha256, after.input_sha256)
        self.assertEqual(before.position.values, after.position.values)

    def test_additional_bodies_are_explicit_immutable_and_evaluated_as_a_union(self):
        mercury = ob.orbits.SrpEclipsingBody("mercury")
        venus = ob.orbits.SrpEclipsingBody("venus", 6e6)
        self.assertEqual(mercury.radius_m, 2440530)
        values = [mercury, venus]
        settings = ob.orbits.SrpSettings(enabled=True, earth_shadow=False, moon_shadow=False,
                                         additional_eclipsing_bodies=values)
        values.clear()
        self.assertEqual(settings.additional_eclipsing_bodies, (mercury, venus))
        self.assertEqual(ob.orbits.SrpSettings._read(settings._native()), settings)
        with self.assertRaises(FrozenInstanceError):
            venus.radius_m = 1
        for body, radius in (("sun", 1), ("moon", 1), ("earth", 1), ("mars", 0),
                             ("venus", float("nan")), ("jupiter", float("inf"))):
            with self.assertRaises(ValueError):
                ob.orbits.SrpEclipsingBody(body, radius)
        with self.assertRaises(ValueError):
            replace(settings, additional_eclipsing_bodies=(mercury, mercury))
        with self.assertRaises(TypeError):
            replace(settings, additional_eclipsing_bodies=("venus",))
        positions = []
        for body in (ob.ThirdBody.MERCURY, ob.ThirdBody.VENUS):
            value = ob.SrpBodyPosition()
            value.body, value.position_m = body, (1e8, 0, 0)
            positions.append(value)
        result = ob.evaluate_srp((0,0,0), (149597870700,0,0), settings._native(),
                                 additional_positions=positions)
        self.assertEqual(result.lighting_ratio, 0)
        with self.assertRaises(ValueError):
            ob.evaluate_srp((0,0,0), (149597870700,0,0), settings._native())
        positions[1].body = ob.ThirdBody.MERCURY
        with self.assertRaises(ValueError):
            ob.evaluate_srp((0,0,0), (149597870700,0,0), settings._native(),
                            additional_positions=positions)

    @unittest.skipUnless(ob.ephemeris_available(), "requires CSPICE")
    def test_body_assignment_identity_real_centres_coverage_and_saved_drafts(self):
        sample = SAMPLE.parent / "de440s_planets_2016_leap"
        metadata = json.loads((sample / "manifest.json").read_text())
        first = ob.Epoch(metadata["first_tai_ns"] + 600 * 10**9)
        last = ob.Epoch(first.tai_nanoseconds + 60 * 10**9)
        bodies = (ob.orbits.SrpEclipsingBody("mercury"), ob.orbits.SrpEclipsingBody("venus"))
        settings = ob.orbits.SrpSettings(enabled=True, additional_eclipsing_bodies=bodies)
        with tempfile.TemporaryDirectory() as folder:
            root = Path(folder)
            data = ob.DataSnapshot.builtin_time().with_ephemeris("de440s", sample / "manifest.json", root / "runtime")
            project = ob.Project("Planet silhouettes", first, last)
            orbit = ob.orbits.Hpop(initial=ob.orbits.Cartesian(first,(7e6,0,0),(0,6200,5300)),
                                  gravity_degree=0,srp=settings)
            sat = project.objects.satellite("Sat",orbit=orbit)
            analysis = project.prepare(data=data)
            states = analysis.states(sat,step=u.s(30))
            mitigated = replace(settings, boundary_mitigation=True)
            sat.orbit = replace(orbit,hpop=replace(orbit.hpop,srp=mitigated))
            with_boundary = project.prepare(data=data).states(sat,step=u.s(30))
            self.assertNotEqual(states.input_sha256,with_boundary.input_sha256)
            self.assertEqual(analysis.states(sat,step=u.s(30)).position.values,states.position.values)
            sat.orbit = replace(orbit,hpop=replace(orbit.hpop,srp=replace(settings,additional_eclipsing_bodies=bodies[::-1])))
            reversed_states=project.prepare(data=data).states(sat,step=u.s(30))
            self.assertEqual(states.input_sha256,reversed_states.input_sha256)
            self.assertEqual(states.position.values,reversed_states.position.values)
            path=root/"bodies.orbelle"
            project.save(path)
            restored=ob.Project.open(path, data=data)
            self.assertEqual(restored.objects[sat.id].orbit.hpop.srp.additional_eclipsing_bodies,bodies[::-1])
            # The DE file has Jupiter's system barycentre but no 599 body centre.
            sat.orbit=replace(orbit,hpop=replace(orbit.hpop,srp=replace(settings,
                additional_eclipsing_bodies=(ob.orbits.SrpEclipsingBody("jupiter"),))))
            with self.assertRaises((ob.EphemerisCoverageError,ob.AnalysisCapabilityError,ob.PropagationError)):
                project.prepare(data=data)
            self.assertEqual(analysis.states(sat,step=u.s(30)).position.values,states.position.values)
            # Disabled shadows keep unsupported-body drafts, without querying them.
            no_shadow=replace(sat.orbit.hpop.srp,shadow_model="none",boundary_mitigation=True)
            sat.orbit=replace(orbit,hpop=replace(orbit.hpop,srp=no_shadow))
            one=project.prepare(data=data).states(sat,step=u.s(30))
            sat.orbit=replace(orbit,hpop=replace(orbit.hpop,srp=replace(no_shadow,additional_eclipsing_bodies=(),boundary_mitigation=False)))
            two=project.prepare(data=data).states(sat,step=u.s(30))
            self.assertEqual(one.input_sha256,two.input_sha256)
            self.assertEqual(one.position.values,two.position.values)
            del data, analysis, restored

    @unittest.skipUnless(ob.ephemeris_available(), "requires CSPICE")
    def test_srp_only_prepare_report_save_reopen_and_immutable_analysis(self):
        metadata = json.loads((SAMPLE / "manifest.json").read_text())
        first = ob.Epoch(metadata["first_tai_ns"] + 600 * 10**9)
        last = ob.Epoch(first.tai_nanoseconds + 600 * 10**9)
        with tempfile.TemporaryDirectory() as folder:
            root = Path(folder)
            data = ob.DataSnapshot.builtin_time().with_ephemeris(
                "de440s", SAMPLE / "manifest.json", root / "runtime")
            project = ob.Project("SRP only", first, last)
            orbit = ob.orbits.Hpop(initial=ob.orbits.Cartesian(first, (7e6, 0, 0), (0, 6200, 5300)),
                                  gravity_degree=0, srp=ob.orbits.SrpSettings(enabled=True, cr=1.7))
            sat = project.objects.satellite("Sat", orbit=orbit)
            with self.assertRaisesRegex(ob.AnalysisCapabilityError, "Missing ephemeris"):
                project.prepare()
            analysis = project.prepare(data=data)
            before = analysis.states(sat, step=u.s(60))
            report = analysis.report(ob.ReportKind.CARTESIAN_STATE, objects=sat, step=u.s(60),
                                     units=ob.ReportUnits.SI, time_scale=ob.ReportTimeScale.TAI)
            self.assertEqual(len(report.table.rows), 11)
            path = root / "srp.orbelle"
            project.save(path, analysis=analysis)
            restored = ob.Project.open(path, data=data)
            self.assertEqual(restored.objects[sat.id].orbit.hpop.srp, orbit.hpop.srp)
            self.assertEqual(restored.prepare(data=data).states(restored.objects[sat.id], step=u.s(60))
                             .position.values, before.position.values)
            identities, endpoints = set(), set()
            for method in ("true", "apparent_to_true_cb", "apparent"):
                with self.subTest(method=method):
                    selected = replace(orbit, hpop=replace(orbit.hpop, srp=replace(
                        orbit.hpop.srp, sun_position_method=method)))
                    sat.orbit = selected
                    result = project.prepare(data=data)
                    states = result.states(sat, step=u.s(60))
                    identities.add(states.input_sha256)
                    endpoints.add(tuple(states.position.values[-1]))
                    method_path = root / f"{method}.orbelle"
                    project.save(method_path, analysis=result)
                    opened = ob.Project.open(method_path, data=data)
                    self.assertEqual(opened.objects[sat.id].orbit.hpop.srp, selected.hpop.srp)
                    self.assertEqual(opened.prepare(data=data).states(
                        opened.objects[sat.id], step=u.s(60)).position.values, states.position.values)
                    self.assertEqual(analysis.states(sat, step=u.s(60)).position.values,
                                     before.position.values)
            self.assertEqual(len(identities), 3)
            self.assertEqual(len(endpoints), 3)
            sat.orbit = replace(orbit, hpop=replace(orbit.hpop, srp=replace(orbit.hpop.srp, cr=3.4)))
            doubled = project.prepare(data=data).states(sat, step=u.s(60))
            self.assertNotEqual(doubled.input_sha256, before.input_sha256)
            self.assertNotEqual(doubled.position.values[-1], before.position.values[-1])
            self.assertEqual(analysis.states(sat, step=u.s(60)).position.values, before.position.values)
            sat.orbit = replace(orbit, hpop=replace(orbit.hpop, srp=replace(orbit.hpop.srp, enabled=False)))
            self.assertNotEqual(project.prepare().states(sat, step=u.s(60)).position.values[-1],
                                before.position.values[-1])
            del analysis, data, restored, opened, result

    @unittest.skipUnless(ob.ephemeris_available(), "requires CSPICE")
    def test_received_sun_observer_velocity_and_policy_coverage(self):
        metadata = json.loads((SAMPLE / "manifest.json").read_text())
        start = ob.Epoch(metadata["first_tai_ns"])
        epoch = ob.Epoch(start.tai_nanoseconds + 600 * 10**9)
        with tempfile.TemporaryDirectory() as folder:
            provider = ob.load_spice_snapshot(SAMPLE / "manifest.json", Path(folder) / "runtime")
            provider.validate_spacecraft_apparent_sun_interval(epoch, epoch)
            centre = provider.apparent_sun_from_spacecraft(epoch, (0, 0, 0), (0, 0, 0))
            earth = provider.apparent_sun_from_earth(epoch)
            for a, b in zip(centre, earth):
                self.assertAlmostEqual(a, b, delta=1e-3)
            moving = provider.apparent_sun_from_spacecraft(epoch, (7e6, 0, 0), (0, 7500, 0))
            fixed = provider.apparent_sun_from_spacecraft(epoch, (7e6, 0, 0), (0, 0, 0))
            self.assertGreater(sum((a-b)**2 for a, b in zip(moving, fixed))**.5, 1e6)
            with self.assertRaises(ob.EphemerisCoverageError):
                provider.apparent_sun_from_spacecraft(start, (7e6, 0, 0), (0, 7500, 0))
            with self.assertRaises(ob.EphemerisCoverageError):
                provider.apparent_sun_from_spacecraft(epoch, (2e15, 0, 0), (0, 0, 0))
            with self.assertRaises(ValueError):
                provider.apparent_sun_from_spacecraft(epoch, (7e6, 0, 0), (0, float("nan"), 0))
            self.assertEqual(provider.apparent_sun_from_spacecraft(epoch, (0, 0, 0), (0, 0, 0)), centre)
            del provider


if __name__ == "__main__":
    unittest.main()
