"""Sun/Moon configuration, immutable ephemerides and explicit update consent."""
import json
import shutil
import tempfile
import unittest
from pathlib import Path

import orbitforge as of
from orbitforge import units as u

SAMPLE = Path(__file__).resolve().parents[3] / "data/samples/spice/de440s_2016_leap"


class ThirdBodyTests(unittest.TestCase):
    def test_easy_settings_validate_normalize_and_roundtrip(self):
        initial = of.orbits.Cartesian(of.Epoch(0), (7e6, 0, 0), (0, 6200, 5300))
        orbit = of.orbits.Hpop(initial=initial, gravity_degree=0,
                              third_bodies=["moon", "sun"], sun_mu=1.327e20)
        self.assertEqual(orbit.hpop.third_bodies, ("sun", "moon"))
        self.assertEqual(orbit.hpop.ephemeris, "de440s")
        project = of.Project("Bodies", of.Epoch(0), of.Epoch(600 * 10**9))
        sat = project.objects.satellite("Sat", orbit=orbit)
        self.assertEqual(sat.orbit.hpop, orbit.hpop)
        with self.assertRaisesRegex(of.AnalysisCapabilityError, "Missing ephemeris"):
            project.prepare()
        for bodies in (("mars",), ("sun", "sun"), "sun"):
            with self.assertRaises(ValueError):
                of.orbits.Hpop(initial=initial, third_bodies=bodies)
        with self.assertRaises(ValueError):
            of.orbits.Hpop(initial=initial, third_bodies=("sun",), sun_mu=0)

    @unittest.skipUnless(of.ephemeris_available(), "requires CSPICE")
    def test_spice_prepare_save_and_changed_data_requires_consent(self):
        with tempfile.TemporaryDirectory() as folder:
            root = Path(folder)
            shutil.copytree(SAMPLE, root / "ephemeris")
            manifest = root / "ephemeris/manifest.json"
            metadata = json.loads(manifest.read_text())
            first = of.Epoch(metadata["first_tai_ns"])
            last = of.Epoch(metadata["first_tai_ns"] + 600 * 10**9)
            data = of.DataSnapshot.builtin_time().with_ephemeris("de440s", manifest, root / "runtime")
            project = of.Project("Third bodies", first, last)
            sat = project.objects.satellite("Sat", orbit=of.orbits.Hpop(
                initial=of.orbits.Keplerian(first, u.km(42164), 0, u.deg(15)),
                gravity_degree=0, third_bodies=("sun", "moon")))
            analysis = project.prepare(data=data)
            before = analysis.states(sat, step=u.s(60)).position.values
            self.assertEqual(len(before), 11)
            path = root / "scenario.orbitforge"
            project.save(path, analysis=analysis)
            restored = of.Project.open(path, data=data)
            self.assertEqual(restored.objects[sat.id].orbit.hpop.third_bodies, ("sun", "moon"))
            metadata["snapshot_id"] += "-changed"
            manifest.write_text(json.dumps(metadata))
            changed = data.with_ephemeris("de440s", manifest, root / "runtime")
            plan = of.Project.plan_open(path, data=changed)
            self.assertEqual(plan.data_changes[0].role, "ephemeris/de440s")
            with self.assertRaises(of.DataChangedError):
                plan.open()
            self.assertIsNotNone(plan.open(accept_data_changes=True))
            self.assertEqual(analysis.states(sat, step=u.s(60)).position.values, before)
            metadata["last_tai_ns"] = last.tai_nanoseconds - 1
            manifest.write_text(json.dumps(metadata))
            short = data.with_ephemeris("de440s", manifest, root / "runtime")
            with self.assertRaises(of.EphemerisCoverageError):
                of.Project.plan_open(path, data=short)
            # Release owned copies before removing their workspace on Windows.
            del analysis, data, changed, short, plan, restored


if __name__ == "__main__":
    unittest.main()
