"""60-digit independent root solves for the documented first-order J2 design model.

Requires mpmath 1.3.0. This is a numerical reference, not STK output.
Run from the repository root; the output file is a reviewed test fixture.
"""
from pathlib import Path
import mpmath as mp

mp.mp.dps = 60
R, MU, J2 = map(mp.mpf, ("6378136.6", "3.986004418e14", "0.0010826359"))
SPIN = mp.mpf("0.00007292115")
SUN = 2 * mp.pi / (mp.mpf("365.2422") * 86400)


def frequencies(a, e, cosine):
    n = mp.sqrt(MU / a**3)
    beta = mp.sqrt(1-e**2)
    factor = mp.mpf("1.5") * J2 * (R / (a * beta**2))**2
    node = -factor*n*cosine
    periapsis = factor*n*(mp.mpf("2.5")*cosine**2-mp.mpf("0.5"))
    mean = n*(1 + factor*beta*(mp.mpf("1.5")*cosine**2-mp.mpf("0.5")))
    return node, periapsis, mean


rows = []


def record(kind, a, e, cosine):
    node, peri, mean = frequencies(a, e, cosine)
    rows.append((kind, a, e, mp.acos(cosine), node, 2*mp.pi/(peri+mean)))


# Critical sun-synchronous: solve in eccentricity, holding perigee fixed.
rp = R + 500000
c = -1/mp.sqrt(5)
ecc = mp.findroot(lambda e: frequencies(rp/(1-e), e, c)[0]-SUN, (mp.mpf("0.2"), mp.mpf("0.5")))
record(2, rp/(1-ecc), ecc, c)
# Molniya: anomalistic frequency equals twice Earth's nominal rotation.
c = 1/mp.sqrt(5)
ecc = mp.findroot(lambda e: frequencies(rp/(1-e), e, c)[2]-2*SPIN, (mp.mpf("0.7"), mp.mpf("0.8")))
record(4, rp/(1-ecc), ecc, c)
for kind in (6, 7):
    def cosine(a):
        return mp.cos(53*mp.pi/180) if kind == 6 else -SUN/(-frequencies(a, 0, 1)[0])

    def constraint(a):
        node, peri, mean = frequencies(a, 0, cosine(a))
        return (mean+peri)/(SPIN-node)-15

    axis = mp.findroot(constraint, (mp.mpf("6800000"), mp.mpf("7200000")))
    record(kind, axis, 0, cosine(axis))
axis = R+500000
record(8, axis, 0, -SUN/(-frequencies(axis, 0, 1)[0]))
output = Path("tests/reference/propagators/orbit_design.txt")
output.write_text("# mpmath 1.3.0, 60 digits; default EarthOrbitDesignRequest, type overridden\n"
                  "# type a_m e i_rad node_rad_s nodal_period_s\n" +
                  "\n".join(" ".join(str(v) if isinstance(v, int) else mp.nstr(v, 35) for v in row)
                            for row in rows) + "\n", encoding="utf-8")
