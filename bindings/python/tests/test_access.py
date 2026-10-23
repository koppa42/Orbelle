import math
from pathlib import Path
import tempfile
import unittest

# Native value/format tests; public editing is in test_objects.py.
from orbelle import _native as native

EOP = Path(native.__file__).parent / 'data/eop/iers_c04_2000_01_01_2000_01_02/manifest.json'


class AccessTests(unittest.TestCase):
    def inputs(self):
        project = native.Project('Access', native.Epoch(0), native.Epoch(600_000_000_000))
        orbit = native.OrbitInput()
        project.add_satellite(orbit)
        eop = native.load_eop_snapshot(EOP)
        project.earth_orientation = eop.data_info
        station = native.ProjectGroundStation()
        station.object_id = 'station'
        project.add_ground_station(station)
        sensor = native.ProjectSensor()
        sensor.object_id = 'sensor'
        sensor.parent_satellite_id = 'satellite-1'
        sensor.pointing = native.SensorPointing.TARGETED
        sensor.target_station_id = 'station'
        project.add_sensor(sensor)
        return project

    def test_aer_report_shares_session_values_and_is_immutable(self):
        p = self.inputs()
        session = native.AccessSession(p, 'station', 'sensor', EOP)
        d = native.ReportDefinition()
        d.id = d.name = 'aer'
        d.query.kind = native.ReportKind.AER
        d.query.object_ids = ['station', 'sensor']
        d.query.step_nanoseconds = 60_000_000_000
        result = native.generate_report(p, d, EOP)
        self.assertEqual(result.table.row_count, 11)
        for row in result.table.rows:
            sample = session.at(row[2])
            self.assertAlmostEqual(row[4], sample.observation.elevation_rad, places=13)
            self.assertAlmostEqual(row[5], sample.observation.range_m, places=8)
            self.assertEqual(row[7], int(sample.visible))
            self.assertAlmostEqual(row[8], 0, places=13)
        expected = session.at(p.first).observation.range_m
        stations = p.ground_stations
        stations[0].location.longitude_rad = 1
        p.ground_stations = stations
        self.assertEqual(session.at(p.first).observation.range_m, expected)
        with tempfile.TemporaryDirectory() as directory:
            csv = Path(directory) / 'aer.csv'
            result.export_csv(csv)
            self.assertIn('Range (km)', csv.read_text(encoding='utf-8-sig'))
        d.query.kind = native.ReportKind.ACCESS_INTERVALS
        intervals = native.generate_report(self.inputs(), d, EOP)
        self.assertEqual(intervals.table.row_count, len(session.intervals(p.first, p.last, d.query.step_nanoseconds)))
        with self.assertRaises(ValueError):
            d.query.object_ids = ['sensor']
            native.generate_report(p, d, EOP)

    def test_geometry_and_sensor_reference_validation(self):
        state = native.CartesianState(native.Epoch(0), native.OriginId.EARTH, native.FrameId.ITRF,
                                  [6379137, 0, 0], [2, 0, 0])
        observation = native.observe_ground_target(native.GroundStationLocation(), state)
        self.assertIsNone(observation.azimuth_rad)
        self.assertEqual(observation.range_rate_mps, 2)
        self.assertAlmostEqual(observation.elevation_rad, math.pi / 2)
        p = self.inputs()
        with self.assertRaises(native.ProjectError):
            p.without_ground_station('station')
        self.assertEqual(p.without_satellite('satellite-1').sensors, [])
        self.assertEqual(len(p.with_satellite_copy('satellite-1', 'copy', 'Copy').sensors), 2)
        old_hash = native.report_input_sha256(p)
        sensors = p.sensors
        sensors[0].half_angle_rad = .1
        p.sensors = sensors
        self.assertNotEqual(native.report_input_sha256(p), old_hash)

    def test_sensor_and_station_constraints_roundtrip(self):
        p = self.inputs()
        stations = p.ground_stations
        stations[0].minimum_elevation_rad = 0.2
        p.ground_stations = stations
        with tempfile.TemporaryDirectory() as directory:
            path = Path(directory) / 'sensors.orbelle'
            p.save(path)
            restored = native.load_project(path)
            self.assertEqual(restored.sensors[0].target_station_id, 'station')
            self.assertEqual(restored.sensors[0].parent_satellite_id, 'satellite-1')
            self.assertEqual(restored.sensors[0].pointing, native.SensorPointing.TARGETED)
            self.assertEqual(restored.sensors[0].half_angle_rad, p.sensors[0].half_angle_rad)
            self.assertEqual(restored.ground_stations[0].minimum_elevation_rad, 0.2)
            raw = path.read_bytes()
            restored.save(path, overwrite=True)
            self.assertEqual(path.read_bytes(), raw)


if __name__ == '__main__':
    unittest.main()
