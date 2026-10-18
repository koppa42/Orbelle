import java.util.*;
import org.hipparchus.analysis.UnivariateFunction;
import org.hipparchus.geometry.euclidean.threed.Vector3D;
import org.hipparchus.optim.MaxEval;
import org.hipparchus.optim.nonlinear.scalar.GoalType;
import org.hipparchus.optim.univariate.*;
import org.orekit.bodies.*;
import org.orekit.frames.*;
import org.orekit.orbits.*;
import org.orekit.propagation.analytical.KeplerianPropagator;
import org.orekit.propagation.events.*;
import org.orekit.propagation.events.handlers.ContinueOnEvent;
import org.orekit.time.*;
import org.orekit.utils.PVCoordinates;

// Static Earth axes isolate geometry/search from separately validated IERS transforms.
// Orekit propagation, QSW frame, ellipsoidal direct-view detector; Hipparchus Brent extrema.
public class OrbitForgeIntersatelliteReference {
    static final double MU = 3.986004418e14;
    static final AbsoluteDate ZERO = new AbsoluteDate(2000,1,1,12,0,0,TimeScalesFactory.getTAI());
    static final Frame FRAME = FramesFactory.getGCRF();
    static KeplerianPropagator orbit(double a, double e, double i, double node, double anomaly) {
        return new KeplerianPropagator(new KeplerianOrbit(a,e,i,.35,node,anomaly,PositionAngleType.TRUE,FRAME,ZERO,MU));
    }
    static double[] extreme(UnivariateFunction f, double a, double b, boolean max) {
        double t=a, v=f.value(a);
        for(double left=a; left<b;) {
            double right=Math.min(left+20,b);
            var opt=new BrentOptimizer(1e-14,1e-9).optimize(new MaxEval(300),new UnivariateObjectiveFunction(f),
                max?GoalType.MAXIMIZE:GoalType.MINIMIZE,new SearchInterval(left,right));
            for(double c:new double[]{left,opt.getPoint(),right}) {
                double value=f.value(c);
                if(max?value>v:value<v) { t=c; v=value; }
            }
            left=right;
        }
        return new double[]{t,v};
    }
    static void pv(PVCoordinates s) {
        var p=s.getPosition(); var v=s.getVelocity();
        System.out.print(" "+p.getX()+" "+p.getY()+" "+p.getZ()+" "+v.getX()+" "+v.getY()+" "+v.getZ());
    }
    static void run(String name, KeplerianPropagator a, KeplerianPropagator b, double first, double last, boolean flat) {
        var earth=new OneAxisEllipsoid(6378137,1/298.257223563,FRAME);
        var detector=new InterSatDirectViewDetector(earth,b).withMaxCheck(5).withThreshold(1e-10).withHandler(new ContinueOnEvent());
        System.out.print("C "+name+" "+first+" "+last+" "+(flat?1:0));
        pv(a.getPVCoordinates(ZERO,FRAME)); pv(b.getPVCoordinates(ZERO,FRAME)); System.out.println();
        for(double t=first;;t=Math.min(t+137,last)) {
            var date=ZERO.shiftedBy(t); var sa=a.propagate(date); var pa=sa.getPVCoordinates(); var pb=b.getPVCoordinates(date,FRAME);
            var dr=pb.getPosition().subtract(pa.getPosition()); var dv=pb.getVelocity().subtract(pa.getVelocity());
            var ric=LOFType.QSW.rotationFromInertial(pa).applyTo(dr);
            System.out.println("S "+t+" "+dr.getNorm()+" "+Vector3D.dotProduct(dr.normalize(),dv)+" "+dv.getNorm()+
                " "+ric.getX()+" "+ric.getY()+" "+ric.getZ()+" "+(detector.g(sa)>=0?1:0));
            if(t==last) break;
        }
        boolean visible=detector.g(a.propagate(ZERO.shiftedBy(first)))>=0, clipped=visible;
        double start=first;
        var logger=new EventsLogger(); a.addEventDetector(logger.monitorDetector(detector));
        if(last>first) a.propagate(ZERO.shiftedBy(first),ZERO.shiftedBy(last));
        var windows=new ArrayList<double[]>();
        for(var event:logger.getLoggedEvents()) {
            double t=event.getState().getDate().durationFrom(ZERO);
            if(event.isIncreasing()) { start=t; clipped=false; visible=true; }
            else { if(t>start && visible) windows.add(new double[]{start,t,clipped?1:0,0}); visible=false; }
        }
        if(visible && last>start) windows.add(new double[]{start,last,clipped?1:0,1});
        a.clearEventsDetectors();
        UnivariateFunction range=t->Vector3D.distance(a.getPosition(ZERO.shiftedBy(t),FRAME),b.getPosition(ZERO.shiftedBy(t),FRAME));
        for(var w:windows) {
            var m=extreme(range,w[0],w[1],false);
            System.out.println("W "+w[0]+" "+w[1]+" "+w[2]+" "+w[3]+" "+m[0]+" "+m[1]);
        }
        var min=extreme(range,first,last,false); var max=extreme(range,first,last,true);
        System.out.println("G "+min[0]+" "+min[1]+" "+max[0]+" "+max[1]);
    }
    public static void main(String[] args) {
        run("coplanar",orbit(7e6,.02,0,0,0),orbit(7.7e6,.08,0,0,1),0,18000,false);
        run("inclined",orbit(7e6,.02,.6,.2,0),orbit(8e6,.1,1.2,.8,1),0,18000,false);
        run("retrograde",orbit(7e6,.02,.6,.2,0),orbit(7.5e6,.05,2.5,.8,1),0,12000,false);
        run("leo_geo",orbit(7e6,.01,.9,0,0),orbit(42164e3,.001,.01,.2,2),0,12000,false);
        run("all_visible",orbit(7e6,0,0,0,0),orbit(7e6,0,0,0,.1),0,600,true);
        run("all_occulted",orbit(7e6,0,0,0,0),orbit(7e6,0,0,0,3),0,600,true);
        run("polar",orbit(7e6,.01,Math.PI/2,0,0),orbit(8e6,.03,Math.PI/2,.2,2),0,12000,false);
        run("endpoint",orbit(7e6,.02,.6,.2,0),orbit(8e6,.1,1.2,.8,1),230,260,false);
        run("instant",orbit(7e6,.02,.6,.2,0),orbit(8e6,.1,1.2,.8,1),333,333,false);
    }
}
