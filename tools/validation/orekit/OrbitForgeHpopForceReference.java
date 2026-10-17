// Independent combined HPOP force assembly: Orekit 13.1.2 / Hipparchus 4.0.2.
// All state and D/K derivatives use Gradient; NRL finite differences are disabled.
import java.nio.file.*;
import java.util.*;
import org.hipparchus.CalculusFieldElement;
import org.hipparchus.analysis.differentiation.Gradient;
import org.hipparchus.geometry.euclidean.threed.*;
import org.orekit.bodies.*;
import org.orekit.frames.*;
import org.orekit.time.*;
import org.orekit.utils.*;
import org.orekit.propagation.*;
import org.orekit.forces.gravity.*;
import org.orekit.forces.gravity.potential.*;
import org.orekit.forces.drag.*;
import org.orekit.forces.radiation.*;
import org.orekit.models.earth.atmosphere.*;

public final class OrbitForgeHpopForceReference {
    static final int DIM=8;
    static final double MU=3.986004415e14, R=6378136.3, MOON_MU=4.902800118e12;
    static final Frame FRAME=FramesFactory.getGCRF();
    static final AbsoluteDate DATE=OrbitForgeRelativityReference.EPOCH;
    static final Vector3D SUN=OrbitForgeRelativityReference.sunPosition(0);
    static final Vector3D MOON=new Vector3D(3e8,-2e8,1e8);
    static final StringBuilder OUT=new StringBuilder();
    static Gradient constant(double x){return Gradient.constant(DIM,x);}
    static FieldVector3D<Gradient> variables(Vector3D v,int offset) {
        return new FieldVector3D<>(Gradient.variable(DIM,offset,v.getX()),
            Gradient.variable(DIM,offset+1,v.getY()),Gradient.variable(DIM,offset+2,v.getZ()));
    }
    static ExtendedPositionProvider body(Vector3D p) {
        return new ExtendedPositionProvider() {
            public Vector3D getPosition(AbsoluteDate date,Frame frame) {
                return FRAME.getStaticTransformTo(frame,date).transformPosition(p);
            }
            public <T extends CalculusFieldElement<T>> FieldVector3D<T> getPosition(FieldAbsoluteDate<T> date,Frame frame) {
                return FRAME.getStaticTransformTo(frame,date).transformPosition(new FieldVector3D<>(date.getField(),p));
            }
        };
    }
    static void number(double value) {
        if(!Double.isFinite(value))throw new IllegalStateException("nonfinite reference");
        OUT.append(String.format(Locale.ROOT," %.17g",value));
    }
    static void vector(Vector3D value){for(double x:value.toArray())number(x);}
    public static void main(String[] args)throws Exception {
        OUT.append("# Orekit 13.1.2 / Hipparchus 4.0.2 Gradient: combined gravity, relativity, Sun/Moon, unshadowed SRP, NRLMSISE00\n");
        OUT.append("# mask(1=relativity,2=Sun/Moon,4=SRP,8=drag) Cd Cr r[3] v[3] R[9] Rdot[9] Sun[3] SunV[3] Moon[3] a[3] Jr[9] Jv[9] dD[3] dK[3]\n");
        OUT.append("# TAI=synthetic UTC 2000-01-01 12:00; GM=3.986004415e14 R=6378136.3; D=Cd*.02 K=Cr*.03\n");
        double[][] c={{1},{0,0},{-4.84165371736e-4,3e-7,2e-6}},s={{0},{0,0},{0,-4e-7,-7e-7}};
        var harmonics=GravityFieldFactory.getNormalizedProvider(R,MU,TideSystem.TIDE_FREE,c,s);
        var parameters=new NRLMSISE00InputParameters() {
            public AbsoluteDate getMinDate(){return AbsoluteDate.PAST_INFINITY;}
            public AbsoluteDate getMaxDate(){return AbsoluteDate.FUTURE_INFINITY;}
            public double getDailyFlux(AbsoluteDate date){return 150;}
            public double getAverageFlux(AbsoluteDate date){return 150;}
            public double[] getAp(AbsoluteDate date){return new double[]{15,0,0,0,0,0,0};}
        };
        for(double angle:new double[]{-.7,.2,1.1}) {
            var rotation=new Rotation(new Vector3D(1,2,-1),angle,RotationConvention.FRAME_TRANSFORM);
            var transform=new Transform(DATE,rotation,new Vector3D(2e-5,-3e-5,7e-5));
            var fixed=new Frame(FRAME,transform,"synthetic-body");
            var ellipsoid=new OneAxisEllipsoid(6378137.,1/298.257223563,fixed);
            var atmosphere=new NRLMSISE00(parameters,body(SUN),ellipsoid,TimeScalesFactory.getTAI());
            var drag=new DragForce(atmosphere,new IsotropicDrag(1000,1),false);
            var central=new NewtonianAttraction(MU);
            var gravity=new HolmesFeatherstoneAttractionModel(fixed,harmonics);
            var schwarz=new Relativity(MU);
            var lt=new LenseThirringRelativity(MU,fixed);
            var ds=new DeSitterRelativity(OrbitForgeRelativityReference.body(true),OrbitForgeRelativityReference.body(false));
            var solarGravity=new ThirdBodyAttraction(body(SUN),"Sun",OrbitForgeRelativityReference.SUN_MU);
            var lunarGravity=new ThirdBodyAttraction(body(MOON),"Moon",MOON_MU);
            for(double height:new double[]{245000,750000})for(double latitude:new double[]{-.8,.6})
                for(double coefficient:new double[]{0,1,-1}) {
                    double cd=2.2*coefficient,cr=1.7*coefficient;
                    var r=transform.getInverse().transformPosition(ellipsoid.transform(new GeodeticPoint(latitude,1.2,height)));
                    var v=new Vector3D(1234,-6234,3456);
                    var p=variables(r,0);
                    var date=new FieldAbsoluteDate<>(constant(0).getField(),DATE);
                    var state=new FieldSpacecraftState<>(new FieldAbsolutePVCoordinates<>(FRAME,date,p,variables(v,3))).withMass(constant(1000));
                    var a0=central.acceleration(state,new Gradient[]{constant(MU)}).add(gravity.acceleration(state,new Gradient[]{constant(MU)}));
                    var a1=schwarz.acceleration(state,new Gradient[]{constant(MU)}).add(lt.acceleration(state,new Gradient[]{constant(MU)}))
                        .add(ds.acceleration(state,new Gradient[]{constant(OrbitForgeRelativityReference.SUN_MU)}));
                    var a2=solarGravity.acceleration(state,new Gradient[]{constant(OrbitForgeRelativityReference.SUN_MU)})
                        .add(lunarGravity.acceleration(state,new Gradient[]{constant(MOON_MU)}));
                    var ray=new FieldVector3D<>(date.getField(),SUN).subtract(p);
                    var flux=ray.scalarMultiply(ray.getNormSq().multiply(ray.getNorm()).reciprocal().multiply(-4.56e-6*149597870700.*149597870700.));
                    var a4=new IsotropicRadiationSingleCoefficient(1000,1).radiationPressureAcceleration(state,flux,
                        new Gradient[]{constant(1),Gradient.variable(DIM,7,cr*.03)});
                    var a8=drag.acceleration(state,new Gradient[]{constant(1),Gradient.variable(DIM,6,cd*.02)});
                    for(int mask:new int[]{0,1,2,4,8,15}) {
                        var force=a0;
                        if((mask&1)!=0)force=force.add(a1);
                        if((mask&2)!=0)force=force.add(a2);
                        if((mask&4)!=0)force=force.add(a4);
                        if((mask&8)!=0)force=force.add(a8);
                        OUT.append(mask);number(cd);number(cr);vector(r);vector(v);
                        double[][] jacobian=new double[6][6];
                        transform.getJacobian(CartesianDerivativesFilter.USE_PV,jacobian);
                        for(int block=0;block<2;block++)for(int i=0;i<3;i++)for(int j=0;j<3;j++)number(jacobian[3*block+i][j]);
                        vector(SUN);vector(OrbitForgeRelativityReference.sunVelocity(0));vector(MOON);
                        for(var x:force.toArray())number(x.getValue());
                        for(int block=0;block<2;block++)for(var x:force.toArray())for(int j=0;j<3;j++)number(x.getGradient()[3*block+j]);
                        for(int j=6;j<8;j++)for(var x:force.toArray())number(x.getGradient()[j]);
                        OUT.append('\n');
                    }
                }
        }
        Files.writeString(Path.of(args[0]),OUT.toString(),java.nio.charset.StandardCharsets.US_ASCII);
    }
}
