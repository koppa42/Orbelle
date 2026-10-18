// Independent reference: Orekit event detectors and Keplerian propagation only.
import java.util.*;
import org.hipparchus.geometry.euclidean.threed.Vector3D;
import org.orekit.frames.*;
import org.orekit.orbits.*;
import org.orekit.propagation.analytical.KeplerianPropagator;
import org.orekit.propagation.events.*;
import org.orekit.propagation.events.handlers.ContinueOnEvent;
import org.orekit.time.*;
import org.orekit.utils.PVCoordinates;

public class OrbitForgeOrbitalEventsReference {
    static final double MU=3.986004418e14;
    static final AbsoluteDate ZERO=new AbsoluteDate(2000,1,1,12,0,0,TimeScalesFactory.getTAI());
    static final Frame FRAME=FramesFactory.getGCRF();
    static List<double[]> calculate(Orbit orbit, double first, double last, double check) {
        var p=new KeplerianPropagator(orbit);
        var log=new EventsLogger();
        var apsis=new ApsideDetector(orbit).withMaxCheck(check).withThreshold(1e-11).withHandler(new ContinueOnEvent());
        var node=new NodeDetector(FRAME).withMaxCheck(check).withThreshold(1e-11).withHandler(new ContinueOnEvent());
        if(orbit.getE()>1e-8) p.addEventDetector(log.monitorDetector(apsis));
        if(Math.abs(Math.sin(orbit.getI()))>1e-8) p.addEventDetector(log.monitorDetector(node));
        var result=new ArrayList<double[]>();
        if(first==last) return result;
        p.propagate(ZERO.shiftedBy(first-1),ZERO.shiftedBy(last+1));
        for(var event:log.getLoggedEvents()) {
            var s=event.getState();
            double t=s.getDate().durationFrom(ZERO);
            if(t<first-1e-8 || t>last+1e-8) continue;
            t=Math.max(first,Math.min(last,t));
            int kind=event.getEventDetector()==apsis?(event.isIncreasing()?0:1):(event.isIncreasing()?2:3);
            var pv=s.getPVCoordinates();
            result.add(new double[]{t,kind,pv.getPosition().getNorm(),pv.getVelocity().getNorm()});
        }
        result.sort(Comparator.<double[]>comparingDouble(x->x[0]).thenComparingDouble(x->x[1]));
        return result;
    }
    static void run(String name, Orbit input, double first, double last) {
        // Serialize Cartesian input once, then construct both reference propagators from it.
        var orbit=new CartesianOrbit(input.getPVCoordinates(),FRAME,ZERO,MU);
        var a=calculate(orbit,first,last,15); var b=calculate(orbit,first,last,7.5);
        if(a.size()!=b.size()) throw new AssertionError("Reference count: "+name);
        for(int j=0;j<a.size();++j) {
            if(a.get(j)[1]!=b.get(j)[1] || Math.abs(a.get(j)[0]-b.get(j)[0])>2e-8)
                throw new AssertionError("Reference convergence: "+name);
        }
        var pv=orbit.getPVCoordinates(); var r=pv.getPosition(); var v=pv.getVelocity();
        System.out.printf(Locale.ROOT,"C %s %.17g %.17g",name,first,last);
        for(double x:new double[]{r.getX(),r.getY(),r.getZ(),v.getX(),v.getY(),v.getZ()}) System.out.printf(Locale.ROOT," %.17g",x);
        System.out.printf(Locale.ROOT," %d %d%n",orbit.getE()<=1e-8?1:0,Math.abs(Math.sin(orbit.getI()))<=1e-8?1:0);
        for(var event:a) {
            System.out.print("E");
            for(double x:event) System.out.printf(Locale.ROOT," %.17g",x);
            System.out.println();
        }
    }
    static Orbit orbit(double a,double e,double i,double argp,double raan,double anomaly) {
        return new KeplerianOrbit(a,e,i,argp,raan,anomaly,PositionAngleType.TRUE,FRAME,ZERO,MU);
    }
    public static void main(String[] args) {
        System.out.println("# Orekit 13.1.2 / Hipparchus 4.0.2; ApsideDetector + NodeDetector + KeplerianPropagator");
        System.out.println("# Earth/GCRF/SI; TAI seconds since 2000-01-01 12:00:00 TAI; mu=3.986004418e14");
        System.out.println("# C name first last initial[6] near_circular near_equatorial; E time kind(0 peri,1 apo,2 ascending,3 descending) radius speed");
        run("leo",orbit(7200000,.04,.8,.4,.7,.3),0,18000);
        run("geo",orbit(42164000,.1,.3,.8,1.1,2),0,100000);
        run("molniya",orbit(26600000,.72,1.107,.7,2,.6),0,90000);
        run("retrograde",orbit(9500000,.2,2.4,.6,2.2,1.1),-10000,17000);
        run("near_circular",orbit(7200000,1e-10,.7,.4,.6,1),0,12000);
        run("equatorial",orbit(9500000,.2,0,.6,0,1.1),0,20000);
        run("circular_equatorial",orbit(7000000,0,0,0,0,.5),0,12000);
        run("near_retrograde_equator",orbit(9500000,.2,Math.PI-1e-10,.6,0,1.1),0,20000);
        run("partial",orbit(7200000,.04,.8,.4,.7,.3),100,200);
        run("instant",orbit(7200000,.04,.8,.4,.7,.3),123,123);
        run("hyperbolic",orbit(-10000000,1.8,1.2,.3,.9,-.8),-1000,10000);
        run("boundary",new CartesianOrbit(new PVCoordinates(new Vector3D(7000000,0,0),new Vector3D(0,8000,2000)),FRAME,ZERO,MU),0,12000);
    }
}
