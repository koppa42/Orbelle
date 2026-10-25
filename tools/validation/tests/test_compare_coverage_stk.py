"""Independent sanity cases for the STK result comparison, without an STK license."""
import importlib.util
from pathlib import Path
import unittest

path = Path(__file__).resolve().parents[1] / "compare_coverage_stk.py"
spec = importlib.util.spec_from_file_location("compare_coverage_stk", path)
comparison = importlib.util.module_from_spec(spec)
spec.loader.exec_module(comparison)


class CoverageComparisonTests(unittest.TestCase):
    def test_multiple_assets_merge_overlap_without_double_counting(self):
        windows = comparison.union([(7, 9), (2, 4), (1, 3), (4, 5), (2, 3), (1, 3)])
        self.assertEqual(windows, [[1, 5], [7, 9]])
        self.assertEqual(sum(b - a for a, b in windows), 6)
        with self.assertRaises(ValueError):
            comparison.union([(4, 3)])
        with self.assertRaises(ValueError):
            comparison.union([(float("nan"), 3)])

    def test_area_weighted_instantaneous_and_cumulative_coverage(self):
        # Unequal 1:3 cell areas catch accidental point-count percentages.
        points = [[[1, 2]], [[3, 4]]]
        self.assertEqual(comparison.coverage_at(points, [1, 3], 1.5), 25)
        self.assertEqual(comparison.coverage_at(points, [1, 3], 3.5), 75)
        self.assertEqual(comparison.coverage_at(points, [1, 3], 5), 0)
        self.assertEqual(comparison.coverage_at(points, [1, 3], 5, cumulative=True), 100)
        self.assertEqual(comparison.coverage_at([[], [[3, 4]]], [1, 3], 5, cumulative=True), 75)

    def test_reference_point_order_and_longitude_wrap_do_not_change_matching(self):
        values = {"Latitude": [10, 20], "Longitude": [190, 40], "FOM Value": [3, 8]}
        result = comparison.point_map(values, "FOM Value")
        self.assertEqual(result[comparison.coordinate_key(10, -170)], 3)
        self.assertEqual(result[comparison.coordinate_key(20, 40)], 8)
        with self.assertRaises(ValueError):
            comparison.point_map({"Latitude": [0, 0], "Longitude": [10, 370], "FOM Value": [1, 2]}, "FOM Value")


if __name__ == "__main__":
    unittest.main()
