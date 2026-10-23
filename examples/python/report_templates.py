"""Query once, reuse display templates, and save their configuration."""
import tempfile
from pathlib import Path

import orbelle as ob
from orbelle import units as u

project = ob.Project("Reports", ob.Epoch(0), ob.Epoch(120_000_000_000))
station = project.objects.ground_station("Equator", location=ob.Wgs84Location(u.deg(0), u.deg(0)))
analysis = project.prepare()
template = ob.reports.Template([
    "time.epoch",
    ob.reports.Column("state.x", unit="km", title="X"),
    ob.reports.Column("state.vx", unit="m/s"),
])
report = analysis.report(ob.ReportKind.CARTESIAN_STATE, objects=station,
                         step=u.s(30), frame=ob.FrameId.ITRF, template=template)
print(report.column_title(1), report.numeric_value(0, 1))

# The full SI data remain captured, even when some columns are hidden.
si = ob.reports.Template(units=ob.ReportUnits.SI).apply(report)
assert si.table.rows[0][4] == 6378137
assert report.input_sha256 == si.input_sha256
summary = analysis.report(ob.ReportKind.OBJECT_SUMMARY, objects=ob.reports.ALL)
assert summary.table.row_count == 1

project.set_reports([report.definition])
with tempfile.TemporaryDirectory() as directory:
    report.export_csv(Path(directory) / "state.csv")
    project.save(Path(directory) / "mission.orbelle")
