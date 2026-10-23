"""Facility ground coordinates, observer heights, metadata and public object editing."""
import math
from pathlib import Path
import struct
import tempfile
import unittest

import orbelle as of
from orbelle import units as u
from project_format import rewrite_section


class FacilityTests(unittest.TestCase):
    def test_coordinate_conversion_matches_independent_erfa_reference(self):
        value = of.Wgs84Location.from_coordinates(of.GroundPositionType.CARTESIAN,
            (-5599000.5577049947, 233011.67223479203, -3040909.4706983363),
            height_above_ground=u.m(12.5))
        self.assertAlmostEqual(value.longitude.radians, 3.1, delta=1e-14)
        self.assertAlmostEqual(value.latitude.radians, -0.5, delta=1e-14)
        self.assertAlmostEqual(value.ellipsoid_height.metres, 2500, delta=1e-7)
        for type in (of.GroundPositionType.GEODETIC, of.GroundPositionType.GEOCENTRIC,
                     of.GroundPositionType.CARTESIAN, of.GroundPositionType.SPHERICAL,
                     of.GroundPositionType.CYLINDRICAL):
            result = of.Wgs84Location.from_coordinates(type, value.coordinates(type),
                height_above_ground=value.height_above_ground)
            self.assertAlmostEqual(result.ellipsoid_height.metres, 2500, delta=1e-7)
            self.assertEqual(result.height_above_ground, u.m(12.5))

    def test_public_edit_save_copy_and_metadata_preserve_physical_identity(self):
        project = of.Project("Facility", of.Epoch(0), of.Epoch(60_000_000_000))
        station = project.objects.create(of.objects.GroundStation("Antenna",
            of.Wgs84Location(u.deg(0), u.deg(0), u.m(350), u.m(25)),
            minimum_elevation=u.deg(-5), position_type=of.GroundPositionType.CARTESIAN,
            description="Roof antenna\nLine two"))
        physical = of.report_input_sha256(project)
        with project.edit():
            project.objects.update(station, description="Updated", position_type=of.GroundPositionType.SPHERICAL)
        self.assertEqual(of.report_input_sha256(project), physical)
        self.assertEqual(station.description, "Updated")
        self.assertEqual(station.position_type, of.GroundPositionType.SPHERICAL)
        self.assertEqual(station.location.height_above_ground, u.m(25))
        series = project.to_scenario().sample([project.first, project.last])[0]
        self.assertEqual(series.states[0].position_m, [6378512, 0, 0])
        self.assertEqual(series.states[0].velocity_mps, [0, 0, 0])
        definition = of.ReportDefinition()
        definition.id = "facility-position"
        definition.name = "Facility position"
        definition.query.kind = of.ReportKind.GROUND_STATIONS
        definition.query.scope = of.ReportScope.ALL_COMPATIBLE
        definition.format.time_scale = of.ReportTimeScale.TAI
        report = of.generate_report(project, definition)
        row = dict(zip((column.id for column in report.table.columns), report.table.rows[0]))
        self.assertEqual(row["station.height"], 350)
        self.assertEqual(row["station.height_above_ground"], 25)
        self.assertEqual(row["station.observer_height"], 375)
        self.assertEqual([row[f"state.{axis}"] for axis in "xyz"], [6378512, 0, 0])
        with tempfile.TemporaryDirectory() as directory:
            path = Path(directory) / "facility.orbelle"
            project.save(path)
            reopened = of.Project.open(path)
            original = reopened.objects[station.id]
            self.assertEqual(original.description, "Updated")
            self.assertEqual(original.location, station.location)
            duplicate = reopened.objects.duplicate(original, name="Copy")
            self.assertNotEqual(duplicate.id, original.id)
            self.assertEqual(duplicate.description, original.description)
            self.assertEqual(duplicate.position_type, original.position_type)
        with self.assertRaises(of.ProjectError):
            station.location = of.Wgs84Location(u.deg(0), u.deg(0), u.m(99999), u.m(2))
        self.assertEqual(station.location.height_above_ground, u.m(25))
        station.description = ""
        self.assertEqual(station.description, "")

    def test_corrupt_new_station_fields_are_rejected_with_valid_crcs(self):
        project = of.Project("Facility", of.Epoch(0), of.Epoch(1))
        project.objects.ground_station("Station", location=of.Wgs84Location(u.deg(0), u.deg(0)))
        with tempfile.TemporaryDirectory() as directory:
            path = Path(directory) / "facility.orbelle"
            project.save(path)
            original = path.read_bytes()
            def corrupt(body, field, value):
                offset = 4
                for _ in range(2):
                    size = struct.unpack_from("<I", body, offset)[0]
                    offset += 4 + size
                changed = bytearray(body)
                if field == "height":
                    struct.pack_into("<d", changed, offset + 24, value)
                else:
                    struct.pack_into("<I", changed, offset + 40, value)
                return changed
            for field, value in (("height", -1), ("height", math.nan), ("type", 99)):
                path.write_bytes(rewrite_section(original, b"GSTN", lambda b: corrupt(b, field, value)))
                with self.assertRaises(of.ProjectError):
                    of.Project.open(path)


if __name__ == "__main__":
    unittest.main()
