// Orekit 13.1.2 / Hipparchus 4.0.2; no Orbelle or ERFA implementation.
// Independent IERS 1996 nutation/precession; automatic time derivatives.
import java.nio.file.*;
import java.nio.charset.StandardCharsets;
import java.util.Locale;
import java.util.concurrent.TimeUnit;
import org.hipparchus.CalculusFieldElement;
import org.hipparchus.dfp.Dfp;
import org.hipparchus.dfp.DfpField;
import org.hipparchus.analysis.differentiation.UnivariateDerivative1;
import org.hipparchus.geometry.euclidean.threed.*;
import org.orekit.data.DataContext;
import org.orekit.time.*;
import org.orekit.utils.IERSConventions;

public final class OrbelleCelestialFramesReference {
    static UnivariateDerivative1 u(double x) { return new UnivariateDerivative1(x,0); }
    static UnivariateDerivative1[][] mul(UnivariateDerivative1[][] a, UnivariateDerivative1[][] b) {
        var c=new UnivariateDerivative1[3][3];
        for(int i=0;i<3;i++) for(int j=0;j<3;j++) {
            c[i][j]=u(0);
            for(int k=0;k<3;k++) c[i][j]=c[i][j].add(a[i][k].multiply(b[k][j]));
        }
        return c;
    }
    static UnivariateDerivative1[][] trans(UnivariateDerivative1[][] a) {
        var b=new UnivariateDerivative1[3][3];
        for(int i=0;i<3;i++) for(int j=0;j<3;j++) b[i][j]=a[j][i];
        return b;
    }
    static UnivariateDerivative1[][] spin(UnivariateDerivative1 a) {
        var c=a.cos();var s=a.sin();
        return new UnivariateDerivative1[][]{{c,s,u(0)},{s.negate(),c,u(0)},{u(0),u(0),u(1)}};
    }
    static void append(StringBuilder s,double x) { s.append(String.format(Locale.ROOT," %.17e",x)); }
    public static void main(String[] args) throws Exception {
        TimeScales ts=DataContext.getDefault().getTimeScales();
        AbsoluteDate zero=new AbsoluteDate(2000,1,1,12,0,0.,ts.getTAI());
        long[] epochs={-883656000000000000L,-89726371000000000L,-89726370000000001L,
            -89726369999999999L,-89726369000000000L,-32184000000L,0L,
            536500835000000000L,536500836000000000L,536500837000000000L,
            845640000123456789L,1577880000000000000L};
        StringBuilder out=new StringBuilder("# Orekit 13.1.2 / Hipparchus 4.0.2; 48 celestial cases\n");
        out.append("# ns kind u xp yp dx dy u_rate xp_rate yp_rate dx_rate dy_rate R[9] Rdot[9] CIRF_to_FK5[9]\n");
        for(long ns:epochs) for(int variant=0;variant<2;variant++) {
            double offset=variant==0?-32.: -36.4, dx=variant*2e-9, dy=-variant*3e-9;
            double ur=variant*-2e-8, dxr=variant*1e-13, dyr=variant*-2e-13;
            if(ns==0 && variant==1) { dxr=.001; dyr=-.0007; }
            AbsoluteDate base=zero.shiftedBy(ns,TimeUnit.NANOSECONDS);
            var date=new FieldAbsoluteDate<UnivariateDerivative1>(base,new UnivariateDerivative1(0,1));
            TimeScale ut1=new TimeScale() {
                public String getName(){return "UT1-explicit";}
                public TimeOffset offsetFromTAI(AbsoluteDate d){return new TimeOffset(offset);}
                public <T extends CalculusFieldElement<T>> T offsetFromTAI(FieldAbsoluteDate<T> d){
                    return d.getField().getZero().add(offset);
                }
            };
            var conv=IERSConventions.IERS_1996;
            var eps=conv.getMeanObliquityFunction(ts).value(date);
            var nut=conv.getNutationFunction(ts).value(date);
            var angles=conv.getPrecessionFunction(ts).value(date);
            double eps0=conv.getMeanObliquityFunction(ts).value(AbsoluteDate.J2000_EPOCH);
            var r4=new FieldRotation<UnivariateDerivative1>(date.getField(),
                new Rotation(Vector3D.PLUS_I,eps0,RotationConvention.FRAME_TRANSFORM));
            var p=r4.compose(new FieldRotation<>(RotationOrder.ZXZ,RotationConvention.FRAME_TRANSFORM,
                angles[0].negate(),angles[1].negate(),angles[2]),RotationConvention.FRAME_TRANSFORM);
            var n=new FieldRotation<>(RotationOrder.XZX,RotationConvention.FRAME_TRANSFORM,
                eps,nut[0].negate(),eps.add(nut[1]).negate());
            var eq=nut[0].multiply(eps.cos()).add(nut[2]);
            var gm=conv.getGMSTFunction(ut1,ts).value(date);
            var er=IERSConventions.IERS_2010.getEarthOrientationAngleFunction(ut1,ts.getTAI()).value(date);
            gm=new UnivariateDerivative1(gm.getValue(),gm.getFirstDerivative()*(1+ur));
            er=new UnivariateDerivative1(er.getValue(),er.getFirstDerivative()*(1+ur));
            // The large unwrapped ERA and GMST angles lose picoradians on
            // subtraction in double. Evaluate their difference at 80 decimal digits.
            // Rates remain the independent automatic derivatives above.
            var highDate=new FieldAbsoluteDate<Dfp>(new DfpField(80),base);
            var highGm=conv.getGMSTFunction(ut1,ts).value(highDate);
            var highEr=IERSConventions.IERS_2010.getEarthOrientationAngleFunction(ut1,ts.getTAI()).value(highDate);
            var eraMinusGmst=new UnivariateDerivative1(highEr.subtract(highGm).getReal(),
                er.getFirstDerivative()-gm.getFirstDerivative());
            var xy=IERSConventions.IERS_2010.getXYSpXY2Function(ts).value(date);
            var s=xy[2].subtract(xy[0].multiply(xy[1]).multiply(.5));
            var x=xy[0].add(new UnivariateDerivative1(dx,dxr));
            var y=xy[1].add(new UnivariateDerivative1(dy,dyr));
            var z=u(1).subtract(x.multiply(x)).subtract(y.multiply(y)).sqrt();
            var a=u(1).divide(z.add(1));var axy=a.multiply(x).multiply(y).negate();
            var q=new UnivariateDerivative1[][]{
                {u(1).subtract(a.multiply(x).multiply(x)),axy,x.negate()},
                {axy,u(1).subtract(a.multiply(y).multiply(y)),y.negate()},{x,y,z}};
            var c=mul(spin(s.negate()),q);
            for(int kind=0;kind<2;kind++) {
                var factor=spin(eraMinusGmst.subtract(eq));
                if(kind==0) factor=mul(trans(mul(n.getMatrix(),p.getMatrix())),factor);
                var matrix=mul(factor,c);
                out.append(ns).append(' ').append(kind);
                for(double v:new double[]{offset,variant*1e-6,-variant*2e-6,dx,dy,ur,
                                          variant*1e-11,-variant*2e-11,dxr,dyr}) append(out,v);
                for(var row:matrix) for(var v:row) append(out,v.getValue());
                for(var row:matrix) for(var v:row) append(out,v.getFirstDerivative());
                for(var row:factor) for(var v:row) append(out,v.getValue());
                out.append('\n');
            }
        }
        Files.writeString(Path.of(args[0]),out,StandardCharsets.US_ASCII);
        System.out.println("48 independent celestial cases");
    }
}
