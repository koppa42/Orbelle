// Independent full-coast derivatives: Orekit 13.1.2 force models,
// Hipparchus 4.0.2 Gradient field ODE and Dormand-Prince 8(5,3).
import java.nio.file.*;
import java.util.*;
import org.hipparchus.CalculusFieldElement;
import org.hipparchus.analysis.differentiation.Gradient;
import org.hipparchus.geometry.euclidean.threed.*;
import org.hipparchus.ode.*;
import org.hipparchus.ode.nonstiff.DormandPrince853FieldIntegrator;
import org.hipparchus.ode.nonstiff.DormandPrince853Integrator;
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

public final class OrbelleHpopVariationalReference {
    static final int DIM=8;
    static final double MU=3.986004415e14, R=6378136.3, OMEGA=7.292115e-5;
    static final Frame FRAME=FramesFactory.getGCRF();
    static final AbsoluteDate DATE=OrbelleRelativityReference.EPOCH;
    static final StringBuilder OUT=new StringBuilder();
    static Gradient c(double x){return Gradient.constant(DIM,x);}
    static final ExtendedPositionProvider SUN=new ExtendedPositionProvider(){
        public Vector3D getPosition(AbsoluteDate date,Frame frame){
            return FRAME.getStaticTransformTo(frame,date).transformPosition(
                OrbelleRelativityReference.sunPosition(date.durationFrom(DATE)));
        }
        public <T extends CalculusFieldElement<T>> FieldVector3D<T> getPosition(FieldAbsoluteDate<T> date,Frame frame){
            T a=date.durationFrom(DATE).multiply(OrbelleRelativityReference.RATE).add(.3);
            var p=new FieldVector3D<>(a.cos().multiply(OrbelleRelativityReference.R),
                a.sin().multiply(OrbelleRelativityReference.R*Math.cos(OrbelleRelativityReference.TILT)),
                a.sin().multiply(OrbelleRelativityReference.R*Math.sin(OrbelleRelativityReference.TILT)));
            return FRAME.getStaticTransformTo(frame,date).transformPosition(p);
        }
    };
    static final Frame BODY=new Frame(FRAME,new TransformProvider(){
        public Transform getTransform(AbsoluteDate date){
            return new Transform(date,new Rotation(Vector3D.PLUS_K,.3+OMEGA*date.durationFrom(DATE),
                RotationConvention.FRAME_TRANSFORM),new Vector3D(0,0,OMEGA));
        }
        public <T extends CalculusFieldElement<T>> FieldTransform<T> getTransform(FieldAbsoluteDate<T> date){
            return new FieldTransform<>(date.getField(),getTransform(date.toAbsoluteDate()));
        }
    },"synthetic-rotating-body");
    static HolmesFeatherstoneAttractionModel gravity(int order){
        double[][] cn={{0},{0,0},{-4.84165371736e-4,3e-7,2e-6}};
        double[][] sn={{0},{0,0},{0,-4e-7,-7e-7}};
        if(order==0){cn=new double[][]{{0},{0},{-4.84165371736e-4}};sn=new double[][]{{0},{0},{0}};}
        return new HolmesFeatherstoneAttractionModel(BODY,GravityFieldFactory.getNormalizedProvider(
            R,MU,TideSystem.TIDE_FREE,cn,sn));
    }
    static final NewtonianAttraction CENTRAL=new NewtonianAttraction(MU);
    static final HolmesFeatherstoneAttractionModel FULL=gravity(2), ZONAL=gravity(0);
    static final Relativity SCHWARZ=new Relativity(MU);
    static final LenseThirringRelativity LT=new LenseThirringRelativity(MU,BODY);
    static final DeSitterRelativity DS=new DeSitterRelativity(
        OrbelleRelativityReference.body(true),OrbelleRelativityReference.body(false));
    static final ThirdBodyAttraction SOLAR=new ThirdBodyAttraction(SUN,"Sun",OrbelleRelativityReference.SUN_MU);
    static final ThirdBodyAttraction LUNAR=new ThirdBodyAttraction(
        OrbelleHpopForceReference.body(new Vector3D(3e8,-2e8,1e8)),"Moon",4.902800118e12);
    static DragForce drag(){
        var parameters=new NRLMSISE00InputParameters(){
            public AbsoluteDate getMinDate(){return DATE.shiftedBy(-1e8);}
            public AbsoluteDate getMaxDate(){return DATE.shiftedBy(1e8);}
            public double getDailyFlux(AbsoluteDate date){return 150;}
            public double getAverageFlux(AbsoluteDate date){return 150;}
            public double[] getAp(AbsoluteDate date){return new double[]{15,15,15,15,15,15,15};}
        };
        var earth=new OneAxisEllipsoid(6378137,1/298.257223563,BODY);
        return new DragForce(new NRLMSISE00(parameters,SUN,earth,TimeScalesFactory.getTAI()),
            new IsotropicDrag(1000,1),false);
    }
    static final DragForce DRAG=drag();
    // A sphere has a repeated plane-section eigendirection. Hold a canonical
    // basis fixed for this local field evaluation, retaining all Cartesian
    // derivatives. Scalar lighting is invariant; no force formula is replaced.
    static Gradient lighting(FieldSpacecraftState<Gradient> state){
        var sun=SUN.getPosition(state.getDate(),FRAME);var r=state.getPosition();
        var ex=sun.toVector3D().normalize();var ez=Vector3D.crossProduct(ex,r.toVector3D()).normalize();
        var ey=Vector3D.crossProduct(ez,ex);var z=r.dotProduct(ez);
        var p=new FieldVector3D<>(r.dotProduct(ex),r.dotProduct(ey),new Gradient(0,z.getGradient()));
        var local=new FieldSpacecraftState<>(new FieldAbsolutePVCoordinates<>(FRAME,state.getDate(),p,
            FieldVector3D.getZero(c(0).getField()))).withMass(c(1000));
        var force=new SolarRadiationPressure(149597870700.,4.56e-6,
            OrbelleHpopForceReference.body(new Vector3D(sun.getNorm().getReal(),0,0)),
            new OneAxisEllipsoid(6378137,0,FRAME),new IsotropicRadiationSingleCoefficient(1000,1));
        return force.getLightingRatio(local);
    }
    static double variableScale(int i){return i<3?1e6:(i<6?1e3:1);}
    static Gradient[] unpack(double[] y){
        Gradient[] result=new Gradient[6];
        for(int i=0;i<6;i++){
            double[] gradient=new double[8];
            for(int j=0;j<8;j++)gradient[j]=y[6+8*i+j]*variableScale(i)/variableScale(j);
            result[i]=new Gradient(y[i],gradient);
        }
        return result;
    }
    // Expose the complete field derivatives to the unmodified DP853 controller.
    // STM and much smaller parameter columns need separate absolute scales.
    static Gradient[] controlledField(FieldOrdinaryDifferentialEquation<Gradient> field,double[] initial,
                                     double seconds,double maxStep){
        double[] packed=new double[54],absolute=new double[54],relative=new double[54];
        for(int i=0;i<6;i++){
            packed[i]=initial[i];packed[6+8*i+i]=1;absolute[i]=i<3?1e-9:1e-12;
        }
        Arrays.fill(relative,2e-15);
        for(int i=0;i<6;i++)for(int j=0;j<8;j++)absolute[6+8*i+j]=j<6?1e-14:1e-19;
        var ode=new OrdinaryDifferentialEquation(){
            public int getDimension(){return 54;}
            public double[] computeDerivatives(double t,double[] y){
                var rate=field.computeDerivatives(c(t),unpack(y));double[] result=new double[54];
                for(int i=0;i<6;i++){
                    result[i]=rate[i].getValue();
                    for(int j=0;j<8;j++)result[6+8*i+j]=rate[i].getGradient()[j]*variableScale(j)/variableScale(i);
                }
                return result;
            }
        };
        return unpack(new DormandPrince853Integrator(1e-9,maxStep,absolute,relative).integrate(
            new ExpandableODE(ode),new ODEState(0,packed),seconds).getPrimaryState());
    }
    static Gradient[] propagate(int mask,double coefficient,int order,double seconds,double maxStep,double[] initial){

        var equation=new FieldOrdinaryDifferentialEquation<Gradient>(){
            public int getDimension(){return 6;}
            public Gradient[] computeDerivatives(Gradient t,Gradient[] y){
                var date=new FieldAbsoluteDate<>(DATE,t);
                var p=new FieldVector3D<>(y[0],y[1],y[2]);
                var v=new FieldVector3D<>(y[3],y[4],y[5]);
                var state=new FieldSpacecraftState<>(new FieldAbsolutePVCoordinates<>(FRAME,date,p,v)).withMass(c(1000));
                var nominal=FULL.acceleration(state,new Gradient[]{c(MU)});
                if(order==0){
                    var partial=ZONAL.acceleration(state,new Gradient[]{c(MU)});
                    nominal=new FieldVector3D<>(new Gradient(nominal.getX().getReal(),partial.getX().getGradient()),
                        new Gradient(nominal.getY().getReal(),partial.getY().getGradient()),
                        new Gradient(nominal.getZ().getReal(),partial.getZ().getGradient()));
                }
                var a=CENTRAL.acceleration(state,new Gradient[]{c(MU)}).add(nominal);
                if((mask&1)!=0)a=a.add(SCHWARZ.acceleration(state,new Gradient[]{c(MU)}))
                    .add(LT.acceleration(state,new Gradient[]{c(MU)}))
                    .add(DS.acceleration(state,new Gradient[]{c(OrbelleRelativityReference.SUN_MU)}));
                if((mask&2)!=0)a=a.add(SOLAR.acceleration(state,new Gradient[]{c(OrbelleRelativityReference.SUN_MU)}))
                    .add(LUNAR.acceleration(state,new Gradient[]{c(4.902800118e12)}));
                if((mask&4)!=0){
                    var ray=SUN.getPosition(date,FRAME).subtract(p);
                    var flux=ray.scalarMultiply(ray.getNormSq().multiply(ray.getNorm()).reciprocal()
                        .multiply(-4.56e-6*149597870700.*149597870700.));
                    if((mask&16)!=0)flux=flux.scalarMultiply(lighting(state));
                    a=a.add(new IsotropicRadiationSingleCoefficient(1000,1).radiationPressureAcceleration(
                        state,flux,new Gradient[]{c(1),Gradient.variable(DIM,7,1.7*coefficient*.03)}));
                }
                if((mask&8)!=0)a=a.add(DRAG.acceleration(state,
                    new Gradient[]{c(1),Gradient.variable(DIM,6,2.2*coefficient*.02)}));
                return new Gradient[]{y[3],y[4],y[5],a.getX(),a.getY(),a.getZ()};
            }
        };
        if((mask&16)!=0)return controlledField(equation,initial,seconds,maxStep);
        Gradient[] y=new Gradient[6];for(int i=0;i<6;i++)y[i]=Gradient.variable(DIM,i,initial[i]);
        var integrator=new DormandPrince853FieldIntegrator<Gradient>(c(0).getField(),1e-7,maxStep,
            new double[]{1e-8,1e-8,1e-8,1e-11,1e-11,1e-11},
            new double[]{2e-14,2e-14,2e-14,2e-14,2e-14,2e-14});
        return integrator.integrate(new FieldExpandableODE<>(equation),new FieldODEState<>(c(0),y),c(seconds)).getPrimaryState();
    }
    static void number(double x){if(!Double.isFinite(x))throw new IllegalStateException("nonfinite");OUT.append(String.format(Locale.ROOT," %.17g",x));}
    public static void main(String[] args)throws Exception{
        OUT.append("# Orekit 13.1.2 / Hipparchus 4.0.2 Gradient field DP853; physical fixed-TAI endpoint STM and D/K derivatives\n");
        OUT.append("# Shadow mask 16: canonical-plane Orekit field lighting; controlled STM and D/K columns; 5/2.5 s ceilings\n");
        OUT.append("# id mask coefficient covariance_order dt initial[6] state[6] Phi[36] D[6] K[6]\n");
        var earth=new OneAxisEllipsoid(6378137,1/298.257223563,BODY);
        var fixed=earth.transform(new GeodeticPoint(.6,1.2,750000));
        var r=BODY.getStaticTransformTo(FRAME,DATE).transformPosition(fixed);
        var vf=new Vector3D(-Math.sin(1.2),Math.cos(1.2),0).scalarMultiply(Math.sqrt(MU/r.getNorm()));
        var v=BODY.getStaticTransformTo(FRAME,DATE).transformVector(vf);
        double[] initial={r.getX(),r.getY(),r.getZ(),v.getX(),v.getY(),v.getZ()};
        int id=0;double maxStm=0,maxParameter=0,maxPosition=0;
        for(int mask:new int[]{0,1,2,4,8,15,20})for(double coefficient:new double[]{1,0,-1}){
            if(mask<4&&coefficient!=1)continue;
            if(args.length>1 && args[1].equals("--zero-shadow") && (mask!=20 || coefficient!=0))continue;
            for(int order:new int[]{2,0}){
                if(order==0&&mask!=15)continue;
                for(double dt:new double[]{mask==20?-6000:-1200,mask==20?6000:1200}){
                    var a=propagate(mask,coefficient,order,dt,mask==20?5:20,initial);
                    var b=propagate(mask,coefficient,order,dt,mask==20?2.5:10,initial);
                    for(int i=0;i<6;i++){
                        double rowScale=i<3?1e6:1e3;
                        double pos=Math.abs(a[i].getValue()-b[i].getValue());
                        maxPosition=Math.max(maxPosition,pos*(i<3?1:1000));
                        if(pos>(i<3?2e-6:2e-9))throw new IllegalStateException("reference state convergence "+id+" ["+i+"] "+pos);
                        for(int j=0;j<8;j++){
                            double error=Math.abs(a[i].getGradient()[j]-b[i].getGradient()[j]);
                            if(j<6){error*= (j<3?1e6:1e3)/rowScale;maxStm=Math.max(maxStm,error);
                                if(error>(mask==20?5e-11:2e-11))throw new IllegalStateException("reference STM convergence "+id+" ["+i+","+j+"] "+error);}

                        }
                    }
                    // Compare parameter columns in one physical state norm, matching
                    // the consumer test: velocity is weighted by 1000 s. Individual
                    // components can cross zero; their relative errors are ill-conditioned.
                    for(int j=6;j<8;j++){
                        double error=0,norm=1e-7;
                        for(int i=0;i<6;i++){
                            double unit=i<3?1:1000;
                            error=Math.max(error,Math.abs(a[i].getGradient()[j]-b[i].getGradient()[j])*unit);
                            norm=Math.max(norm,Math.abs(b[i].getGradient()[j])*unit);
                        }
                        maxParameter=Math.max(maxParameter,error/norm);
                        if(error/norm>2e-8)throw new IllegalStateException("reference parameter column convergence "+id+" "+error/norm);
                    }
                    OUT.append(id++).append(' ').append(mask);number(coefficient);OUT.append(' ').append(order);number(dt);
                    for(double x:initial)number(x);for(var x:b)number(x.getValue());
                    for(var x:b)for(int j=0;j<6;j++)number(x.getGradient()[j]);
                    for(int j=6;j<8;j++)for(var x:b)number(x.getGradient()[j]);OUT.append('\n');
                }
            }
        }
        Files.writeString(Path.of(args[0]),OUT.toString(),java.nio.charset.StandardCharsets.US_ASCII);
        System.out.printf(Locale.ROOT,"cases=%d state_convergence_m=%.17g scaled_stm=%.17g parameter_relative=%.17g%n",id,maxPosition,maxStm,maxParameter);
    }
}
