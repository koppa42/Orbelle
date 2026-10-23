"""Public lighting reports use captured solar ephemerides, independently of SRP."""
import csv
import io
import json
import tempfile
import unittest
from pathlib import Path

import orbelle as ob
from orbelle import units as u

SAMPLE = Path(__file__).resolve().parents[3] / "data/samples/spice/de440s_2016_leap"

@unittest.skipUnless(ob.ephemeris_available(), "requires CSPICE")
class LightingTests(unittest.TestCase):
    def test_reports_units_statistics_persistence_and_failure_paths(self):
        first = ob.Epoch(json.loads((SAMPLE / "manifest.json").read_text())["first_tai_ns"] + 600 * 10**9)
        last = ob.Epoch(first.tai_nanoseconds + 12000 * 10**9)
        with tempfile.TemporaryDirectory() as folder:
            root = Path(folder)
            data = ob.DataSnapshot.builtin_time().with_ephemeris("de440s", SAMPLE / "manifest.json", root / "runtime")
            project = ob.Project("Lighting", first, last)
            sat = project.objects.satellite("LEO", orbit=ob.orbits.TwoBody(
                initial=ob.orbits.Cartesian(first, (7e6,0,0), (0,7546.053290107542,0))))
            analysis = project.prepare(data=data)
            result = analysis.report(ob.ReportKind.LIGHTING_INTERVALS, objects=sat,
                                     step=u.s(60), boundary_tolerance=u.s(.000001),
                                     event_search_mode=ob.AccessSearchMode.EXTREMUM_ASSISTED,
                                     time_scale=ob.ReportTimeScale.TAI)
            stats = result.lighting_statistics
            self.assertEqual(stats.object_count,1)
            self.assertEqual({r[5] for r in result.table.rows}, {"Sunlight","Penumbra","Umbra"})
            self.assertAlmostEqual(sum(s.total_duration_s for s in (stats.sunlight,stats.penumbra,stats.umbra)),12000,places=8)
            self.assertAlmostEqual(sum(s.fraction for s in (stats.sunlight,stats.penumbra,stats.umbra)),1,places=14)
            self.assertEqual(sum(s.count for s in (stats.sunlight,stats.penumbra,stats.umbra)),result.table.row_count)
            rows=result.table.rows
            self.assertEqual(rows[0][2].tai_nanoseconds,first.tai_nanoseconds)
            self.assertEqual(rows[-1][3].tai_nanoseconds,last.tai_nanoseconds)
            self.assertEqual(rows[0][6],1)
            self.assertEqual(rows[-1][7],1)
            for a,b in zip(rows,rows[1:]):
                self.assertEqual(a[3].tai_nanoseconds,b[2].tai_nanoseconds)
                self.assertNotEqual(a[5],b[5])
            with self.assertRaises(AttributeError):
                stats.sunlight.count=10
            state=analysis.report(ob.ReportKind.LIGHTING_STATE,objects=sat,step=u.s(61),
                                  time_scale=ob.ReportTimeScale.TAI)
            self.assertEqual(state.table.rows[-1][2].tai_nanoseconds,last.tai_nanoseconds)
            self.assertIsNone(state.lighting_statistics)
            self.assertEqual(state.column_unit(3),"%")
            self.assertTrue(all(0 <= r[3] <= 1 for r in state.table.rows))
            si=ob.ReportTemplate()
            si.units=ob.ReportUnits.SI
            si.time_scale=ob.ReportTimeScale.TAI
            self.assertEqual(state.with_template(si).column_unit(3),"1")
            self.assertAlmostEqual(float(state.cell_text(0,3)),100*state.table.rows[0][3],places=5)
            output=root/"lighting.csv"
            result.export_csv(output)
            exported=list(csv.reader(io.StringIO(output.read_text(encoding="utf-8-sig"))))
            self.assertEqual(len(exported),len(rows)+1)
            self.assertEqual(exported[1][5],rows[0][5])
            with self.assertRaises(ValueError):
                analysis.report(ob.ReportKind.LIGHTING_STATE,objects=sat,lighting_ephemeris="missing",step=u.s(60))
            with self.assertRaises(TypeError):
                analysis.report(ob.ReportKind.CARTESIAN_STATE,objects=sat,lighting_ephemeris="de440s")
            with self.assertRaises(TypeError):
                analysis.report(ob.ReportKind.LIGHTING_STATE,objects=sat,boundary_tolerance=u.s(.1))
            with self.assertRaises(ob.AnalysisCancelled):
                analysis.report(ob.ReportKind.LIGHTING_INTERVALS,objects=sat,step=u.s(60),cancelled=lambda: True)
            # Save the resource identity even when the propagator itself needs no Sun.
            project.save(root/"lighting.orbelle",analysis=analysis)
            reopened=ob.Project.open(root/"lighting.orbelle",data=data)
            repeat=reopened.prepare(data=data).report(ob.ReportKind.LIGHTING_INTERVALS,
                objects=reopened.objects[sat.id],step=u.s(60),boundary_tolerance=u.s(.000001),
                time_scale=ob.ReportTimeScale.TAI)
            def values(rows):
                return [[v.tai_nanoseconds if isinstance(v,ob.Epoch) else v for v in r] for r in rows]
            self.assertEqual(values(repeat.table.rows),values(result.table.rows))
            self.assertEqual(repeat.data_sha256,result.data_sha256)
            sat.name="Renamed"
            self.assertEqual(values(analysis.report(ob.ReportKind.LIGHTING_STATE,objects=sat,
                step=u.s(61),time_scale=ob.ReportTimeScale.TAI).table.rows),values(state.table.rows))

            del data, analysis, reopened
