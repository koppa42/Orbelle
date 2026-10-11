from pathlib import Path
import math
import struct
import tempfile
import unittest
import zlib
import orbitforge as of
from project_fixtures import legacy_objects


class GroundStationTests(unittest.TestCase):
    def make_project(self):
        project = of.Project('地面站任务', of.Epoch(0), of.Epoch(7200000000000))
        station = of.ProjectGroundStation()
        station.object_id = 'station-a'
        station.display_name = '海岸站'
        station.location = of.GroundStationLocation(3.1, -0.5, 2500)
        project.add_ground_station(station)
        return project

    def test_station_only_roundtrip_preserves_fixed_itrf_states_and_independent_values(self):
        project = self.make_project()
        with tempfile.TemporaryDirectory() as directory:
            path = Path(directory) / '地面站.orbitforge'
            project.save(path)
            restored = of.load_project(path)
            self.assertEqual(restored.ground_stations[0].name, '海岸站')
            self.assertEqual(restored.ground_stations[0].location.longitude_rad, 3.1)
            result = restored.to_scenario().sample([project.first, project.last])[0]
            self.assertEqual(result.provenance.object_id, 'station-a')
            self.assertEqual(result.states[0].frame, of.FrameId.ITRF)
            self.assertEqual(result.states[0].velocity_mps, [0, 0, 0])
            self.assertEqual(result.states[0].position_m, result.states[1].position_m)
            for actual, expected in zip(result.states[0].position_m,
                                        [-5599000.5577049947, 233011.67223479203, -3040909.4706983363]):
                self.assertAlmostEqual(actual, expected, delta=1e-7)
            copied = restored.with_ground_station_copy('station-a', 'station-b', '副本')
            renamed = copied.renamed_ground_station('station-b', '北岸站')
            removed = renamed.without_ground_station('station-a')
            self.assertEqual(removed.ground_stations[0].object_id, 'station-b')
            self.assertEqual(removed.ground_stations[0].name, '北岸站')
            self.assertEqual(len(restored.ground_stations), 1)
            revised = removed.with_settings('Shorter', of.Epoch(1), project.last)
            self.assertEqual(revised.ground_stations[0].location.height_m, 2500)

    def test_cross_kind_ids_and_invalid_location_are_rejected_without_mutating_project(self):
        project = self.make_project()
        orbit = of.OrbitInput()
        orbit.object_id = 'station-a'
        with self.assertRaises(of.ProjectError):
            project.add_satellite(orbit)
        self.assertEqual(len(project.satellites), 0)
        orbit.object_id = 'satellite-a'
        project.add_satellite(orbit)
        self.assertEqual(project.to_scenario().object_count, 2)
        with self.assertRaises(of.ProjectError):
            project.with_ground_station_copy('station-a', 'satellite-a', 'Duplicate')
        for longitude, latitude, height in [(math.nan, 0, 0), (0, 2, 0), (4, 0, 0),
                                             (0, 0, math.inf), (0, 0, -12001), (0, 0, 100001)]:
            station = of.ProjectGroundStation()
            station.location = of.GroundStationLocation(longitude, latitude, height)
            with self.assertRaises(of.ProjectError):
                project.add_ground_station(station)
        self.assertEqual(len(project.ground_stations), 1)

    @staticmethod
    def rewrite(raw, change, file_version=9):
        chunks, offset = [], 24
        while offset < len(raw):
            tag, version, size, _ = struct.unpack_from('<IIQI', raw, offset)
            body = raw[offset + 20:offset + 20 + size]
            offset += 20 + size
            if file_version < 9 and tag.to_bytes(4, 'little') == b'OBJS':
                body, version = legacy_objects(body), 2
            if file_version < 8 and tag.to_bytes(4, 'little') == b'STYL':
                continue
            if file_version < 6 and tag.to_bytes(4, 'little') == b'RPTS':
                continue
            if tag.to_bytes(4, 'little') == b'GSTN':
                body = change(body)
            if body is not None:
                chunks.append(struct.pack('<IIQI', tag, version, len(body), zlib.crc32(body)) + body)
        payload = b''.join(chunks)
        return raw[:8] + struct.pack('<IQI', file_version, len(payload), zlib.crc32(payload)) + payload

    def test_station_section_is_required_and_validated_even_with_correct_crcs(self):
        with tempfile.TemporaryDirectory() as directory:
            path = Path(directory) / 'invalid.orbitforge'
            self.make_project().save(path)
            raw = path.read_bytes()
            for mutate in (lambda body: None,
                           lambda body: body[:-8] + struct.pack('<d', math.nan),
                           lambda body: struct.pack('<I', 10001) + body[4:],
                           lambda body: body + b'\x00'):
                path.write_bytes(self.rewrite(raw, mutate))
                with self.assertRaises(of.ProjectError):
                    of.load_project(path)

    def test_v4_project_without_station_section_still_loads_and_upgrades(self):
        with tempfile.TemporaryDirectory() as directory:
            path = Path(directory) / 'legacy.orbitforge'
            empty = of.Project('Legacy', of.Epoch(0), of.Epoch(1))
            empty.save(path)
            raw = path.read_bytes()
            path.write_bytes(self.rewrite(raw, lambda body: None, file_version=4))
            project = of.load_project(path)
            self.assertEqual(project.ground_stations, [])
            project.add_ground_station(self.make_project().ground_stations[0])
            project.save(path, overwrite=True)
            self.assertEqual(struct.unpack_from('<I', path.read_bytes(), 8)[0], 9)
            self.assertEqual(len(of.load_project(path).ground_stations), 1)
