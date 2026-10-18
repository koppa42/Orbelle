"""Ordered satellite-pair reports, frozen resources, formatting and failure boundaries."""
import csv
import io
from pathlib import Path
import tempfile
import unittest
import orbitforge as of
from orbitforge import units as u
EOP=Path(__file__).resolve().parents[3]/"data/samples/eop/iers_c04_2000_01_01_2000_01_02/manifest.json"

def scene():
    p=of.Project("Relative motion",of.Epoch(0),of.Epoch(600*10**9))
    a=p.objects.satellite("Primary",orbit=of.orbits.TwoBody(initial=of.orbits.Cartesian(p.first,(7e6,0,0),(0,7500,100))))
    b=p.objects.satellite("Target",orbit=of.orbits.TwoBody(initial=of.orbits.Cartesian(p.first,(7.5e6,0,0),(0,-7000,1000))))
    return p,a,b,of.DataSnapshot.load(eop_manifest=EOP)

class IntersatelliteTests(unittest.TestCase):
    def test_values_order_statistics_templates_csv_and_snapshot(self):
        p,a,b,data=scene(); analysis=p.prepare(data=data)
        progress=[]
        r=analysis.report(of.ReportKind.RELATIVE_MOTION,objects=[a,b],step=u.s(60),progress=lambda n,t:progress.append((n,t)))
        self.assertEqual(r.table.row_count,11)
        self.assertEqual(r.definition.query.object_ids,[a.id,b.id])
        self.assertEqual(r.table.rows[0][3],500000)
        self.assertEqual(r.table.rows[0][4],0)
        self.assertAlmostEqual(sum(v*v for v in r.table.rows[0][6:9])**.5,500000)
        self.assertEqual(r.table.rows[0][9],1)
        self.assertEqual(progress[-1],(11,11)); self.assertEqual(progress,sorted(progress))
        stats=r.intersatellite_statistics
        self.assertEqual(stats.minimum_range.value,500000)
        self.assertGreater(stats.maximum_range.value,500000)
        self.assertGreater(stats.visibility.count,0)
        self.assertEqual(len(stats.windows),stats.visibility.count)
        with self.assertRaises(AttributeError): stats.minimum_range=None
        fmt=of.reports.Template([of.reports.Column("relative.range",unit="m",title="Distance")])
        formatted=fmt.apply(r)
        self.assertEqual(formatted.table.row_count,r.table.row_count)
        self.assertEqual(formatted.column_unit(0),"m")
        self.assertEqual(formatted.intersatellite_statistics.minimum_range.value,stats.minimum_range.value)
        reverse=analysis.report(of.ReportKind.RELATIVE_MOTION,objects=[b,a],step=u.s(60))
        self.assertAlmostEqual(reverse.table.rows[-1][3],r.table.rows[-1][3])
        self.assertAlmostEqual(reverse.table.rows[-1][4],r.table.rows[-1][4])
        self.assertNotEqual(reverse.table.rows[-1][6:9],r.table.rows[-1][6:9])
        passes=analysis.report(of.ReportKind.INTERSATELLITE_ACCESS,objects=[a,b],step=u.s(60))
        self.assertEqual(passes.table.row_count,stats.visibility.count)
        with tempfile.TemporaryDirectory() as folder:
            path=Path(folder)/"relative.csv"; formatted.export_csv(path)
            rows=list(csv.reader(io.StringIO(path.read_text(encoding="utf-8-sig"))))
            self.assertEqual(rows[0],["Distance (m)"])
            self.assertEqual(float(rows[1][0]),500000)
            saved=Path(folder)/"relative.orbitforge"; p.save(saved,analysis=analysis)
            reopened=of.Project.open(saved,data=data)
            repeated=reopened.prepare(data=data).report(of.ReportKind.RELATIVE_MOTION,
                objects=[reopened.objects[a.id],reopened.objects[b.id]],step=u.s(60))
            self.assertEqual(repeated.table.rows[0][3:],r.table.rows[0][3:])
        a.name="Renamed"
        repeated=analysis.report(of.ReportKind.RELATIVE_MOTION,objects=[a,b],step=u.s(60))
        self.assertEqual(repeated.table.rows[0][1],"Primary → Target")
        b.orbit=of.orbits.TwoBody(initial=of.orbits.Cartesian(p.first,(7.6e6,0,0),(0,-7000,1000)))
        frozen=analysis.report(of.ReportKind.RELATIVE_MOTION,objects=[a,b],step=u.s(60))
        self.assertEqual(frozen.table.rows[0][3:],r.table.rows[0][3:])
        fresh=p.prepare(data=data).report(of.ReportKind.RELATIVE_MOTION,objects=[a,b],step=u.s(60))
        self.assertEqual(fresh.table.rows[0][3],600000)

    def test_pair_rejection_coincidence_instant_and_cancel(self):
        p,a,b,data=scene(); analysis=p.prepare(data=data)
        for objects in (of.reports.ALL,[a],[a,a]):
            with self.assertRaises((ValueError,TypeError)):
                analysis.report(of.ReportKind.RELATIVE_MOTION,objects=objects,step=u.s(60))
        with self.assertRaises(of.AnalysisCancelled):
            analysis.report(of.ReportKind.RELATIVE_MOTION,objects=[a,b],step=u.s(60),cancelled=lambda:True)
        with self.assertRaises(ValueError):
            analysis.report(of.ReportKind.RELATIVE_MOTION,objects=[a,b],step=u.s(1),boundary_tolerance=u.s(2))
        bare,ba,bb,_=scene()
        with self.assertRaises(ValueError):
            bare.prepare().report(of.ReportKind.RELATIVE_MOTION,objects=[ba,bb],step=u.s(60))
        instant=analysis.report(of.ReportKind.RELATIVE_MOTION,objects=[a,b],step=u.s(60),first=p.first,last=p.first)
        self.assertEqual(instant.table.row_count,1); self.assertEqual(instant.intersatellite_statistics.visibility.count,0)
        b.orbit=a.orbit
        same=p.prepare(data=data).report(of.ReportKind.RELATIVE_MOTION,objects=[a,b],step=u.s(60),first=p.first,last=p.first)
        self.assertEqual(same.table.rows[0][3],0); self.assertIsNone(same.table.rows[0][4])
        self.assertEqual(same.table.rows[0][6:9],[0,0,0])
