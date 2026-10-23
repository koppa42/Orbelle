import math
import unittest
import orbelle as ob


class StandaloneApiTests(unittest.TestCase):
    def test_demo_contract_and_energy(self):
        result = ob.make_circular_demo_session().sample(ob.demo_epochs())
        self.assertEqual(result.provenance.object_id, "demo-satellite")
        self.assertEqual(result.provenance.input_revision, 1)
        self.assertEqual(len(result.states), 6)
        self.assertEqual(result.states[0].position_m, [7_000_000.0, 0.0, 0.0])
        for state in result.states:
            self.assertEqual(state.frame, ob.FrameId.GCRF)
            self.assertEqual(state.origin, ob.OriginId.EARTH)
            radius = math.hypot(*state.position_m)
            energy = sum(v * v for v in state.velocity_mps) / 2 - 3.986004418e14 / radius
            self.assertAlmostEqual(radius, 7_000_000.0, delta=1e-8)
            self.assertAlmostEqual(energy, -3.986004418e14 / 14_000_000.0, delta=1e-7)

    def test_epoch_preserves_nanoseconds_at_large_epoch(self):
        self.assertEqual(ob.Epoch(8_000_000_000_000_000_001).seconds_since(
            ob.Epoch(8_000_000_000_000_000_000)), 1e-9)

    def test_invalid_epoch_is_not_silently_converted(self):
        with self.assertRaises(TypeError):
            ob.Epoch("2026-01-01T00:00:00Z")
        with self.assertRaises(TypeError):
            ob.Epoch(0.5)

    def test_results_survive_session_destruction_and_are_read_only(self):
        result = ob.make_circular_demo_session().sample([ob.Epoch(0)])
        state = result.states[0]
        del result
        self.assertEqual(state.position_m[0], 7_000_000.0)
        with self.assertRaises(AttributeError):
            state.position_m = [0.0, 0.0, 0.0]

    def test_empty_sampling_is_explicit(self):
        result = ob.make_circular_demo_session().sample([])
        self.assertEqual(result.states, [])

    def test_revision_copy_keeps_science_and_original_provenance(self):
        original = ob.make_circular_demo_session()
        updated = original.with_input_revision(42)
        self.assertEqual(original.provenance.input_revision, 1)
        self.assertEqual(updated.provenance.input_revision, 42)
        self.assertEqual(updated.provenance.algorithm, original.provenance.algorithm)
        self.assertEqual(updated.provenance.object_id, original.provenance.object_id)
        epochs = ob.demo_epochs()
        expected = original.sample(epochs).states
        del original
        actual = updated.sample(epochs)
        self.assertEqual(actual.provenance.input_revision, 42)
        for left, right in zip(expected, actual.states, strict=True):
            self.assertEqual(left.position_m, right.position_m)
            self.assertEqual(left.velocity_mps, right.velocity_mps)
        with self.assertRaises((TypeError, OverflowError)):
            updated.with_input_revision(-1)


if __name__ == "__main__":
    unittest.main()

