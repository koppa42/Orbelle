"""Query once, reuse display templates, and save their configuration."""
import tempfile
from pathlib import Path

import orbelle as of
from orbelle import units as u

project = of.Project("Reports", of.Epoch(0), of.Epoch(120_000_000_000))
station = project.objects.ground_station("Equator", location=of.Wgs84Location(u.deg(0), u.deg(0)))
analysis = project.prepare()
template = of.reports.Template([
    "time.epoch",
    of.reports.Column("state.x", unit="km", title="X"),
    of.reports.Column("state.vx", unit="m/s"),
])
report = analysis.report(of.ReportKind.CARTESIAN_STATE, objects=station,
                         step=u.s(30), frame=of.FrameId.ITRF, template=template)
print(report.column_title(1), report.numeric_value(0, 1))

# The full SI data remain captured, even when some columns are hidden.
si = of.reports.Template(units=of.ReportUnits.SI).apply(report)
assert si.table.rows[0][4] == 6378137
assert report.input_sha256 == si.input_sha256
summary = analysis.report(of.ReportKind.OBJECT_SUMMARY, objects=of.reports.ALL)
assert summary.table.row_count == 1

project.set_reports([report.definition])
with tempfile.TemporaryDirectory() as directory:
    report.export_csv(Path(directory) / "state.csv")
    project.save(Path(directory) / "mission.orbelle")
