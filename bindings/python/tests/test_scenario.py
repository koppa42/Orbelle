import unittest
import orbelle as of


class ScenarioTests(unittest.TestCase):
    def test_empty_then_insert_is_immutable(self):
        empty = of.Scenario('Mission', of.Epoch(0), of.Epoch(10_000_000_000))
        self.assertEqual(empty.object_count, 0)
        self.assertEqual(empty.sample([of.Epoch(0)]), [])
        session = of.make_circular_demo_session()
        scene = empty.with_object(session)
        self.assertEqual(empty.object_count, 0)
        self.assertEqual(scene.object_count, 1)
        self.assertEqual(scene.sample([of.Epoch(0)])[0].provenance.object_id,
                         session.provenance.object_id)
        with self.assertRaises(ValueError):
            scene.with_object(session)
        with self.assertRaises(IndexError):
            scene.sample([of.Epoch(-1)])

    def test_invalid_creation(self):
        with self.assertRaises(ValueError):
            of.Scenario(' ', of.Epoch(0), of.Epoch(10))
        with self.assertRaises(ValueError):
            of.Scenario('Mission', of.Epoch(10), of.Epoch(0))

    def test_batch_matches_single_insert_and_failure_preserves_readers(self):
        empty = of.Scenario('Mission', of.Epoch(0), of.Epoch(10_000_000_000))
        session = of.make_circular_demo_session()
        batch = empty.with_objects([session])
        single = empty.with_object(session)
        self.assertEqual(batch.revision, single.revision)
        self.assertEqual(batch.object_count, single.object_count)
        self.assertEqual(batch.sample([of.Epoch(0)])[0].states[0].position_m,
                         single.sample([of.Epoch(0)])[0].states[0].position_m)
        self.assertEqual(batch.with_objects([]).revision, batch.revision)
        with self.assertRaises(ValueError):
            empty.with_objects([session, session])
        self.assertEqual(empty.object_count, 0)
        self.assertEqual(batch.object_count, 1)
