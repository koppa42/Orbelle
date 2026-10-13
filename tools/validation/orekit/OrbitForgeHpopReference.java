// Independent static-field reference: Orekit 13.1.2 Holmes-Featherstone + DP853.
// GCRF and the gravity frame coincide deliberately, isolating gravity/integration
// from the separately validated Earth orientation adapter. No OrbitForge code.
import java.nio.file.Files;
import java.nio.file.Path;
import java.util.Locale;
import org.hipparchus.geometry.euclidean.threed.Vector3D;
import org.hipparchus.ode.nonstiff.DormandPrince853Integrator;
import org.orekit.forces.gravity.HolmesFeatherstoneAttractionModel;
import org.orekit.forces.gravity.potential.GravityFieldFactory;
import org.orekit.forces.gravity.potential.TideSystem;
import org.orekit.frames.FramesFactory;
import org.orekit.orbits.CartesianOrbit;
import org.orekit.orbits.OrbitType;
import org.orekit.propagation.SpacecraftState;
import org.orekit.propagation.numerical.NumericalPropagator;
import org.orekit.time.AbsoluteDate;
import org.orekit.time.TimeScalesFactory;
import org.orekit.utils.PVCoordinates;

public final class OrbitForgeHpopReference {
    public static void main(String[] args) throws Exception {
        Locale.setDefault(Locale.ROOT);
        byte[] bytes = Files.readAllBytes(Path.of(args[0]));
        String sha = java.util.HexFormat.of().formatHex(java.security.MessageDigest.getInstance("SHA-256").digest(bytes));
        if (!sha.equals("5247a9e9c316dd2c8f8fd491d53be0e163cb5cb3676b021754240cc9e44cb43b"))
            throw new IllegalArgumentException("Wrong EGM96 source");
        final double mu = 3.986004415e14;
        final double radius = 6378136.3;
        var frame = FramesFactory.getGCRF();
        var epoch = new AbsoluteDate(2000, 1, 1, 12, 0, 0, TimeScalesFactory.getTAI());
        double[][] cases = {{7e6,0,0,0,6200,5300}, {42164000,0,0,0,Math.sqrt(mu/42164000),0},
                            {0,8e6,0,100,0,7000}, {7e6,0,0,0,9700,1000}, {7e6,0,0,0,12000,1000}};
        System.out.println("# Orekit 13.1.2 / Hipparchus 4.0.2; EGM96 static tide-free; identity body frame");
        System.out.println("# degree order dt_seconds rx ry rz vx vy vz expected_r expected_v acceleration_at_initial");
        for (int[] nm : new int[][] {{2,0},{8,8},{20,20}}) {
            int degree=nm[0], order=nm[1];
            double[][] c = new double[degree+1][], s = new double[degree+1][];
            for (int n=0; n<=degree; n++) { c[n]=new double[Math.min(n,order)+1]; s[n]=new double[c[n].length]; }
            c[0][0]=1;
            for (String line : new String(bytes, java.nio.charset.StandardCharsets.US_ASCII).split("\\R")) {
                String[] f = line.strip().split("\\s+");
                if (f.length<5 || !f[0].equals("gfc")) continue;
                int n=Integer.parseInt(f[1]), m=Integer.parseInt(f[2]);
                if (n<=degree && m<=order) { c[n][m]=Double.parseDouble(f[3]); s[n][m]=Double.parseDouble(f[4]); }
            }
            var coefficients = GravityFieldFactory.getNormalizedProvider(radius, mu, TideSystem.TIDE_FREE, c, s);
            var gravity = new HolmesFeatherstoneAttractionModel(frame, coefficients);
            for (double[] initial : cases) {
                var r = new Vector3D(initial[0],initial[1],initial[2]);
                var v = new Vector3D(initial[3],initial[4],initial[5]);
                var orbit = new CartesianOrbit(new PVCoordinates(r,v),frame,epoch,mu);
                var acceleration = gravity.acceleration(new SpacecraftState(orbit), new double[] {mu})
                    .add(r.scalarMultiply(-mu / Math.pow(r.getNorm(),3)));
                for (double dt : new double[] {-86400,-6000,6000,86400}) {
                    var integrator = new DormandPrince853Integrator(0.0001,30,
                        new double[]{1e-6,1e-6,1e-6,1e-9,1e-9,1e-9,1e-6},
                        new double[]{1e-14,1e-14,1e-14,1e-14,1e-14,1e-14,1e-14});
                    var propagator = new NumericalPropagator(integrator);
                    propagator.setOrbitType(OrbitType.CARTESIAN);
                    propagator.setInitialState(new SpacecraftState(orbit));
                    propagator.addForceModel(gravity);
                    var pv = propagator.propagate(epoch.shiftedBy(dt)).getPVCoordinates(frame);
                    System.out.printf("%d %d %.0f",degree,order,dt);
                    for (double value : initial) System.out.printf(" %.17g",value);
                    for (double value : pv.getPosition().toArray()) System.out.printf(" %.17g",value);
                    for (double value : pv.getVelocity().toArray()) System.out.printf(" %.17g",value);
                    for (double value : acceleration.toArray()) System.out.printf(" %.17g",value);
                    System.out.println();
                }
            }
        }
    }
}
