from dataclasses import replace
from pathlib import Path
from concurrent.futures import ThreadPoolExecutor
import math
import struct
import tempfile
import unittest
import orbitforge as of
from project_format import read_sections, with_sections

ROOT = Path(__file__).resolve().parents[3]


def settings():
    state = tuple(tuple((10000.0 if i < 3 else 1.0) if i == j else 0.0
                        for j in range(6)) for i in range(6))
    return of.orbits.CovarianceSettings(
        enabled=True, state=state, consider_analysis=True,
        drag=of.orbits.CovarianceConsider(True, 1e-4, (.1, 0, 0, 0, 0, 0)),
        srp=of.orbits.CovarianceConsider(True, 2e-4, (0, -.1, 0, 0, 0, 0)),
        consider_cross_correlation=True, drag_srp_covariance=5e-5)


def reference_project():
    rows = [list(map(float, line.split())) for line in
            (ROOT / "tests/reference/propagators/universal_kepler_stm.txt").read_text().splitlines()
            if line and not line.startswith("#")]
    ref = next(row for row in rows if row[0] == 14)
    end = of.Epoch(round(ref[2] * 1e9))
    project = of.Project("Covariance reference", of.Epoch(min(0, end.tai_nanoseconds)),
                         of.Epoch(max(1, end.tai_nanoseconds)))
    state = of.CartesianState(of.Epoch(0), of.OriginId.EARTH, of.FrameId.GCRF, ref[3:6], ref[6:9])
    sat = project.objects.satellite("Satellite", orbit=of.orbits.Hpop(
        initial=state, gravity_degree=0, gravity_order=0, relative_tolerance=1e-13,
        position_tolerance_m=1e-6, velocity_tolerance_mps=1e-9, covariance=settings()))
    return project, sat, ref, end


class HpopCovarianceProjectTests(unittest.TestCase):
    def test_saved_project_query_and_report_against_independent_orekit(self):
        p, satellite, ref, end = reference_project()
        with tempfile.TemporaryDirectory() as directory:
            path = Path(directory) / "covariance.orbitforge"
            p.save(path)
            restored = of.Project.open(path)
            satellite = restored.objects[satellite.id]
            self.assertEqual(satellite.orbit.covariance, settings())
            a = restored.prepare()
            result = a.covariance(satellite, at=end)
            phi = [ref[15 + 6 * i:21 + 6 * i] for i in range(6)]
            initial = settings()
            expected = [[sum(phi[i][k] * initial.state[k][k] * phi[j][k]
                             for k in range(6)) for j in range(6)] for i in range(6)]
            largest = 0.0
            for i in range(6):
                for j in range(6):
                    error = abs(result.covariance.state[i][j] - expected[i][j])
                    error /= math.sqrt(expected[i][i]) * math.sqrt(expected[j][j])
                    largest = max(largest, error)
                    self.assertLess(error, 3e-9)
                cross = sum(initial.drag.state_cross_covariance[k] * phi[i][k] for k in range(6))
                self.assertAlmostEqual(result.covariance.drag.state_cross_covariance[i],
                                       cross, delta=1e-7)
            print("Project covariance vs Orekit normalized max error:", largest)
            report = a.report(of.ReportKind.STATE_COVARIANCE, objects=satellite,
                              step=of.units.s(60), first=end, last=end)
            self.assertEqual(len(report.table.rows), 1)
            self.assertEqual(report.table.rows[0][4], result.covariance.state[0][0])
            self.assertEqual(report.numeric_value(0, 4), result.covariance.state[0][0] / 1e6)
            output = Path(directory) / "result.csv"
            report.export_csv(output)
            self.assertIn("Cov(x,x)", output.read_text(encoding="utf-8-sig"))

    def test_snapshot_concurrency_cancellation_and_disabled_queries(self):
        p, satellite, _, end = reference_project()
        analysis = p.prepare()
        expected = analysis.covariance(satellite, at=end).covariance.state
        satellite.orbit = replace(satellite.orbit, covariance=replace(settings(), enabled=False))
        self.assertEqual(analysis.covariance(satellite, at=end).covariance.state, expected)
        with self.assertRaises(of.AnalysisCapabilityError):
            p.prepare().covariance(satellite, at=end)
        with ThreadPoolExecutor(max_workers=2) as pool:
            results = list(pool.map(lambda _: analysis.covariance(satellite, at=end), range(2)))
        self.assertTrue(all(r.covariance.state == expected for r in results))
        with self.assertRaises(of.AnalysisCancelled):
            analysis.covariance(satellite, at=end, cancelled=lambda: True)
        with self.assertRaises(TypeError):
            analysis.covariance(satellite, at=end, cancelled=lambda: "yes")

    def test_current_fixture_drafts_and_integrity(self):
        raw = (ROOT / "tests/integration/fixtures/project-current.orbitforge").read_bytes()
        with tempfile.TemporaryDirectory() as directory:
            path = Path(directory) / "draft.orbitforge"
            path.write_bytes(raw)
            p = of.Project.open(path)
            s = p.objects["satellite-b"].orbit.covariance
            self.assertFalse(s.enabled)
            self.assertEqual((s.frame, s.velocity), ("lvlh", "rotating"))
            self.assertEqual((s.gravity_degree, s.gravity_order), (12, 7))
            self.assertTrue(s.independent_gravity)
            self.assertEqual(s.drag.state_cross_covariance[:3], (.01, .02, .03))
            p.save(path, overwrite=True)
            self.assertEqual(path.read_bytes(), raw)
            for field_offset, payload in ((0, b"\x02"), (1, struct.pack("<I", 99)),
                                          (9, struct.pack("<d", float("nan")))):
                sections = []
                for tag, version, body in read_sections(raw):
                    if tag == b"OBJS":
                        value = bytearray(body)
                        value[len(body)-438+field_offset:len(body)-438+field_offset+len(payload)] = payload
                        body = bytes(value)
                    sections.append((tag, version, body))
                path.write_bytes(with_sections(raw, sections))
                with self.assertRaises(of.ProjectError):
                    of.Project.open(path)

    def test_immutable_configuration_validates_full_joint_matrix(self):
        with self.assertRaises(ValueError):
            of.orbits.CovarianceSettings(enabled=True)
        with self.assertRaises(ValueError):
            replace(settings(), drag=of.orbits.CovarianceConsider(True, 1e-4, (100, 0, 0, 0, 0, 0)))
        with self.assertRaises(ValueError):
            replace(settings(), state=((1.0,),))
        with self.assertRaises(TypeError):
            replace(settings(), enabled=1)
        disabled = replace(settings(), enabled=False, consider_analysis=False,
                           drag=of.orbits.CovarianceConsider(True, 0, (100, 0, 0, 0, 0, 0)))
        self.assertEqual(of.orbits.CovarianceSettings._read(disabled._native()), disabled)
