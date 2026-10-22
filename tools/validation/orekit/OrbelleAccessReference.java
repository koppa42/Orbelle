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
import org.orekit.time.*;
import org.orekit.utils.PVCoordinates;

// Independent reference: Orekit propagation/topocentric geometry/event detection;
// Hipparchus Brent continuous extrema. Deliberately static Earth axes isolate the
// access/search calculation from EOP differences (frames have their own protocol).
public class OrbelleAccessReference {
    static final double MU = 3.986004418e14, R = 6378137, A = 7e6;
    static final AbsoluteDate ZERO = new AbsoluteDate(2000, 1, 1, 12, 0, 0, TimeScalesFactory.getTAI());
    static final Frame FRAME = FramesFactory.getGCRF();
    static double[] extreme(UnivariateFunction f, double a, double b, boolean max) {
        double t = a, v = f.value(a);
        for (double left = a; left < b;) {
            double right = Math.min(left + 20, b);
            var opt = new BrentOptimizer(1e-14, 1e-9).optimize(new MaxEval(300),
                new UnivariateObjectiveFunction(f), max ? GoalType.MAXIMIZE : GoalType.MINIMIZE,
                new SearchInterval(left, right));
            for (double candidate : new double[]{left, opt.getPoint(), right}) {
                double value = f.value(candidate);
                if (max ? value > v : value < v) { t = candidate; v = value; }
            }
            left = right;
        }
        return new double[]{t, v};
    }
    static void run(String name, double first, double last, double vy, double vz,
                    double lon, double lat, double minEl, double cone, double check) {
        var orbit = new CartesianOrbit(new PVCoordinates(new Vector3D(A, 0, 0),
            new Vector3D(0, vy, vz)), FRAME, ZERO, MU);
        var propagator = new KeplerianPropagator(orbit);
        var earth = new OneAxisEllipsoid(R, 1 / 298.257223563, FRAME);
        var station = new TopocentricFrame(earth, new GeodeticPoint(lat, lon, 0), "station");
        var stationPosition = earth.transform(station.getPoint());
        java.util.function.ToDoubleFunction<org.orekit.propagation.SpacecraftState> margin = s -> {
            var p = s.getPosition();
            double result = station.getElevation(p, FRAME, s.getDate()) - minEl;
            if (cone > 0)
                result = Math.min(result, cone - Vector3D.angle(p.negate(), stationPosition.subtract(p)));
            return result;
        };
        boolean visible = margin.applyAsDouble(propagator.propagate(ZERO.shiftedBy(first))) >= 0;
        boolean clipped = visible;
        double start = first;
        var logger = new EventsLogger();
        propagator.addEventDetector(logger.monitorDetector(new FunctionalDetector()
            .withFunction(margin).withMaxCheck(check).withThreshold(1e-10)));
        propagator.propagate(ZERO.shiftedBy(first), ZERO.shiftedBy(last));
        var windows = new ArrayList<double[]>();
        for (var event : logger.getLoggedEvents()) {
            double t = event.getState().getDate().durationFrom(ZERO);
            if (event.isIncreasing()) { start = t; clipped = false; visible = true; }
            else { if (t > start && visible) windows.add(new double[]{start,t,clipped?1:0,0}); visible = false; }
        }
        if (visible && last > start) windows.add(new double[]{start,last,clipped?1:0,1});
        propagator.clearEventsDetectors();
        UnivariateFunction elevation = t -> station.getElevation(
            propagator.propagate(ZERO.shiftedBy(t)).getPosition(), FRAME, ZERO.shiftedBy(t));
        UnivariateFunction range = t -> station.getRange(
            propagator.propagate(ZERO.shiftedBy(t)).getPosition(), FRAME, ZERO.shiftedBy(t));
        System.out.println("C " + name + " " + first + " " + last + " " + vy + " " + vz +
            " " + lon + " " + lat + " " + minEl + " " + cone);
        double total = 0, shortest = Double.POSITIVE_INFINITY, longest = 0;
        for (var w : windows) {
            var el = extreme(elevation, w[0], w[1], true);
            var rg = extreme(range, w[0], w[1], false);
            System.out.println("P " + w[0] + " " + w[1] + " " + (int)w[2] + " " + (int)w[3] +
                " " + el[0] + " " + el[1] + " " + rg[0] + " " + rg[1]);
            total += w[1]-w[0]; shortest = Math.min(shortest,w[1]-w[0]); longest = Math.max(longest,w[1]-w[0]);
        }
        System.out.println("S " + windows.size() + " " + total + " " +
            (windows.isEmpty()?0:total/windows.size()) + " " + (windows.isEmpty()?0:shortest) + " " + longest);
    }
    public static void main(String[] args) {
        double speed = Math.sqrt(MU/A), rate = speed/A;
        run("equatorial",0,12000,speed,0,.23,0,0,0,1);
        run("inclined_eccentric",0,15000,6200,5300,.2,.3,.1,0,1);
        run("nadir",0,12000,speed,0,.23,0,0,.15,.5);
        run("short_cone",110,140,speed,0,rate*123.456789,0,0,.0001,.001);
        double lat = .25;
        var earth = new OneAxisEllipsoid(R, 1 / 298.257223563, FRAME);
        var point = new TopocentricFrame(earth, new GeodeticPoint(lat, 0, 0), "grazing");
        double peak = point.getElevation(new Vector3D(A,0,0), FRAME, ZERO);
        run("grazing",110,140,speed,0,rate*123.456789,lat,peak-1e-8,0,.001);
        run("clipped",0,30,speed,0,0,0,0,0,1);
        run("empty",0,600,speed,0,0,1.5,0,0,1);
    }
}
