from concurrent.futures import ThreadPoolExecutor
from pathlib import Path
import math
import unittest
import orbitforge as of

ROOT=Path(__file__).resolve().parents[3]

def covariance():
    value=of.HpopCovarianceInput()
    value.state=[[(10000 if i<3 else .1) if i==j else 0. for j in range(6)] for i in range(6)]
    for name in ("drag","srp"):
        p=of.HpopConsiderParameter();p.variance=.0001
        p.state_cross_covariance=[.02,-.03,.01,.0001,-.0002,.0003]
        setattr(value,name,p)
    value.drag_srp_covariance=.00001
    return value

class CelestialFramesTests(unittest.TestCase):
    def test_installed_provider_against_orekit(self):
        frames=of.make_frame_provider()
        rows=[s.split() for s in (ROOT/"tests/reference/time_frames/hpop_celestial_frames.txt").read_text().splitlines()
              if s and not s.startswith("#")]
        self.assertEqual(len(rows),48)
        for row in rows:
            epoch=of.Epoch(int(row[0]));values=list(map(float,row[2:]))
            eop=of.EarthOrientationState(of.EarthOrientationSample(epoch,*values[:5],"orekit"),
                                         of.EarthOrientationRates(*values[5:10]))
            target=(of.FrameId.FK5_J2000,of.FrameId.TRUE_OF_DATE)[int(row[1])]
            actual=frames.gcrf_to_celestial_state(epoch,eop,target)
            for k in range(9):
                self.assertLessEqual(abs(actual.rotation.matrix[k//3][k%3]-values[10+k]),2e-11)
                self.assertLessEqual(abs(actual.rotation_rate_per_second[k//3][k%3]-values[19+k]),2e-15)
            self.assertIn("iers1996",actual.rotation.model)
            self.assertEqual(actual.rotation.eop_source_id,"orekit")

    def test_covariance_propagation_endpoints_ownership_and_missing_resources(self):
        manifest=ROOT/"data/samples/eop/iers_c04_2016_12_28_2017_01_04/manifest.json"
        data=of.DataSnapshot.load(eop_manifest=manifest)
        eop=of.load_eop_snapshot(manifest)
        v=of.OrbitInput();v.propagator=of.OrbitPropagator.HPOP
        opt=v.hpop;opt.gravity_degree=opt.gravity_order=0;v.hpop=opt
        for epoch,seconds in ((eop.data_info.first_epoch,30),(eop.data_info.last_epoch,-30)):
            v.initial=of.CartesianState(epoch,of.OriginId.EARTH,of.FrameId.GCRF,[7e6,2e6,1e6],[-1000,7000,1000])
            end=of.Epoch(epoch.tai_nanoseconds+seconds*10**9)
            baseline=of.propagate_hpop_covariance(v,end,covariance(),data=data)
            for frame in (of.HpopCovarianceFrame.FK5_J2000,of.HpopCovarianceFrame.TRUE_OF_DATE):
                for velocity in (of.HpopCovarianceVelocity.PROJECTED,of.HpopCovarianceVelocity.ROTATING):
                    coordinates=of.HpopCovarianceCoordinates(frame,velocity)
                    with self.assertRaises(ValueError):
                        of.convert_hpop_covariance(v.initial,covariance(),coordinates)
                    converted=of.convert_hpop_covariance(v.initial,covariance(),coordinates,data=data)
                    actual=of.propagate_hpop_covariance(v,end,converted,data=data)
                    self.assertEqual(actual.variational.state.position_m,baseline.variational.state.position_m)
                    for i in range(6):
                        for j in range(6):
                            expected=baseline.covariance.state
                            self.assertLessEqual(abs(actual.covariance.state[i][j]-expected[i][j])/
                                                 math.sqrt(expected[i][i]*expected[j][j]),2e-13)
                    def query(_):
                        return of.convert_hpop_covariance(v.initial,converted,of.HpopCovarianceCoordinates(),data=data).state
                    with ThreadPoolExecutor(max_workers=3) as pool:
                        matrices=list(pool.map(query,range(3)))
                    self.assertEqual(matrices[0],matrices[1]);self.assertEqual(matrices[1],matrices[2])
            outside=of.CartesianState(of.Epoch(epoch.tai_nanoseconds-seconds*10**9),
                                       of.OriginId.EARTH,of.FrameId.GCRF,[7e6,0,0],[0,7500,0])
            with self.assertRaises(of.EopCoverageError):
                of.convert_hpop_covariance(outside,covariance(),coordinates,data=data)
        self.assertEqual(of.FrameId.GCRF.name,"GCRF")
        self.assertNotEqual(of.FrameId.FK5_J2000,of.FrameId.SPICE_J2000)

if __name__=="__main__":
    unittest.main()
