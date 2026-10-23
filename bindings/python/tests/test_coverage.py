"""Coverage uses immutable Analysis trajectories and area-weighted union results."""
import csv
import math
from pathlib import Path
import tempfile
import unittest
import orbelle as ob
from orbelle import units as u

EOP = Path(__file__).resolve().parents[3] / "data/samples/eop/iers_c04_2000_01_01_2000_01_02/manifest.json"


def scene():
    p = ob.Project("Coverage", ob.Epoch(0), ob.Epoch(600 * 10**9))
    sat = p.objects.satellite("Asset", orbit=ob.orbits.TwoBody(
        initial=ob.orbits.Cartesian(p.first, (7e6, 0, 0), (0, 6200, 5300))))
    data = ob.DataSnapshot.load(eop_manifest=EOP)
    grid = ob.CoverageGrid()
    grid.south_rad, grid.north_rad = -math.pi/2, math.pi/2
    grid.west_rad, grid.east_rad = -math.pi, math.pi
    grid.resolution_rad = math.pi/3
    return p, sat, data, grid


class CoverageTests(unittest.TestCase):
    def test_prepared_snapshot_progress_format_export_and_roundtrip(self):
        p, sat, data, grid = scene()
        analysis = p.prepare(data=data)
        progress = []
        result = analysis.report(ob.ReportKind.COVERAGE_GRID, objects=[sat], step=u.s(60),
                                 coverage=grid, progress=lambda n, total: progress.append((n, total)))
        stats = result.coverage_statistics
        self.assertEqual(result.table.row_count, len(stats.points))
        self.assertGreater(stats.ever_covered_area_m2, 0)
        self.assertLessEqual(stats.ever_covered_area_m2, stats.total_area_m2)
        self.assertEqual(progress, sorted(progress))
        self.assertEqual(progress[-1], (result.table.row_count, result.table.row_count))
        self.assertAlmostEqual(stats.mean_coverage_fraction,
            sum(p.cell.area_m2*p.statistics.covered_seconds/600 for p in stats.points)/stats.total_area_m2)
        with self.assertRaises(AttributeError): stats.total_area_m2 = 0
        formatted = ob.reports.Template([ob.reports.Column("coverage.area", unit="km^2")]).apply(result)
        self.assertAlmostEqual(formatted.numeric_value(0, 0), stats.points[0].cell.area_m2/1e6)
        self.assertEqual(formatted.coverage_statistics.maximum_gap_seconds, stats.maximum_gap_seconds)
        series = analysis.report(ob.ReportKind.COVERAGE_TIME, objects=[sat], step=u.s(60), coverage=grid)
        self.assertEqual(series.table.row_count, 11)
        self.assertAlmostEqual(series.table.rows[-1][5], stats.ever_covered_area_m2)
        with tempfile.TemporaryDirectory() as folder:
            output = Path(folder)/"coverage.csv"
            formatted.export_csv(output)
            with output.open(encoding="utf-8-sig", newline="") as stream:
                rows = list(csv.reader(stream))
            self.assertEqual(len(rows), result.table.row_count+1)
            self.assertAlmostEqual(float(rows[1][0]), stats.points[0].cell.area_m2/1e6)
            saved = Path(folder)/"coverage.orbelle"
            p.save(saved, analysis=analysis)
            reopened = ob.Project.open(saved, data=data)
            again = reopened.prepare(data=data).report(ob.ReportKind.COVERAGE_GRID,
                objects=[reopened.objects[sat.id]], step=u.s(60), coverage=grid)
            self.assertEqual(again.table.rows, result.table.rows)
        sat.orbit = ob.orbits.TwoBody(initial=ob.orbits.Cartesian(p.first, (8e6, 0, 0), (0, 6200, 5300)))
        frozen = analysis.report(ob.ReportKind.COVERAGE_GRID, objects=[sat], step=u.s(60), coverage=grid)
        self.assertEqual(frozen.table.rows, result.table.rows)
        self.assertNotEqual(p.prepare(data=data).report(ob.ReportKind.COVERAGE_GRID,
            objects=[sat], step=u.s(60), coverage=grid).table.rows, result.table.rows)

    def test_explicit_assets_limits_errors_and_cancel(self):
        p, sat, data, grid = scene()
        analysis = p.prepare(data=data)
        for assets in (ob.reports.ALL, [], [sat, sat]):
            with self.assertRaises((ValueError, TypeError)):
                analysis.report(ob.ReportKind.COVERAGE_GRID, objects=assets, step=u.s(60), coverage=grid)
        with self.assertRaises(TypeError):
            analysis.report(ob.ReportKind.CARTESIAN_STATE, objects=[sat], step=u.s(60), coverage=grid)
        with self.assertRaises(ob.AnalysisCancelled):
            analysis.report(ob.ReportKind.COVERAGE_TIME, objects=[sat], step=u.s(60), coverage=grid, cancelled=lambda: True)
        with self.assertRaises(ValueError):
            analysis.report(ob.ReportKind.COVERAGE_GRID, objects=[sat], step=u.s(60), coverage=grid, first=p.first, last=p.first)
        grid.resolution_rad = 1e-12
        with self.assertRaises(ValueError):
            analysis.report(ob.ReportKind.COVERAGE_GRID, objects=[sat], step=u.s(60), coverage=grid)
        bare, asset, _, grid = scene()
        with self.assertRaises(ValueError):
            bare.prepare().report(ob.ReportKind.COVERAGE_GRID, objects=[asset], step=u.s(60), coverage=grid)
