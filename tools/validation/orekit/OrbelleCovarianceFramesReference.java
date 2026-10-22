// Independent STK-axis references using Orekit 13.1.2 / Hipparchus 4.0.2.
// No Orbelle source or finite-difference rotation rates are used.
import java.nio.file.*;
import java.nio.charset.StandardCharsets;
import java.util.Locale;
import org.hipparchus.geometry.euclidean.threed.Vector3D;
import org.orekit.frames.*;
import org.orekit.time.*;
import org.orekit.utils.*;

public final class OrbelleCovarianceFramesReference {
    static final double MU = 3.986004415e14;
    static final AbsoluteDate DATE = new AbsoluteDate(2000, 1, 1, 12, 0, 0., TimeScalesFactory.getTAI());
    // GCRF, STK-LVLH projected/rotating, STK-Frenet projected/rotating.
    static final LOFType[] TYPES = {null, LOFType.LVLH_INERTIAL, LOFType.LVLH,
                                  LOFType.NTW_INERTIAL, LOFType.NTW};
    static Transform transform(int kind, PVCoordinates pv) {
        return kind == 0 ? Transform.IDENTITY : TYPES[kind].transformFromInertial(DATE, pv);
    }
    static void append(StringBuilder out, double value) {
        out.append(String.format(Locale.ROOT, " %.17e", value));
    }
    public static void main(String[] args) throws Exception {
        double[][] states = {
            {7e6,0,0, 0,7500,0, 0,0,0},
            {7e6,2e6,1e6, -1000,7000,1000, .001,-.0002,.003},
            {1e7,-2e7,4e6, 4000,2000,-500, -.0001,.0003,-.0005},
            {7e6,2e6,-5e5, 500,-7100,1400, .0001,-.0002,.0004},
            {1e-6,2e-6,7e6, 7400,0,-300, .0002,.0003,-.0001},
            {7e6,0,0, 2000,.2,0, 0,1e-8,2e-8},
            {8e6,-1e6,2e6, 2000,12000,4000, -.0003,.0001,.0002},
            {4e7,1e7,-3e6, -500,2900,600, .00001,.00002,-.00003}
        };
        StringBuilder out = new StringBuilder("# Orekit 13.1.2, Hipparchus 4.0.2; TAI epoch 0; SI; 200 cases\n");
        out.append("# id source target r[3] v[3] a[3] jacobian[36]\n");
        int count = 0;
        for (int k=0; k<states.length; ++k) {
            double[] s = states[k];
            Vector3D r = new Vector3D(s[0],s[1],s[2]), v = new Vector3D(s[3],s[4],s[5]);
            Vector3D a = new Vector3D(-MU/Math.pow(r.getNorm(),3), r)
                .add(new Vector3D(s[6],s[7],s[8]));
            if(k==0) a = new Vector3D(-v.getNormSq()/r.getNorm(),0,0);
            PVCoordinates pv = new PVCoordinates(r,v,a);
            for(int from=0; from<TYPES.length; ++from)
                for(int to=0; to<TYPES.length; ++to) {
                    Transform combined = new Transform(DATE, transform(from,pv).getInverse(), transform(to,pv));
                    double[][] jacobian = new double[6][6];
                    combined.getJacobian(CartesianDerivativesFilter.USE_PV,jacobian);
                    out.append(count++).append(' ').append(from).append(' ').append(to);
                    for(double value:r.toArray()) append(out,value);
                    for(double value:v.toArray()) append(out,value);
                    for(double value:a.toArray()) append(out,value);
                    for(double[] row:jacobian) for(double value:row) append(out,value);
                    out.append('\n');
                }
        }
        if(count!=200) throw new AssertionError(count);
        Files.writeString(Path.of(args[0]),out,StandardCharsets.US_ASCII);
        System.out.println("Orekit frame cases: "+count);
    }
}
