import java.util.*;
import org.hipparchus.analysis.integration.IterativeLegendreGaussIntegrator;
import org.hipparchus.geometry.euclidean.threed.Vector3D;
import org.orekit.bodies.*;
import org.orekit.frames.*;
import org.orekit.orbits.*;
import org.orekit.propagation.analytical.KeplerianPropagator;
import org.orekit.propagation.events.*;
import org.orekit.time.*;
import org.orekit.utils.PVCoordinates;

// Deliberately nonrotating WGS84: isolates union, constraints and event search.
// Earth rotation/EOP is independently exercised by the existing ERFA/frame suite.
public class OrbitForgeCoverageReference {
    static final double MU=3.986004418e14, R=6378137, F=1/298.257223563, END=12000;
    static final AbsoluteDate ZERO=new AbsoluteDate(2000,1,1,12,0,0,TimeScalesFactory.getTAI());
    static final Frame FRAME=FramesFactory.getGCRF();
    static final OneAxisEllipsoid EARTH=new OneAxisEllipsoid(R,F,FRAME);
    static final Vector3D ANCHOR=EARTH.transform(new GeodeticPoint(.1,.2,0));
    static ArrayList<double[]> windows(TopocentricFrame point, int mode, int asset, double elevation) {
        double angle=asset==0 ? 0 : 1, c=Math.cos(angle), s=Math.sin(angle);
        var orbit=new CartesianOrbit(new PVCoordinates(new Vector3D(7e6*c,7e6*s,0),
            new Vector3D(-6200*s,6200*c,5300)),FRAME,ZERO,MU);
        var prop=new KeplerianPropagator(orbit);
        var target=EARTH.transform(point.getPoint());
        java.util.function.ToDoubleFunction<org.orekit.propagation.SpacecraftState> margin=state->{
            var pos=state.getPosition();
            double value=point.getElevation(pos,FRAME,state.getDate())-elevation;
            if(mode>0) value=Math.min(value,.3-Vector3D.angle(mode==1 ? pos.negate() : ANCHOR.subtract(pos),target.subtract(pos)));
            return value;
        };
        boolean visible=margin.applyAsDouble(prop.propagate(ZERO))>=0;
        double start=0;
        var logger=new EventsLogger();
        prop.addEventDetector(logger.monitorDetector(new FunctionalDetector().withFunction(margin).withMaxCheck(1).withThreshold(1e-10)));
        prop.propagate(ZERO,ZERO.shiftedBy(END));
        var intervals=new ArrayList<double[]>();
        for(var event:logger.getLoggedEvents()) {
            double t=event.getState().getDate().durationFrom(ZERO);
            if(event.isIncreasing()) { start=t; visible=true; }
            else { if(visible && t>start) intervals.add(new double[]{start,t}); visible=false; }
        }
        if(visible) intervals.add(new double[]{start,END});
        return intervals;
    }
    static void run(String name,double south,double north,double west,double east,double resolution,double elevation,int mode) {
        south=Math.toRadians(south);north=Math.toRadians(north);west=Math.toRadians(west);east=Math.toRadians(east);
        resolution=Math.toRadians(resolution);elevation=Math.toRadians(elevation);
        System.out.printf(Locale.ROOT,"C %s %.17g %.17g %.17g %.17g %.17g %.17g %d%n",name,south,north,west,east,resolution,elevation,mode);
        double span=east-west; if(span<=0) span+=2*Math.PI;
        int bands=(int)Math.ceil((north-south)/resolution),index=0;
        for(int b=0;b<bands;++b) {
            double lo=south+(north-south)*b/bands,hi=south+(north-south)*(b+1)/bands,lat=(lo+hi)/2;
            int columns=Math.max(1,(int)Math.ceil(span*Math.cos(lat)/resolution));
            double e2=F*(2-F);
            var integrator=new IterativeLegendreGaussIntegrator(8,1e-13,.001);
            double area=integrator.integrate(100000,x->R*R*(1-e2)*Math.cos(x)/Math.pow(1-e2*Math.sin(x)*Math.sin(x),2),lo,hi)*span/columns;
            for(int col=0;col<columns;++col) {
                double lon=Math.IEEEremainder(west+span*(col+.5)/columns,2*Math.PI);
                var point=new TopocentricFrame(EARTH,new GeodeticPoint(lat,lon,0),"cell");
                System.out.printf(Locale.ROOT,"P %d %.17g %.17g %.17g%n",index++,lon,lat,area);
                var all=new ArrayList<double[]>();
                for(int asset=0;asset<2;++asset) all.addAll(windows(point,mode,asset,elevation));
                // Independent event sweep: active-count transitions, not pairwise interval merging.
                var events=new TreeMap<Double,Integer>();
                for(var w:all) { events.merge(w[0],1,Integer::sum); events.merge(w[1],-1,Integer::sum); }
                int active=0;double start=0;var union=new ArrayList<double[]>();
                for(var event:events.entrySet()) {
                    int next=active+event.getValue();
                    if(active==0 && next>0) start=event.getKey();
                    if(active>0 && next==0) union.add(new double[]{start,event.getKey()});
                    active=next;
                }
                double covered=0;
                var gaps=new ArrayList<Double>(); var internal=new ArrayList<Double>();
                double cursor=0;
                for(int j=0;j<union.size();++j) {
                    var w=union.get(j); double gap=w[0]-cursor;
                    if(gap>0) { gaps.add(gap); if(j>0) internal.add(gap); }
                    covered+=w[1]-w[0];cursor=w[1];
                    System.out.printf(Locale.ROOT,"W %.17g %.17g%n",w[0],w[1]);
                }
                if(cursor<END) gaps.add(END-cursor);
                if(union.isEmpty()) internal.add(END);
                System.out.printf(Locale.ROOT,"S %.17g %.17g %.17g %.17g %.17g%n",covered,
                    gaps.stream().mapToDouble(Double::doubleValue).max().orElse(0),gaps.stream().mapToDouble(Double::doubleValue).average().orElse(0),
                    internal.stream().mapToDouble(Double::doubleValue).max().orElse(0),internal.stream().mapToDouble(Double::doubleValue).average().orElse(0));
            }
        }
    }
    public static void main(String[] args) {
        run("global",-90,90,-180,180,60,0,0);
        run("regional",-30,60,-40,100,30,10,0);
        run("dateline",-30,60,170,-140,30,0,0);
        run("nadir",-10,50,-10,90,20,0,1);
        run("tracking",-10,50,-10,90,20,0,2);
    }
}
