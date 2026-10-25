"""Create a reusable region, then generate its area-weighted coverage report."""
import math
from pathlib import Path
import orbelle as ob
from orbelle import units as u

project = ob.Project("Coverage", ob.Epoch(0), ob.Epoch(3600*10**9))
satellite = project.objects.satellite("LEO", orbit=ob.orbits.TwoBody(
    initial=ob.orbits.Cartesian(project.first, (7e6, 0, 0), (0, 6200, 5300))))
eop = Path(ob.__file__).parent / "data/eop/iers_c04_2000_01_01_2000_01_02/manifest.json"
region = project.objects.region("Worldwide", boundary=ob.regions.Rectangle(
    south=u.deg(-90), north=u.deg(90), west=u.deg(-180), east=u.deg(180)), grid_spacing=u.deg(30))
# For a polygon, use regions.Polygon((regions.Vertex(u.deg(lon), u.deg(lat)), ...)).
analysis = project.prepare(data=ob.DataSnapshot.load(eop_manifest=eop))
grid = ob.CoverageGrid()  # All angles are geodetic radians; ground height is zero.
# Named regions own their boundary and spacing; this grid supplies analysis constraints.
grid.minimum_elevation_rad = math.radians(5)
grid.include_end_gaps = True
result = analysis.report(ob.ReportKind.COVERAGE_GRID, objects=[satellite],
                         region=region, coverage=grid, step=u.s(60), coverage_memory_mib=512)
# For denser grids, increase the per-job budget without changing scientific settings.
stats = result.coverage_statistics
print("Grid points:", stats.point_count)
print("Ever covered area (%):", 100*stats.ever_covered_area_m2/stats.total_area_m2)
print("Mean instantaneous coverage (%):", 100*stats.mean_coverage_fraction)
print("Longest uncovered gap (s):", stats.maximum_gap_seconds)
assert stats.ever_covered_area_m2 > 0
assert stats.total_area_m2 > stats.ever_covered_area_m2
# Each point retains the exact merged windows; rendering is never a calculation input.
assert any(point.windows for point in stats.iter_points())
