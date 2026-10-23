"""Lighting intervals and illumination from an explicit DE440s snapshot.

Run with the sample SPICE manifest and a temporary kernel workspace:
  python lighting_analysis.py data/samples/spice/de440s_2016_leap/manifest.json out/spice
"""
import json
from pathlib import Path
import sys
import orbelle as ob
from orbelle import units as u

manifest, workspace = map(Path, sys.argv[1:3])
first = ob.Epoch(json.loads(manifest.read_text())["first_tai_ns"] + 600 * 10**9)
last = ob.Epoch(first.tai_nanoseconds + 12000 * 10**9)
data = ob.DataSnapshot.builtin_time().with_ephemeris("de440s", manifest, workspace)
project = ob.Project("Lighting analysis", first, last)
satellite = project.objects.satellite("LEO", orbit=ob.orbits.TwoBody(
    initial=ob.orbits.Cartesian(first, (7e6, 0, 0), (0, 7546.053290107542, 0))))
analysis = project.prepare(data=data)
intervals = analysis.report(ob.ReportKind.LIGHTING_INTERVALS, objects=satellite,
                            step=u.s(60), boundary_tolerance=u.s(.001))
illumination = analysis.report(ob.ReportKind.LIGHTING_STATE, objects=satellite, step=u.s(10))
for label, stats in (("Sunlight", intervals.lighting_statistics.sunlight),
                     ("Penumbra", intervals.lighting_statistics.penumbra),
                     ("Umbra", intervals.lighting_statistics.umbra)):
    print(f"{label}: {stats.count} intervals, {stats.total_duration_s:.6f} s, {100*stats.fraction:.3f}%")
print(f"Illumination: {illumination.table.row_count} samples")
# intervals.export_csv("lighting.csv")
# project.save("lighting.orbelle", analysis=analysis)
