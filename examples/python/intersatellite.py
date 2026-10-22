"""Satellite-pair range, RIC and WGS84 direct-view windows from one prepared snapshot."""
from pathlib import Path
import orbelle as of
from orbelle import units as u

project=of.Project("Satellite pair",of.Epoch(0),of.Epoch(600*10**9))
primary=project.objects.satellite("Primary",orbit=of.orbits.TwoBody(
    initial=of.orbits.Cartesian(project.first,(7e6,0,0),(0,7500,100))))
target=project.objects.satellite("Target",orbit=of.orbits.TwoBody(
    initial=of.orbits.Cartesian(project.first,(7.5e6,0,0),(0,-7000,1000))))
eop=Path(of.__file__).parent/"data/eop/iers_c04_2000_01_01_2000_01_02/manifest.json"
analysis=project.prepare(data=of.DataSnapshot.load(eop_manifest=eop))
relative=analysis.report(of.ReportKind.RELATIVE_MOTION,objects=[primary,target],step=u.s(60))
visibility=analysis.report(of.ReportKind.INTERSATELLITE_ACCESS,objects=[primary,target],step=u.s(60))
print("Closest range (m):",relative.intersatellite_statistics.minimum_range.value)
print("RIC position (m):",relative.table.rows[0][6:9])
print("Visible windows:",visibility.table.row_count)
assert relative.table.row_count==11 and visibility.table.row_count>0
