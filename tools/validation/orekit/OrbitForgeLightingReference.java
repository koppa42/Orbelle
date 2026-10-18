// Independent reference: Orekit eclipse detection, illumination and Keplerian propagation.
import java.util.*;
import org.hipparchus.CalculusFieldElement;
import org.hipparchus.analysis.solvers.BrentSolver;
import org.hipparchus.geometry.euclidean.threed.*;
import org.orekit.bodies.OneAxisEllipsoid;
import org.orekit.forces.radiation.*;
import org.orekit.frames.*;
import org.orekit.orbits.*;
import org.orekit.propagation.*;
import org.orekit.propagation.analytical.KeplerianPropagator;
import org.orekit.propagation.events.*;
import org.orekit.propagation.events.handlers.ContinueOnEvent;
import org.orekit.time.*;
import org.orekit.utils.*;

public class OrbitForgeLightingReference {
    static final double MU=3.986004418e14, AU=149597870700., R=6378137., SUN=695700000.;
    static final AbsoluteDate ZERO=new AbsoluteDate(2000,1,1,12,0,0,TimeScalesFactory.getTAI());
    static final Frame FRAME=FramesFactory.getGCRF();
    static final OneAxisEllipsoid EARTH=new OneAxisEllipsoid(R,0,FRAME);
    static ExtendedPositionProvider sun(Vector3D position) {
        return new ExtendedPositionProvider() {
            public Vector3D getPosition(AbsoluteDate date,Frame frame) { return position; }
            public <T extends CalculusFieldElement<T>> FieldVector3D<T> getPosition(FieldAbsoluteDate<T> date,Frame frame) {
                return new FieldVector3D<>(date.getField(),position);
            }
        };
    }
    static Vector3D direction(double lon,double lat) {
        return new Vector3D(AU*Math.cos(lat)*Math.cos(lon),AU*Math.cos(lat)*Math.sin(lon),AU*Math.sin(lat));
    }
    static record Result(List<double[]> intervals,List<double[]> samples) {}
    static Result calculate(double first,double last,double[] initial,Vector3D sunPosition,double check) {
        var orbit=new CartesianOrbit(new PVCoordinates(new Vector3D(initial[0],initial[1],initial[2]),
            new Vector3D(initial[3],initial[4],initial[5])),FRAME,ZERO,MU);
        var p=new KeplerianPropagator(orbit);
        var source=sun(sunPosition);
        var outer=new EclipseDetector(source,SUN,EARTH).withPenumbra().withMaxCheck(check)
            .withThreshold(1e-10).withHandler(new ContinueOnEvent());
        var inner=new EclipseDetector(source,SUN,EARTH).withUmbra().withMaxCheck(check)
            .withThreshold(1e-10).withHandler(new ContinueOnEvent());
        var logger=new EventsLogger();
        p.addEventDetector(logger.monitorDetector(outer)); p.addEventDetector(logger.monitorDetector(inner));
        if(last>first) p.propagate(ZERO.shiftedBy(first),ZERO.shiftedBy(last));
        var boundaries=new TreeSet<Double>(); boundaries.add(first); boundaries.add(last);
        for(var event:logger.getLoggedEvents()) boundaries.add(event.getState().getDate().durationFrom(ZERO));
        p.clearEventsDetectors();
        var srp=new SolarRadiationPressure(source,EARTH,new IsotropicRadiationSingleCoefficient(1,1));
        var intervals=new ArrayList<double[]>(); var samples=new ArrayList<double[]>();
        var b=new ArrayList<>(boundaries);
        for(int i=1;i<b.size();i++) {
            double a=b.get(i-1), z=b.get(i), mid=(a+z)/2;
            var state=p.propagate(ZERO.shiftedBy(mid));
            int phase=outer.g(state)>=0?0:inner.g(state)<=0?2:1;
            intervals.add(new double[]{a,z,phase,i==1?1:0,i==b.size()-1?1:0});
            // Sample within each phase; this includes every penumbra, not just a coarse grid.
            for(double q:new double[]{.1,.5,.9}) {
                double t=Math.rint((a+(z-a)*q)*1e9)/1e9;
                var s=p.propagate(ZERO.shiftedBy(t));
                int ph=outer.g(s)>=0?0:inner.g(s)<=0?2:1;
                var pos=s.getPosition();
                samples.add(new double[]{t,srp.getLightingRatio(s),ph,pos.getX(),pos.getY(),pos.getZ()});
            }
        }
        for(double t:new double[]{first,last}) {
            var s=p.propagate(ZERO.shiftedBy(t));
            var pos=s.getPosition();
            samples.add(new double[]{t,srp.getLightingRatio(s),outer.g(s)>=0?0:inner.g(s)<=0?2:1,pos.getX(),pos.getY(),pos.getZ()});
        }
        return new Result(intervals,samples);
    }
    static void run(String name,double first,double last,double[] initial,Vector3D sun,double check) {
        var a=calculate(first,last,initial,sun,check); var b=calculate(first,last,initial,sun,check/2);
        if(a.intervals.size()!=b.intervals.size()) throw new AssertionError("Reference event count");
        for(int i=0;i<a.intervals.size();i++) for(int j=0;j<5;j++)
            if(Math.abs(a.intervals.get(i)[j]-b.intervals.get(i)[j])>1e-8)
                throw new AssertionError("Reference convergence");
        System.out.printf("C %s %.17g %.17g",name,first,last);
        for(double v:initial) System.out.printf(" %.17g",v);
        for(double v:sun.toArray()) System.out.printf(" %.17g",v);
        System.out.println();
        for(var v:b.intervals) System.out.printf("I %.17g %.17g %d %d %d%n",v[0],v[1],(int)v[2],(int)v[3],(int)v[4]);
        for(var v:b.samples) System.out.printf("V %.17g %.17g %d %.17g %.17g %.17g%n",v[0],v[1],(int)v[2],v[3],v[4],v[5]);
    }
    public static void main(String[] args) {
        Locale.setDefault(Locale.ROOT);
        System.out.println("# Orekit 13.1.2 / Hipparchus 4.0.2; EclipseDetector + SolarRadiationPressure + KeplerianPropagator");
        System.out.println("# SI; TAI seconds from 2000-01-01 12:00:00 TAI; GCRF; spherical Earth; geometric fixed Sun");
        System.out.println("# C name first last initial[6] sun[3]; I first last phase(0 sunlight,1 penumbra,2 umbra) clipped[2]; V time fraction phase position[3]");
        double r=7e6,speed=Math.sqrt(MU/r),rate=speed/r;
        double[] circular={r,0,0,0,speed,0};
        run("leo",0,12000,circular,direction(.23,.05),1);
        run("inclined_eccentric",0,15000,new double[]{r,0,0,0,6200,5300},direction(.23,.05),1);
        run("geo",0,90000,new double[]{42164000,0,0,0,Math.sqrt(MU/42164000),0},direction(.23,0),2);
        run("sunlight",0,1200,circular,direction(0,Math.PI/2),1);
        run("clipped_umbra",2800,2900,circular,direction(0,0),1);
        run("instant",123,123,circular,direction(0,0),1);
        run("annular",0,600,new double[]{-2e9,0,0,0,-Math.sqrt(MU/2e9),0},direction(1e-5,0),1);
        double centre=123.456789,lon=rate*centre+Math.PI;
        var orbit=new CartesianOrbit(new PVCoordinates(new Vector3D(r,0,0),new Vector3D(0,speed,0)),FRAME,ZERO,MU);
        var at=new KeplerianPropagator(orbit).propagate(ZERO.shiftedBy(centre));
        for(boolean umbra:new boolean[]{false,true}) {
            double beta=new BrentSolver(1e-14).solve(100,lat -> {
                var d=new EclipseDetector(sun(direction(lon,lat)),SUN,EARTH);
                return (umbra?d.withUmbra():d.withPenumbra()).g(at);
            },.8,1.3);
            run(umbra?"short_umbra":"grazing",110,140,circular,direction(lon,beta-1e-8),.001);
        }
    }
}
