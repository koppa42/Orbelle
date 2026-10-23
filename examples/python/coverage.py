"""Global WGS84 coverage and revisit gaps from the shared prepared Analysis API."""
import math
from pathlib import Path
import orbelle as ob
from orbelle import units as u

project = ob.Project("Coverage", ob.Epoch(0), ob.Epoch(3600*10**9))
satellite = project.objects.satellite("LEO", orbit=ob.orbits.TwoBody(
    initial=ob.orbits.Cartesian(project.first, (7e6, 0, 0), (0, 6200, 5300))))
eop = Path(ob.__file__).parent / "data/eop/iers_c04_2000_01_01_2000_01_02/manifest.json"
analysis = project.prepare(data=ob.DataSnapshot.load(eop_manifest=eop))
grid = ob.CoverageGrid()  # All angles are geodetic radians; ground height is zero.
grid.south_rad, grid.north_rad = -math.pi/2, math.pi/2
grid.west_rad, grid.east_rad = -math.pi, math.pi
grid.resolution_rad = math.radians(30)
grid.minimum_elevation_rad = math.radians(5)
grid.include_end_gaps = True
result = analysis.report(ob.ReportKind.COVERAGE_GRID, objects=[satellite],
                         coverage=grid, step=u.s(60))
stats = result.coverage_statistics
print("Grid points:", len(stats.points))
print("Ever covered area (%):", 100*stats.ever_covered_area_m2/stats.total_area_m2)
print("Mean instantaneous coverage (%):", 100*stats.mean_coverage_fraction)
print("Longest uncovered gap (s):", stats.maximum_gap_seconds)
assert stats.ever_covered_area_m2 > 0
assert stats.total_area_m2 > stats.ever_covered_area_m2
# Each point retains the exact merged windows; rendering is never a calculation input.
assert any(point.windows for point in stats.points)
