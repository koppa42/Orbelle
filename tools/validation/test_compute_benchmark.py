"""Exercise the real smoke executable and reject corrupted measurement evidence."""

import argparse
import copy
import json
from pathlib import Path
import subprocess
import sys
import unittest
import uuid

from analyze_compute_benchmark import analyze

ARGS = None


class ComputeContract(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        cls.work = ARGS.work.resolve() / uuid.uuid4().hex
        cls.work.mkdir(parents=True)
        cls.source = ARGS.source.resolve()
        cls.executable = ARGS.executable.resolve()
        cls.raw_path = cls.work / "smoke.json"
        cls.command = [str(cls.executable), "--profile", "smoke", "--data-root",
                       str(cls.source / "data/samples"), "--workspace", str(cls.work / "spice"),
                       "--output", str(cls.raw_path)]
        completed = subprocess.run(cls.command, text=True, capture_output=True, timeout=60)
        if completed.returncode:
            raise RuntimeError(completed.stdout + completed.stderr)
        cls.raw = json.loads(cls.raw_path.read_text())

    def reject(self, mutation, message=None):
        changed = copy.deepcopy(self.raw)
        mutation(changed)
        with self.assertRaises((ValueError, KeyError, TypeError), msg=message):
            analyze(changed, "smoke")

    def test_real_smoke_is_complete_but_not_performance_baseline(self):
        result = analyze(self.raw, "smoke")
        self.assertTrue(result["valid"])
        self.assertFalse(result["performance_baseline"])
        self.assertIsNone(result["acceptance"])
        self.assertEqual(len(result["cases"]), 6)
        self.assertEqual(len(self.raw["samples"]), 18)
        with self.assertRaises(ValueError):
            analyze(self.raw, "baseline")

    def test_rejects_incomplete_or_mislabeled_work(self):
        mutations = [lambda r: r["samples"].pop(),
                     lambda r: r["samples"].append(r["samples"][-1]),
                     lambda r: r["samples"][0].update(operations=55),
                     lambda r: r["samples"][0].update(iteration=True),
                     lambda r: r["samples"][0].update(phase="measure"),
                     lambda r: r["samples"][0].update(case="spice_scalar"),
                     lambda r: r["workload"].update(objects=9),
                     lambda r: r.update(profile="baseline")]
        for i, mutation in enumerate(mutations):
            with self.subTest(i=i):
                self.reject(mutation)

    def test_rejects_failed_numeric_checks_even_with_valid_flag(self):
        for field, value in [("max_phase_position_m", 1), ("max_radius_error_m", float("nan")),
                             ("max_energy_error_m2_s2", -1), ("max_momentum_error_m2_s", True)]:
            with self.subTest(field=field):
                self.reject(lambda r: r["samples"][0]["checks"].update({field: value}))
        self.reject(lambda r: r["samples"][6]["checks"].update(max_roundtrip_velocity_mps=1e-6))
        self.reject(lambda r: r["samples"][1].update(valid=False))
        self.reject(lambda r: r.update(error="incomplete computation"))

    def test_rejects_bad_clocks_and_changed_outputs(self):
        self.reject(lambda r: r["samples"][0].update(end_ns=r["samples"][0]["begin_ns"]))
        self.reject(lambda r: r["samples"][1].update(begin_ns=r["samples"][0]["begin_ns"]))
        self.reject(lambda r: r["samples"][0].update(cpu_end_ns=0))
        self.reject(lambda r: r["clocks"].update(steady=False))
        self.reject(lambda r: r["samples"][1]["checks"].update(digest="0" * 16))
        # A consistently changed batch digest must still disagree with scalar.
        def alter_batch(report):
            for sample in report["samples"]:
                if sample["case"] == "spice_batch":
                    sample["checks"]["digest"] = "1" * 16
        self.reject(alter_batch)

    def test_rejects_incomplete_or_changed_provenance(self):
        for key in ("time_sha256", "time_id", "kernels", "spice_toolkit_version",
                    "spice_first_tai_ns", "spice_last_tai_ns", "eop_source_id", "spice_source_id"):
            with self.subTest(key=key):
                self.reject(lambda r: r["data"].pop(key))
        self.reject(lambda r: r["data"]["kernels"][0].update(sha256="0" * 64))
        self.reject(lambda r: r["data"].update(frame_derivative_model="unknown"))

    def test_refuses_to_overwrite_evidence(self):
        before = self.raw_path.read_bytes()
        completed = subprocess.run(self.command, text=True, capture_output=True, timeout=60)
        self.assertNotEqual(completed.returncode, 0)
        self.assertIn("already exists", completed.stderr)
        self.assertEqual(self.raw_path.read_bytes(), before)

    def test_missing_data_cannot_produce_valid_measurement(self):
        command = self.command.copy()
        command[command.index("--data-root") + 1] = str(self.work / "missing-data")
        destination = self.work / "failed.json"
        command[-1] = str(destination)
        completed = subprocess.run(command, text=True, capture_output=True, timeout=60)
        self.assertNotEqual(completed.returncode, 0)
        failed = json.loads(destination.read_text())
        self.assertFalse(failed["valid"])
        self.assertIn("error", failed)
        with self.assertRaises(ValueError):
            analyze(failed)

    def test_unknown_profile_is_rejected(self):
        command = self.command.copy()
        command[2] = "baseline-fast"
        command[-1] = str(self.work / "unknown-profile.json")
        completed = subprocess.run(command, text=True, capture_output=True, timeout=60)
        self.assertNotEqual(completed.returncode, 0)
        self.assertIn("Unknown profile", completed.stderr)

    def test_runner_records_inputs_and_preserves_existing_run(self):
        directory = self.work / "captured"
        command = [sys.executable, str(self.source / "tools/validation/run_compute_benchmark.py"),
                   "--executable", str(self.executable), "--source", str(self.source),
                   "--profile", "smoke", "--output", str(directory)]
        completed = subprocess.run(command, text=True, capture_output=True, timeout=60)
        self.assertEqual(completed.returncode, 0, completed.stdout + completed.stderr)
        capture = json.loads((directory / "capture.json").read_text())
        self.assertTrue(capture["valid"])
        self.assertTrue(capture["source_unchanged"])
        self.assertTrue(capture["runtime_unchanged"])
        self.assertTrue(capture["build_inputs_unchanged"])
        self.assertIn("benchmarks/compute/main.cpp", capture["source_sha256"])
        self.assertIn(str(self.executable), capture["runtime_sha256"])
        before = (directory / "capture.json").read_bytes()
        completed = subprocess.run(command, text=True, capture_output=True, timeout=60)
        self.assertNotEqual(completed.returncode, 0)
        self.assertEqual((directory / "capture.json").read_bytes(), before)


if __name__ == "__main__":
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--executable", required=True, type=Path)
    parser.add_argument("--source", required=True, type=Path)
    parser.add_argument("--work", required=True, type=Path)
    ARGS, remaining = parser.parse_known_args()
    unittest.main(argv=[sys.argv[0], *remaining])
