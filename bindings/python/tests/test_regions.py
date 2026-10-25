"""Named regions share the object transaction, persistence and coverage pipeline."""
import math
from pathlib import Path
import tempfile
import unittest
import orbelle as ob
from orbelle import units as u
from test_coverage import scene


def triangle():
    return ob.regions.Polygon(tuple(ob.regions.Vertex(u.deg(lon), u.deg(lat))
                                   for lon, lat in ((170, -10), (-170, -10), (180, 10))))


class RegionTests(unittest.TestCase):
    def test_crud_transactions_invalid_input_and_roundtrip(self):
        p = ob.Project("Regions", ob.Epoch(0), ob.Epoch(60 * 10**9))
        region = p.objects.create(ob.objects.Region("Pacific", triangle(), "Observation area"))
        identity = region.id
        self.assertEqual(p.objects.find(type=ob.objects.Region), (region,))
        self.assertEqual(p.objects.read(region).boundary, triangle())
        with p.edit():
            region.name = "Renamed"
            region.description = "Saved note"
            region.grid_spacing = u.deg(2)
        self.assertEqual(region.id, identity)
        copy = p.objects.duplicate(region, name="Copy")
        self.assertNotEqual(copy.id, identity)
        self.assertEqual(copy.boundary, region.boundary)
        self.assertAlmostEqual(copy.grid_spacing.radians, math.radians(2))
        with self.assertRaises(ob.ProjectError):
            region.grid_spacing = u.deg(0)
        with self.assertRaises(TypeError):
            region.grid_spacing = 2
        with self.assertRaises(ValueError):
            with p.edit():
                region.name = "Should roll back"
                region.boundary = ob.regions.Polygon((ob.regions.Vertex(u.deg(0), u.deg(0)),) * 3)
        self.assertEqual(region.name, "Renamed")
        with self.assertRaisesRegex(TypeError, "does not accept orbit"):
            p.objects.update(region, orbit=object())
        with tempfile.TemporaryDirectory() as directory:
            path = Path(directory) / "regions.orbelle"
            p.save(path)
            loaded = ob.Project.open(path)
            self.assertEqual(loaded.objects[identity].boundary, triangle())
            self.assertEqual(loaded.objects[identity].description, "Saved note")
            self.assertAlmostEqual(loaded.objects[identity].grid_spacing.radians, math.radians(2))
        p.objects.remove(copy)
        self.assertEqual(len(p.objects.find(type=ob.objects.Region)), 1)

    def test_named_coverage_snapshot_and_cross_project_scope(self):
        p, sat, data, grid = scene()
        region = p.objects.region("World", boundary=ob.regions.Rectangle(
            u.deg(-90), u.deg(90), u.deg(-180), u.deg(180)), grid_spacing=u.rad(grid.resolution_rad))
        analysis = p.prepare(data=data)
        baseline = analysis.report(ob.ReportKind.COVERAGE_GRID, objects=[sat], coverage=grid, step=u.s(60))
        result = analysis.report(ob.ReportKind.COVERAGE_GRID, objects=[sat], region=region,
                                 coverage=grid, step=u.s(60))
        self.assertEqual(result.table.rows, baseline.table.rows)
        self.assertAlmostEqual(result.coverage_statistics.grid.north_rad, math.pi / 2)
        self.assertIsNotNone(result.coverage_statistics.region)
        # Mutable boundary/grid values returned to callers must not mutate the scientific result.
        displayed_grid = result.coverage_statistics.grid
        displayed_boundary = result.coverage_statistics.region
        displayed_grid.south_rad = .7
        displayed_boundary.south_rad = .8
        self.assertAlmostEqual(result.coverage_statistics.grid.south_rad, -math.pi / 2)
        self.assertAlmostEqual(result.coverage_statistics.region.south_rad, -math.pi / 2)
        region.boundary = triangle()
        self.assertEqual(analysis.report(ob.ReportKind.COVERAGE_GRID, objects=[sat], region=region,
            coverage=grid, step=u.s(60)).table.rows, result.table.rows)
        grid.resolution_rad = math.radians(5)
        region.grid_spacing = u.deg(5)
        polygon = p.prepare(data=data).report(ob.ReportKind.COVERAGE_GRID, objects=[sat],
            region=region, coverage=grid, step=u.s(60))
        self.assertLess(polygon.coverage_statistics.total_area_m2, result.coverage_statistics.total_area_m2)
        self.assertAlmostEqual(polygon.coverage_statistics.grid.resolution_rad, math.radians(5))
        self.assertNotEqual(result.coverage_statistics.grid.resolution_rad,
                            polygon.coverage_statistics.grid.resolution_rad)
        p.objects.remove(region)
        with self.assertRaises(ob.ObjectNotFoundError):
            p.prepare(data=data).report(ob.ReportKind.COVERAGE_GRID, objects=[sat], region=region,
                coverage=grid, step=u.s(60))
        other = ob.Project("Other", p.first, p.last).objects.region("Other area", boundary=triangle())
        with self.assertRaises(ob.ProjectError):
            analysis.report(ob.ReportKind.COVERAGE_GRID, objects=[sat], region=other, coverage=grid)
        with self.assertRaises(TypeError):
            analysis.report(ob.ReportKind.CARTESIAN_STATE, objects=[sat], region=region)
