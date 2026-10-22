// Independent IERS 2010 celestial series from Orekit 13.1.2, not ERFA/SOFA.
// Input per line: tai_ns,u,xp,yp,dX,dY. Output: row-major GCRF->ITRF matrix.
import java.io.BufferedReader;
import java.io.InputStreamReader;
import java.util.Locale;
import java.util.concurrent.TimeUnit;
import org.hipparchus.CalculusFieldElement;
import org.orekit.data.DataContext;
import org.orekit.time.AbsoluteDate;
import org.orekit.time.FieldAbsoluteDate;
import org.orekit.time.TimeOffset;
import org.orekit.time.TimeScale;
import org.orekit.time.TimeScales;
import org.orekit.utils.IERSConventions;

public final class OrbelleFrameReference {
    static double[][] multiply(double[][] a, double[][] b) {
        double[][] r = new double[3][3];
        for (int i = 0; i < 3; i++)
            for (int j = 0; j < 3; j++)
                for (int k = 0; k < 3; k++) r[i][j] += a[i][k] * b[k][j];
        return r;
    }
    static double[][] rz(double a) {
        double c = Math.cos(a), s = Math.sin(a);
        return new double[][]{{c,s,0},{-s,c,0},{0,0,1}};
    }
    static double[][] rx(double a) {
        double c = Math.cos(a), s = Math.sin(a);
        return new double[][]{{1,0,0},{0,c,s},{0,-s,c}};
    }
    static double[][] ry(double a) {
        double c = Math.cos(a), s = Math.sin(a);
        return new double[][]{{c,0,-s},{0,1,0},{s,0,c}};
    }
    static double[][] rotation(long ns, double u, double xp, double yp, double dx, double dy) {
        TimeScales ts = DataContext.getDefault().getTimeScales();
        TimeScale tai = ts.getTAI();
        AbsoluteDate date = new AbsoluteDate(2000, 1, 1, 12, 0, 0.0, tai)
                .shiftedBy(ns, TimeUnit.NANOSECONDS);
        TimeScale ut1 = new TimeScale() {
            public String getName() { return "UT1-explicit-snapshot"; }
            public TimeOffset offsetFromTAI(AbsoluteDate d) { return new TimeOffset(u); }
            public <T extends CalculusFieldElement<T>> T offsetFromTAI(FieldAbsoluteDate<T> d) {
                return d.getField().getZero().add(u);
            }
        };
        IERSConventions conv = IERSConventions.IERS_2010;
        double[] xy = conv.getXYSpXY2Function(ts).value(date);
        // Follow IERS 2010 5.5.3: retain model s when applying observed pole offsets.
        // Standard Orekit CIRFProvider uses corrected XY here, so do not use that provider.
        double s = xy[2] - xy[0] * xy[1] / 2;
        double x = xy[0] + dx, y = xy[1] + dy;
        double z = Math.sqrt(1-x*x-y*y), a = 1/(1+z);
        double[][] q = {{1-a*x*x,-a*x*y,-x}, {-a*x*y,1-a*y*y,-y}, {x,y,z}};
        double[][] c = multiply(rz(-s), q);
        double angle = conv.getEarthOrientationAngleFunction(ut1, tai).value(date);
        double sp = -47e-6 * Math.PI/(180*3600) * conv.evaluateTC(date, ts);
        double[][] p = multiply(multiply(rx(-yp), ry(-xp)), rz(sp));
        return multiply(multiply(p, rz(angle)), c);
    }
    public static void main(String[] args) throws Exception {
        Locale.setDefault(Locale.ROOT);
        BufferedReader input = new BufferedReader(new InputStreamReader(System.in));
        String line;
        while ((line = input.readLine()) != null) {
            String[] f = line.split(",");
            if (f.length != 6) throw new IllegalArgumentException("Expected 6 CSV fields");
            double[][] r = rotation(Long.parseLong(f[0]), Double.parseDouble(f[1]),
                    Double.parseDouble(f[2]), Double.parseDouble(f[3]),
                    Double.parseDouble(f[4]), Double.parseDouble(f[5]));
            for (int i = 0; i < 3; i++) for (int j = 0; j < 3; j++) {
                if (i != 0 || j != 0) System.out.print(',');
                System.out.print(Double.toString(r[i][j]));
            }
            System.out.println();
        }
    }
}
