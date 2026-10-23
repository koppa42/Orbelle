"""Public orbital event reports share the continuous C++ engine and frozen results."""
import csv
import io
from pathlib import Path
import tempfile
import unittest
import orbelle as ob
from orbelle import units as u

class OrbitalEventsTests(unittest.TestCase):
    def test_events_units_statistics_templates_and_roundtrip(self):
        first, last = ob.Epoch(0), ob.Epoch(12000 * 10**9)
        project = ob.Project("Events", first, last)
        sat = project.objects.satellite("Eccentric", orbit=ob.orbits.TwoBody(
            initial=ob.orbits.Cartesian(first, (7e6, 0, 0), (0, 8000, 2000))))
        analysis = project.prepare()
        result = analysis.report(ob.ReportKind.ORBITAL_EVENTS, objects=sat, step=u.s(60),
            boundary_tolerance=u.s(.000001), event_search_mode=ob.AccessSearchMode.EXTREMUM_ASSISTED)
        stats = result.orbital_event_statistics
        self.assertEqual(stats.object_count, 1)
        self.assertTrue(all(c > 0 for c in stats.counts))
        self.assertEqual(sum(stats.counts), result.table.row_count)
        self.assertEqual(stats.near_circular_objects, 0)
        self.assertEqual(stats.near_equatorial_objects, 0)
        self.assertEqual({row[3] for row in result.table.rows},
                         {"Periapsis", "Apoapsis", "Ascending node", "Descending node"})
        self.assertEqual(result.table.rows[0][2].tai_nanoseconds, 0)
        self.assertEqual(result.column_unit(4), "km")
        self.assertEqual(result.numeric_value(0, 4), result.table.rows[0][4] / 1000)
        with self.assertRaises(AttributeError):
            stats.object_count = 3
        fmt = ob.ReportTemplate()
        fmt.time_scale = ob.ReportTimeScale.TAI
        fmt.units = ob.ReportUnits.SI
        formatted = result.with_template(fmt)
        self.assertEqual(formatted.orbital_event_statistics.counts, stats.counts)
        self.assertEqual(formatted.column_unit(4), "m")
        instant = analysis.report(ob.ReportKind.ORBITAL_EVENTS, objects=sat, step=u.s(60), first=first, last=first)
        self.assertEqual(instant.table.row_count, 0)
        with self.assertRaises(ob.AnalysisCancelled):
            analysis.report(ob.ReportKind.ORBITAL_EVENTS, objects=sat, step=u.s(60), cancelled=lambda: True)
        with self.assertRaises(ValueError):
            analysis.report(ob.ReportKind.ORBITAL_EVENTS, objects=sat, step=u.s(1), boundary_tolerance=u.s(2))
        with tempfile.TemporaryDirectory() as folder:
            output = Path(folder) / "events.csv"
            result.export_csv(output)
            exported = list(csv.reader(io.StringIO(output.read_text(encoding="utf-8-sig"))))
            self.assertEqual(len(exported), result.table.row_count + 1)
            self.assertEqual(exported[1][3], "Periapsis")
            project.save(Path(folder) / "events.orbelle", analysis=analysis)
            reopened = ob.Project.open(Path(folder) / "events.orbelle")
            repeated = reopened.prepare().report(ob.ReportKind.ORBITAL_EVENTS,
                objects=reopened.objects[sat.id], step=u.s(60), boundary_tolerance=u.s(.000001))
            self.assertEqual(repeated.orbital_event_statistics.counts, stats.counts)
            self.assertEqual([r[2].tai_nanoseconds for r in repeated.table.rows],
                             [r[2].tai_nanoseconds for r in result.table.rows])

    def test_circular_equatorial_suppression_is_explicit(self):
        first = ob.Epoch(0)
        project = ob.Project("Circular", first, ob.Epoch(12000 * 10**9))
        sat = project.objects.satellite("Circular", orbit=ob.orbits.TwoBody(
            initial=ob.orbits.Cartesian(first, (7e6, 0, 0), (0, (3.986004418e14 / 7e6) ** .5, 0))))
        result = project.prepare().report(ob.ReportKind.ORBITAL_EVENTS, objects=sat, step=u.s(60))
        self.assertEqual(result.table.row_count, 0)
        self.assertEqual(result.orbital_event_statistics.counts, [0, 0, 0, 0])
        self.assertEqual(result.orbital_event_statistics.near_circular_objects, 1)
        self.assertEqual(result.orbital_event_statistics.near_equatorial_objects, 1)
