"""Static file gravity shares immutable providers with propagation and saved projects."""
import tempfile
import unittest
from pathlib import Path
import orbitforge as of
from orbitforge import units as u

EOP = Path(__file__).resolve().parents[3] / "data/samples/eop/iers_c04_2000_01_01_2000_01_02/manifest.json"
GRV = """stk.v.6.0
Model Custom
Degree 21
Order 21
Gm 3.986004415e14
RefDistance 6378136.3
Normalized Yes
IncludesPermTide No
BEGIN Coefficients
2 0 -4.84165371736e-4 0
21 21 1e-8 2e-8
END Coefficients
"""

class GravityTests(unittest.TestCase):
    def test_install_select_prepare_roundtrip_and_update_consent(self):
        with tempfile.TemporaryDirectory() as folder:
            root = Path(folder)
            file = root / "gravity.grv"
            file.write_text(GRV)
            catalog = of.DataCatalog(root / "catalog")
            receipt = catalog.install_gravity(file)
            self.assertEqual(receipt.info.degree, 21)
            self.assertEqual(receipt.selection, "custom")
            self.assertEqual(len(catalog.gravity_models), 1)
            data = of.DataSnapshot.load(eop_manifest=EOP).with_gravity("custom", receipt.file)
            project = of.Project("Field", of.Epoch(0), of.Epoch(600 * 10**9))
            sat = project.objects.satellite("Sat", orbit=of.orbits.Hpop(
                initial=of.orbits.Keplerian(of.Epoch(0), u.km(7000), 0, u.deg(25)),
                gravity_model="custom", gravity_degree=21))
            with self.assertRaisesRegex(of.AnalysisCapabilityError, "Missing gravity"):
                project.prepare(data=of.DataSnapshot.load(eop_manifest=EOP))
            analysis = project.prepare(data=data)
            before = analysis.states(sat, step=u.s(60)).position.values
            project.save(root / "project.orbitforge", analysis=analysis)
            restored = of.Project.open(root / "project.orbitforge", data=data)
            self.assertEqual(restored.objects[sat.id].orbit.hpop.gravity_model, "custom")
            file.write_text(GRV.replace("1e-8 2e-8", "2e-8 4e-8"))
            changed = data.with_gravity("custom", file)
            plan = of.Project.plan_open(root / "project.orbitforge", data=changed)
            self.assertEqual(plan.data_changes[0].role, "gravity/custom")
            self.assertEqual(analysis.states(sat, step=u.s(60)).position.values, before)
            with self.assertRaises(of.DataChangedError):
                plan.open()
            self.assertIsNotNone(plan.open(accept_data_changes=True))
            with self.assertRaises(RuntimeError):
                catalog.install_gravity(file, "egm2008")
            del catalog  # Close SQLite before TemporaryDirectory cleanup on Windows.
