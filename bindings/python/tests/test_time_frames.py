import csv
from pathlib import Path
import unittest

import orbelle as ob


class TimeFrameApiTests(unittest.TestCase):
    def setUp(self):
        self.assertTrue(ob.time_frames_available(), "The standard wheel must include ERFA")
        self.time = ob.make_builtin_time_provider()

    def test_leap_second_preserves_nanoseconds(self):
        leap = self.time.from_utc(ob.UtcDateTime(2016, 12, 31, 23, 59, 60, 123456789))
        midnight = self.time.from_utc(ob.UtcDateTime(2017, 1, 1, 0, 0, 0, 123456789))
        self.assertEqual(midnight.tai_nanoseconds - leap.tai_nanoseconds, 1_000_000_000)
        utc = self.time.to_utc(leap)
        self.assertEqual((utc.year, utc.month, utc.day, utc.hour, utc.minute,
                          utc.second, utc.nanosecond), (2016, 12, 31, 23, 59, 60, 123456789))
        self.assertEqual(self.time.tai_minus_utc_seconds(leap), 36)
        self.assertEqual(self.time.tai_minus_utc_seconds(midnight), 37)

    def test_invalid_calendar_and_coverage_errors(self):
        for utc in (ob.UtcDateTime(2026, 2, 29), ob.UtcDateTime(2017, 1, 1, 23, 59, 60),
                    ob.UtcDateTime(2000, 1, 2**31-1), ob.UtcDateTime(2000, 1, -(2**31))):
            with self.assertRaises(ValueError):
                self.time.from_utc(utc)
        for utc in (ob.UtcDateTime(1971, 12, 31), ob.UtcDateTime(2027, 6, 28)):
            with self.assertRaises(ob.TimeDataRangeError):
                self.time.from_utc(utc)
        with self.assertRaises(ob.TimeDataRangeError):
            self.time.to_utc(ob.Epoch(2**63 - 1))

    def test_j2000_tt_and_tai_zero(self):
        self.assertEqual(self.time.from_utc(ob.UtcDateTime(2000, 1, 1, 11, 59, 28)).tai_nanoseconds, 0)
        j2000 = self.time.from_utc(ob.UtcDateTime(2000, 1, 1, 11, 58, 55, 816000000))
        self.assertEqual(j2000.tai_nanoseconds, -32_184_000_000)
        tt = self.time.julian_date(j2000, ob.TimeScale.TT)
        self.assertEqual(tt.scale, ob.TimeScale.TT)
        self.assertAlmostEqual((tt.day - 2451544.5) + tt.fraction, 0.5, delta=2e-16)

    def test_data_metadata_is_owned(self):
        info = self.time.data_info
        del self.time
        self.assertEqual(info.id, "iers-bulletin-c-72-2026-07")
        self.assertEqual(info.utc_valid_until_mjd, 61584)
        self.assertEqual(info.sha256, "6cb6f5d4b819f2e568e25db4b0b26d89dedf031fdffb18bc94d40f4e94e268d7")

    def test_frame_reference_and_position_roundtrip(self):
        epoch = ob.Epoch((2192 * 86400 - 43200) * 1_000_000_000 - 32_184_000_000)
        eop = ob.EarthOrientationSample(epoch, 32.184, 2.55060238e-7, 1.860359247e-6, 0, 0,
                                        "ERFA-2.0.1-t_c2t06a")
        rotation = ob.make_frame_provider().gcrf_to_itrf(epoch, eop)
        reference = Path(__file__).resolve().parents[3] / "tests/reference/time_frames/erfa_2_0_1_c2t06a.csv"
        with reference.open(encoding="utf-8", newline="") as stream:
            expected = [[float(cell) for cell in row] for row in csv.reader(stream)]
        for actual_row, expected_row in zip(rotation.matrix, expected):
            for actual, target in zip(actual_row, expected_row):
                self.assertAlmostEqual(actual, target, delta=1e-12)
        self.assertEqual(rotation.from_frame, ob.FrameId.GCRF)
        self.assertEqual(rotation.to_frame, ob.FrameId.ITRF)
        self.assertEqual(rotation.eop_source_id, "ERFA-2.0.1-t_c2t06a")
        position = [7e6, -3e6, 5e6]
        restored = rotation.inverse_position(rotation.apply_position(position))
        for actual, target in zip(restored, position):
            self.assertAlmostEqual(actual, target, delta=1e-8)

    def test_eop_is_required_at_the_requested_epoch(self):
        frames = ob.make_frame_provider()
        for eop in (ob.EarthOrientationSample(ob.Epoch(1), -32, 0, 0, 0, 0, "wrong-epoch"),
                    ob.EarthOrientationSample(ob.Epoch(0), -32, 0, 0, 0, 0, ""),
                    ob.EarthOrientationSample(ob.Epoch(0), float("nan"), 0, 0, 0, 0, "bad-data")):
            with self.assertRaises(ValueError):
                frames.gcrf_to_itrf(ob.Epoch(0), eop)


if __name__ == "__main__":
    unittest.main()
