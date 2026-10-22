// Independent Orekit KeplerianPropagator. No Earth-orientation or ephemeris files.
// CSV: case,mu,initial_tai_ns,elapsed_ns,rx,ry,rz,vx,vy,vz -> case,elapsed_ns,r,v.
import java.io.BufferedReader;
import java.io.InputStreamReader;
import java.util.Locale;
import java.util.concurrent.TimeUnit;
import org.hipparchus.geometry.euclidean.threed.Vector3D;
import org.orekit.frames.FramesFactory;
import org.orekit.orbits.CartesianOrbit;
import org.orekit.propagation.analytical.KeplerianPropagator;
import org.orekit.time.AbsoluteDate;
import org.orekit.time.TimeScalesFactory;
import org.orekit.utils.PVCoordinates;

public final class OrbelleTwoBodyReference {
    public static void main(String[] args) throws Exception {
        Locale.setDefault(Locale.ROOT);
        var reader = new BufferedReader(new InputStreamReader(System.in));
        var origin = new AbsoluteDate(2000, 1, 1, 12, 0, 0.0, TimeScalesFactory.getTAI());
        for (String line; (line = reader.readLine()) != null;) {
            var s = line.split(",");
            if (s.length != 10) throw new IllegalArgumentException("Expected ten columns");
            var date = origin.shiftedBy(Long.parseLong(s[2]), TimeUnit.NANOSECONDS);
            long elapsed = Long.parseLong(s[3]);
            var position = new Vector3D(Double.parseDouble(s[4]), Double.parseDouble(s[5]), Double.parseDouble(s[6]));
            var velocity = new Vector3D(Double.parseDouble(s[7]), Double.parseDouble(s[8]), Double.parseDouble(s[9]));
            var frame = FramesFactory.getGCRF();
            var orbit = new CartesianOrbit(new PVCoordinates(position, velocity), frame, date, Double.parseDouble(s[1]));
            var pv = new KeplerianPropagator(orbit).propagate(date.shiftedBy(elapsed, TimeUnit.NANOSECONDS)).getPVCoordinates(frame);
            System.out.printf("%s,%d", s[0], elapsed);
            for (double value : pv.getPosition().toArray()) System.out.printf(",%.17g", value);
            for (double value : pv.getVelocity().toArray()) System.out.printf(",%.17g", value);
            System.out.println();
        }
    }
}
