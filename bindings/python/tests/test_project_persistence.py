from pathlib import Path
import struct
import tempfile
import unittest

import orbelle as ob
from orbelle import units as u
from project_format import rewrite_section

EOP = Path(ob.__file__).parent / "data/eop/iers_c04_2000_01_01_2000_01_02/manifest.json"


class ProjectPersistenceTests(unittest.TestCase):
    def setUp(self):
        self.temp = tempfile.TemporaryDirectory()
        self.addCleanup(self.temp.cleanup)
        self.path = Path(self.temp.name) / "任务.orbelle"
        self.project = ob.Project("Mission", ob.Epoch(0), ob.Epoch(100))
        self.satellite = self.project.objects.satellite("Satellite", orbit=ob.orbits.TwoBody(initial=ob.OrbitInput().initial))
        self.station = self.project.objects.ground_station("Station", location=ob.Wgs84Location(u.deg(0), u.deg(0)))
        self.sensor = self.project.objects.sensor("Sensor", parent=self.satellite, field_of_view=ob.sensors.Cone(u.deg(20)))

    def test_save_analysis_is_explicit_and_allows_names_and_styles(self):
        self.project.save(self.path)
        self.assertEqual(self.project.analysis_status, ob.AnalysisStatus.UNRECORDED)
        analysis = self.project.prepare()
        self.satellite.name = "Renamed"
        self.satellite.display = ob.OrbitStyle(line_width=3)
        self.project.save(self.path, overwrite=True, analysis=analysis)
        self.assertFalse(self.project.dirty)
        self.assertEqual(self.project.analysis_status, ob.AnalysisStatus.CURRENT)
        opened = ob.Project.open(self.path)
        self.assertFalse(opened.dirty)
        self.assertEqual(opened.objects[self.satellite.id].name, "Renamed")
        self.assertEqual(opened.analysis_record.input_sha256, self.project.analysis_record.input_sha256)
        with self.assertRaises(ob.ProjectError):
            opened.save(self.path, overwrite=True, analysis=analysis)
        with self.assertRaises(AttributeError):
            self.project.analysis_record.input_sha256 = "altered"

    def test_physical_edits_reject_old_analysis_and_plain_save_preserves_stale_record(self):
        analysis = self.project.prepare()
        self.project.save(self.path, analysis=analysis)
        before = self.path.read_bytes()
        self.sensor.field_of_view = ob.sensors.Cone(u.deg(15))
        self.assertEqual(self.project.analysis_status, ob.AnalysisStatus.STALE)
        with self.assertRaises(ob.RevisionConflictError):
            self.project.save(self.path, overwrite=True, analysis=analysis)
        self.assertEqual(self.path.read_bytes(), before)
        self.project.save(self.path, overwrite=True)
        self.assertEqual(ob.Project.open(self.path).analysis_status, ob.AnalysisStatus.STALE)
        self.project.save(self.path, overwrite=True, analysis=self.project.prepare())
        self.assertEqual(ob.Project.open(self.path).analysis_status, ob.AnalysisStatus.CURRENT)

    def test_failures_preserve_revision_provenance_and_file(self):
        self.project.save(self.path)
        before, revision = self.path.read_bytes(), self.project.revision
        analysis = self.project.prepare()
        events = []
        self.project.subscribe(events.append)
        with self.assertRaises(ob.ProjectError):
            self.project.save(self.path, analysis=analysis)
        self.assertEqual(self.path.read_bytes(), before)
        self.assertEqual(self.project.revision, revision)
        self.assertIsNone(self.project.analysis_record)
        self.assertFalse(self.project.dirty)
        self.assertFalse(events)
        with self.project.edit():
            with self.assertRaises(RuntimeError):
                self.project.save(self.path, overwrite=True, analysis=analysis)
        with self.assertRaises(TypeError):
            self.project.save(self.path, overwrite=1)
        with self.assertRaises(TypeError):
            self.project.save(self.path, analysis=self.project)

    def test_time_only_data_is_saved_and_checked_without_eop(self):
        data = ob.DataSnapshot.builtin_time()
        self.project.save(self.path, analysis=self.project.prepare(data=data))
        self.assertIsNone(self.project.earth_orientation)
        self.assertEqual(self.project.time_info.sha256, data.time_info.sha256)
        self.assertEqual([r.role for r in self.project.data_lock.references], ["utc-time"])
        with self.assertRaises(ob.AnalysisCapabilityError):
            ob.Project.open(self.path)
        opened = ob.load_project(self.path, data=data)
        self.assertEqual(opened.analysis_status, ob.AnalysisStatus.CURRENT)
        self.assertFalse(opened.dirty)

    def test_open_plan_pins_file_and_data_and_requires_consent_every_time(self):
        data = ob.DataSnapshot.load(eop_manifest=EOP)
        previous = ob.DataSnapshot.load(eop_manifest=EOP, extrapolation_days=1)
        self.project.save(self.path, analysis=self.project.prepare(data=previous))
        with self.assertRaises(ob.DataChangedError) as caught:
            ob.Project.open(self.path, data=data)
        plan = caught.exception.plan
        self.assertIsInstance(plan, ob.ProjectOpenPlan)
        self.assertEqual(plan.name, "Mission")
        self.assertEqual([v.role for v in plan.data_changes], ["earth-orientation"])
        self.path.unlink()
        with self.assertRaises(ob.DataChangedError):
            plan.open()
        with self.assertRaises(TypeError):
            plan.open(accept_data_changes=1)
        opened = plan.open(accept_data_changes=True)
        self.assertTrue(opened.dirty)
        self.assertEqual(opened.data_lock.sha256, data.data_lock.sha256)
        self.assertEqual(opened.analysis_status, ob.AnalysisStatus.STALE)
        opened.name = "Independent"
        self.assertEqual(plan.open(accept_data_changes=True).name, "Mission")
        # Confirmation applies only to this plan, with no global auto-accept flag.
        with self.assertRaises(ob.DataChangedError):
            plan.open()

    def test_missing_or_out_of_range_data_cannot_be_accepted(self):
        self.project.save(self.path, analysis=self.project.prepare(data=ob.DataSnapshot.load(eop_manifest=EOP)))
        with self.assertRaises(ob.AnalysisCapabilityError):
            ob.Project.plan_open(self.path, data=ob.DataSnapshot.builtin_time())
        long = ob.Project("Future", ob.Epoch(0), ob.Epoch(3 * 86400_000_000_000))
        long.save(self.path, overwrite=True)
        with self.assertRaises(ob.EopCoverageError):
            ob.Project.plan_open(self.path, data=ob.DataSnapshot.load(eop_manifest=EOP))
        long.last = ob.Epoch(8_000_000_000_000_000_000)
        long.save(self.path, overwrite=True)
        with self.assertRaises(ob.TimeDataRangeError):
            ob.Project.plan_open(self.path, data=ob.DataSnapshot.builtin_time())
        with self.assertRaises(TypeError):
            ob.Project.plan_open(self.path, data="implicit catalog")

    def test_catalog_open_plan_survives_current_pointer_changes(self):
        catalog = ob.DataCatalog(Path(self.temp.name) / "catalog")
        entry = catalog.install_eop(EOP)
        catalog.activate_eop(entry.lock_sha256)
        previous = ob.DataSnapshot.load(eop_manifest=EOP, extrapolation_days=1)
        self.project.save(self.path, analysis=self.project.prepare(data=previous))
        plan = ob.Project.plan_open(self.path, data=catalog)
        catalog.set_setting("current-c04", "f" * 64)
        with self.assertRaises(ob.EopCoverageError):
            ob.Project.plan_open(self.path, data=catalog)
        opened = plan.open(accept_data_changes=True)
        self.assertEqual(opened.data_lock.sha256, entry.lock_sha256)
        self.assertEqual(opened.objects[self.satellite.id].name, "Satellite")
        self.assertTrue(opened.dirty)

    def test_sgp4_time_only_save_reopen_and_prepare(self):
        data = ob.DataSnapshot.builtin_time()
        tle = ob.Tle(
            "1 00005U 58002B   00179.78495062  .00000023  00000-0  28098-4 0  4753",
            "2 00005  34.2682 348.7242 1859667 331.7664  19.3264 10.82419157413667",
        )
        first = ob.make_builtin_time_provider().from_utc(tle.epoch_utc)
        project = ob.Project("TLE", first, ob.Epoch(first.tai_nanoseconds + 1_000_000_000))
        satellite = project.objects.satellite("TLE", orbit=ob.orbits.Sgp4(tle=tle))
        analysis = project.prepare(data=data)
        project.save(self.path, analysis=analysis)
        opened = ob.Project.open(self.path, data=data)
        state = opened.prepare(data=data).state(opened.objects[satellite.id], at=first, frame=ob.FrameId.TEME)
        self.assertEqual(state.position_m, analysis.state(satellite, at=first, frame=ob.FrameId.TEME).position_m)

    def test_current_format_roundtrip_and_integrity(self):
        self.project.save(self.path, analysis=self.project.prepare(data=ob.DataSnapshot.builtin_time()))
        original = self.path.read_bytes()
        self.assertEqual(struct.unpack_from("<I", original, 8)[0], 49)
        raw = ob.load_project_data(self.path)
        raw.save(self.path, overwrite=True)
        self.assertEqual(self.path.read_bytes(), original)
        # Repaired CRCs still cannot make a missing TIME agree with LOCK.
        self.path.write_bytes(rewrite_section(original, b"TIME", lambda body: None))
        with self.assertRaises(ob.ProjectError):
            ob.load_project_data(self.path)
        self.path.write_bytes(rewrite_section(original, b"ANLY", lambda body: body + b"trailing"))
        with self.assertRaises(ob.ProjectError):
            ob.load_project_data(self.path)
        self.project.save(self.path, overwrite=True)
        # Input-only projects remain valid, without fabricating an analysis record.
        plain = ob.Project("Inputs", ob.Epoch(0), ob.Epoch(100))
        plain.save(self.path, overwrite=True)
        self.assertEqual(ob.Project.open(self.path).analysis_status, ob.AnalysisStatus.UNRECORDED)


if __name__ == "__main__":
    unittest.main()
