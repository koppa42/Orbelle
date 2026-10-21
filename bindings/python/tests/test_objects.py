"""Public editing API contracts, using the installed Python package."""
import asyncio
from concurrent.futures import ThreadPoolExecutor
from dataclasses import FrozenInstanceError
import inspect
from pathlib import Path
import tempfile
import unittest
import orbitforge as of
from orbitforge import units as u
from orbitforge.sensors import Cone, Pointing


def make_project():
    project = of.Project("Objects", of.Epoch(0), of.Epoch(600_000_000_000))
    with project.edit():
        satellite = project.objects.satellite("Observer", orbit=of.orbits.TwoBody(
            initial=of.orbits.Keplerian(epoch=project.first, semi_major_axis=u.km(7000),
                                       eccentricity=0.001, inclination=u.deg(53))))
        station = project.objects.ground_station("Hong Kong", location=of.Wgs84Location(
            longitude=u.deg(114.2), latitude=u.deg(22.3), ellipsoid_height=u.m(350)))
        sensor = project.objects.sensor("Camera", parent=satellite,
            field_of_view=Cone(half_angle=u.deg(20)), pointing=Pointing.track(station))
    return project, satellite, station, sensor


class ObjectEditingTest(unittest.TestCase):
    def test_geometric_sensor_patterns_round_trip_and_atomic_rejection(self):
        from orbitforge.sensors import ComplexConic, Rectangular, Custom
        project, _, _, sensor = make_project()
        shapes = [ComplexConic(u.deg(10),u.deg(40),u.deg(300),u.deg(60)),
                  Rectangular(u.deg(30),u.deg(15)),
                  Custom(tuple((u.deg(20),u.deg(clock)) for clock in (0,90,180,270)))]
        with tempfile.TemporaryDirectory() as directory:
            path=Path(directory)/"patterns.orbitforge"
            for shape in shapes:
                sensor.field_of_view=shape
                self.assertEqual(sensor.field_of_view,shape)
                project.save(path,overwrite=True)
                restored=of.Project.open(path)
                self.assertEqual(restored.objects[sensor.id].field_of_view,shape)
        before=project.revision
        with self.assertRaises(ValueError):
            sensor.field_of_view=ComplexConic(u.deg(50),u.deg(40))
        self.assertEqual(project.revision,before)
        self.assertEqual(sensor.field_of_view,shapes[-1])

    def test_creation_lookup_and_fixed_iteration(self):
        project, satellite, station, sensor = make_project()
        self.assertEqual(project.revision, 1)
        self.assertEqual(len(project.objects), 3)
        self.assertEqual(project.objects[sensor.id], sensor)
        self.assertEqual(project.objects.children(satellite), (sensor,))
        self.assertEqual(sensor.parent, satellite)
        self.assertIsNone(project.objects.get("Camera"))
        sentinel = object()
        self.assertIs(project.objects.get("missing", sentinel), sentinel)
        with self.assertRaises(KeyError):
            project.objects["Camera"]
        iterator = iter(project.objects)
        copy = project.objects.duplicate(sensor, name="Camera")
        self.assertEqual(len(tuple(iterator)), 3)
        self.assertEqual(project.objects.find(name="Camera", type=of.objects.Sensor), (sensor, copy))
        created = project.objects.create(of.objects.GroundStation("Station", station.location))
        self.assertNotEqual(created.id, station.id)
        self.assertEqual(created.location, station.location)

    def test_atomic_keyword_updates_and_noop(self):
        project, _, _, sensor = make_project()
        changes = []
        project.subscribe(changes.append)
        with self.assertRaises(of.ProjectError):
            sensor.update(name="Invalid", field_of_view=Cone(half_angle=u.deg(180)))
        self.assertEqual(sensor.name, "Camera")
        self.assertEqual(project.revision, 1)
        self.assertEqual(changes, [])
        self.assertIsNone(sensor.update(name="Narrow", field_of_view=Cone(half_angle=u.deg(15)),
                                       pointing=Pointing.nadir()))
        self.assertEqual(changes[0].updated[0].fields, ["name", "field_of_view", "pointing"])
        sensor.update(name="Narrow")
        sensor.update()
        self.assertEqual(len(changes), 1)
        project.objects.update(sensor, field_of_view=Cone(half_angle=u.deg(10)))
        self.assertEqual(sensor.field_of_view.half_angle, u.deg(10))

    def test_snapshots_are_immutable_values(self):
        project, satellite, station, sensor = make_project()
        saved = project.objects.read(sensor)
        with self.assertRaises(FrozenInstanceError):
            sensor.field_of_view.half_angle = u.deg(5)
        with self.assertRaises(TypeError):
            satellite.orbit.initial.position_m[0] = 0
        with self.assertRaises(FrozenInstanceError):
            station.location.latitude = u.deg(0)
        sensor.name = "Changed"
        self.assertEqual(saved.name, "Camera")
        project.objects.remove(satellite)
        self.assertEqual(saved.field_of_view.half_angle, u.deg(20))
        self.assertIn("deleted", repr(sensor))
        with self.assertRaises(of.ObjectNotFoundError):
            _ = sensor.name

    def test_types_keywords_and_readonly_fields_fail_before_mutation(self):
        project, satellite, station, sensor = make_project()
        for operation in (
            lambda: sensor.update(field_of_veiw=Cone(u.deg(10))),
            lambda: sensor.update(name=None),
            lambda: sensor.update(field_of_view=Cone(15)),
            lambda: station.update(minimum_elevation=u.m(5)),
            lambda: project.objects.update(sensor, orbit=satellite.orbit),
            lambda: project.objects.sensor("Bad", parent=station, field_of_view=Cone(u.deg(20))),
        ):
            with self.assertRaises(TypeError):
                operation()
        for field in ("id", "parent", "field_of_veiw"):
            with self.assertRaises(AttributeError):
                setattr(sensor, field, "invalid")
        self.assertEqual(project.revision, 1)
        self.assertIn("field_of_view", inspect.signature(sensor.update).parameters)
        self.assertIn("orbit", inspect.signature(project.objects.satellite).parameters)

    def test_context_reads_own_writes_and_rolls_back(self):
        project, satellite, station, sensor = make_project()
        with self.assertRaisesRegex(RuntimeError, "cancel"):
            with project.edit():
                sensor.name = "Draft"
                station.minimum_elevation = u.deg(10)
                new = project.objects.sensor("New", parent=satellite, field_of_view=Cone(u.deg(10)))
                self.assertEqual(sensor.name, "Draft")
                self.assertEqual(len(project.objects), 4)
                raise RuntimeError("cancel")
        self.assertEqual(sensor.name, "Camera")
        self.assertEqual(station.minimum_elevation, u.deg(0))
        self.assertEqual(project.revision, 1)
        self.assertIsNone(project.objects.get(new.id))
        with self.assertRaises(of.ObjectNotFoundError):
            _ = new.name
        replacement = project.objects.sensor("New", parent=satellite, field_of_view=Cone(u.deg(10)))
        self.assertNotEqual(new.id, replacement.id)

    def test_caught_operation_failure_and_commit_failure(self):
        project, _, station, sensor = make_project()
        with project.edit():
            sensor.name = "Draft"
            with self.assertRaises(of.ProjectError):
                sensor.update(name="Lost", field_of_view=Cone(u.deg(-1)))
            self.assertEqual(sensor.name, "Draft")
            sensor.field_of_view = Cone(u.deg(10))
        self.assertEqual(project.revision, 2)
        with self.assertRaises(of.ProjectError):
            with project.edit():
                project.objects.remove(station)
                self.assertEqual(sensor.pointing.target_id, station.id)
        self.assertEqual(len(project.objects), 3)
        self.assertEqual(project.revision, 2)
        with project.edit():
            project.objects.remove(station)
            sensor.pointing = Pointing.nadir()
        self.assertEqual(project.revision, 3)
        self.assertIsNone(project.objects.get(station.id))

    def test_nested_edits_cross_project_and_revision_conflict(self):
        project, _, station, sensor = make_project()
        other, other_satellite, other_station, _ = make_project()
        with self.assertRaises(of.ProjectError):
            project.objects.reparent(sensor, other_satellite)
        with self.assertRaises(of.ProjectError):
            sensor.pointing = Pointing.track(other_station)
        with project.edit():
            with self.assertRaises(RuntimeError):
                with project.edit():
                    pass
            with other.edit():
                other_station.name = "Other"
        with self.assertRaises(of.RevisionConflictError):
            with project.edit():
                station.name = "Stale"
                with ThreadPoolExecutor(1) as executor:
                    executor.submit(setattr, station, "name", "Concurrent").result()
        self.assertEqual(station.name, "Concurrent")
        station.name = "Next"
        self.assertEqual(station.name, "Next")

    def test_duplicate_cascade_reparent_and_soft_report_bindings(self):
        project, satellite, station, sensor = make_project()
        definition = of.ReportDefinition()
        definition.id, definition.name, definition.query.object_ids = "report", "Report", [sensor.id]
        project.set_reports([definition])
        plan = project.objects.plan_remove(station)
        self.assertEqual(plan.blocking_sensors, [sensor.id])
        self.assertEqual(plan.revision, project.revision)
        with self.assertRaises(of.ProjectError):
            project.objects.remove(station)
        copy = project.objects.duplicate(satellite, name="Copy")
        child, = project.objects.children(copy)
        self.assertNotEqual(child.id, sensor.id)
        self.assertEqual(child.pointing.target_id, station.id)
        project.objects.reparent(sensor, copy)
        self.assertEqual(sensor.parent, copy)
        self.assertEqual(project.objects.children(satellite), ())
        plan = project.objects.plan_remove(copy)
        self.assertEqual(plan.affected_reports, ["report"])
        project.objects.remove(copy)
        self.assertEqual(project.to_data().reports[0].query.object_ids, [sensor.id])
        self.assertIsNone(project.objects.get(child.id))

    def test_observers_see_published_state_and_may_edit(self):
        project, _, station, sensor = make_project()
        seen = []

        def observer(change):
            seen.append((change.revision, sensor.name))
            if change.revision == 2:
                with project.edit():
                    station.name = "Observer edit"

        token = project.subscribe(observer)
        project.subscribe(lambda change: (_ for _ in ()).throw(ValueError("observer failure")))
        with project.edit():
            sensor.name = "Committed"
        self.assertEqual(seen, [(2, "Committed"), (3, "Committed")])
        self.assertEqual(station.name, "Observer edit")
        self.assertEqual(len(project.observer_errors), 2)
        project.unsubscribe(token)
        sensor.name = "After unsubscribe"
        self.assertEqual(len(seen), 2)

    def test_retained_events_own_exact_revision_snapshots(self):
        project, _, _, sensor = make_project()
        events = []
        project.subscribe(events.append)
        for index in range(20):
            sensor.name = f"Camera {index}"
        first = events[0]
        self.assertEqual(first.revision, 2)
        self.assertEqual(first.after.revision, 2)
        self.assertEqual(first.before.project.sensors[0].name, "Camera")
        self.assertEqual(first.after.project.sensors[0].name, "Camera 0")
        detached = first.after.project
        detached.name = "Detached event"
        self.assertEqual(first.after.name, "Objects")
        del sensor, project
        self.assertEqual(first.updated[0].fields, ["name"])
        self.assertEqual(first.after.project.sensors[0].name, "Camera 0")

    def test_save_dirty_roundtrip_and_computation_bridge(self):
        project, satellite, _, sensor = make_project()
        self.assertTrue(project.dirty)
        with tempfile.TemporaryDirectory() as directory:
            path = Path(directory) / "objects.orbitforge"
            with project.edit():
                with self.assertRaises(RuntimeError):
                    project.save(path)
                with self.assertRaises(RuntimeError):
                    project.to_scenario()
            project.save(path)
            self.assertFalse(project.dirty)
            loaded = of.load_project(path)
            self.assertEqual(loaded.objects[sensor.id].name, sensor.name)
            self.assertEqual(loaded.objects[sensor.id].parent.id, satellite.id)
            expected = project.to_scenario().sample([project.first])
            actual = loaded.to_scenario().sample([project.first])
            self.assertEqual(expected[0].states[0].position_m, actual[0].states[0].position_m)
            definition = of.ReportDefinition()
            definition.id, definition.name = "summary", "Summary"
            definition.query.kind = of.ReportKind.OBJECT_SUMMARY
            definition.query.object_ids = [satellite.id]
            report = of.generate_report(loaded, definition)
            self.assertGreater(report.table.row_count, 0)
            sensor.name = "Unsaved"
            with self.assertRaises(of.ProjectError):
                project.save(path)
            self.assertTrue(project.dirty)
            detached = project.to_data()
            detached.name = "Detached"
            self.assertEqual(project.name, "Objects")

    def test_settings_are_atomic_and_noop_does_not_dirty(self):
        project, satellite, _, _ = make_project()
        original = project.to_data()
        with self.assertRaises(of.ProjectError):
            project.update(name="Invalid", first=of.Epoch(900_000_000_000))
        self.assertEqual(project.name, original.name)
        with project.edit():
            satellite.name = "Temp"
            satellite.name = "Observer"
        self.assertEqual(project.revision, 1)
        project.update(first=of.Epoch(100), last=of.Epoch(1000))
        self.assertEqual(project.current.tai_nanoseconds, 100)
        self.assertEqual(project.to_data().satellites[0].orbit.last.tai_nanoseconds, 1000)

    def test_saved_ids_preserved_and_native_values_detached(self):
        path = Path(__file__).resolve().parents[3] / "tests/integration/fixtures/project-current.orbitforge"
        raw = of.load_project_data(path)
        project = of.Project.open(path)
        self.assertEqual({obj.id for obj in project.objects},
                         {"satellite-a", "satellite-b", "station-a", "sensor-a"})
        self.assertEqual(project.objects["sensor-a"].parent, project.objects["satellite-a"])
        project.objects[next(iter(project.objects)).id].name = "New name"
        self.assertNotEqual(raw.satellites[0].name, "New name")

    def test_orbit_constructors_preserve_model_and_mean_element_semantics(self):
        first, last = of.Epoch(0), of.Epoch(600_000_000_000)
        mean = of.J2MeanElements(first, 7e6, 0.01, 0.9, 0.1, 0.2, 0.3)
        for configure, session in ((of.orbits.J2, of.make_j2_session), (of.orbits.J4, of.make_j4_session)):
            with self.subTest(model=configure.__name__):
                project = of.Project("Mean orbit", first, last)
                satellite = project.objects.satellite("Satellite", orbit=configure(initial=mean))
                actual = project.to_scenario().sample([first, last])[0]
                expected = session(mean, satellite.id).sample([first, last])
                for got, want in zip(actual.states, expected.states):
                    for a, b in zip(got.position_m, want.position_m):
                        self.assertAlmostEqual(a, b, delta=1e-6)
        tle = of.Tle(
            '1 00005U 58002B   00179.78495062  .00000023  00000-0  28098-4 0  4753',
            '2 00005  34.2682 348.7242 1859667 331.7664  19.3264 10.82419157413667')
        time = of.make_builtin_time_provider()
        epoch = time.from_utc(tle.epoch_utc)
        project = of.Project("TLE", epoch, of.Epoch(epoch.tai_nanoseconds + 60_000_000_000))
        satellite = project.objects.satellite("Vanguard", orbit=of.orbits.Sgp4(tle=tle))
        actual = project.to_scenario(time).sample([epoch])[0].states[0]
        expected = of.make_sgp4_session(tle, time, satellite.id).sample([epoch]).states[0]
        self.assertEqual(actual.frame, of.FrameId.TEME)
        self.assertEqual(actual.position_m, expected.position_m)


class AsyncContextTest(unittest.IsolatedAsyncioTestCase):
    async def test_inherited_context_cannot_share_mutable_edit(self):
        project, _, station, _ = make_project()

        async def child():
            station.name = "Child"

        with project.edit():
            station.name = "Parent"
            with self.assertRaisesRegex(RuntimeError, "another thread or asyncio task"):
                await asyncio.create_task(child())
            with self.assertRaisesRegex(RuntimeError, "another thread or asyncio task"):
                await asyncio.to_thread(setattr, station, "name", "Worker")
            self.assertEqual(station.name, "Parent")
        self.assertEqual(station.name, "Parent")
        await asyncio.create_task(child())
        self.assertEqual(station.name, "Child")


if __name__ == "__main__":
    unittest.main()
