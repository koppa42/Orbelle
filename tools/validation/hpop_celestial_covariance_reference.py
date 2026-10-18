"""Independent NumPy/SciPy/mpmath joint covariance from Orekit celestial frames."""
import argparse
import hashlib
from pathlib import Path
import mpmath as mp
import numpy as np
import scipy
from scipy.linalg import cholesky

def main():
    parser=argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--frames",type=Path,required=True)
    parser.add_argument("--output",type=Path,required=True)
    args=parser.parse_args()
    assert (np.__version__,scipy.__version__,mp.__version__)==("2.3.3","1.16.3","1.3.0")
    rng=np.random.default_rng(20261019)
    out=["# Orekit 13.1.2 / Hipparchus 4.0.2; NumPy 2.3.3 / SciPy 1.16.3 / mpmath 1.3.0",
         "# Input frames SHA256 "+hashlib.sha256(args.frames.read_bytes()).hexdigest(),
         "# ns kind velocity u xp yp dx dy u_rate xp_rate yp_rate dx_rate dy_rate P[64] transformed_P[64]"]
    count=0;maximum=0.
    for line in args.frames.read_text().splitlines():
        if not line or line.startswith("#"):continue
        raw=line.split(); row=np.array([float(v) for v in raw[2:]])
        rotation=row[10:19].reshape(3,3); rate=row[19:28].reshape(3,3)
        z=rng.normal(size=(8,8));p=z@z.T+4*np.eye(8)
        scale=np.array([500,800,1500,.2,.4,.7,.004,.009])/np.sqrt(p.diagonal())
        p*=np.outer(scale,scale)
        for rotating in (0,1):
            f=np.eye(8);f[:3,:3]=f[3:6,3:6]=rotation
            if rotating:f[3:6,:3]=rate
            values=[]
            for precision in (80,100):
                with mp.workdps(precision):
                    fm=mp.matrix(f.tolist())
                    values.append(np.array((fm*mp.matrix(p.tolist())*fm.T).tolist(),dtype=float))
            assert np.array_equal(*values)
            expected=values[-1];sigma=np.sqrt(expected.diagonal())
            cholesky(expected/np.outer(sigma,sigma),lower=True)
            delta=np.max(np.abs(f@p@f.T-expected)/np.outer(sigma,sigma))
            assert delta<3e-14
            maximum=max(maximum,float(delta))
            fields=np.concatenate((row[:10],p.ravel(),expected.ravel()))
            out.append(raw[0]+" "+raw[1]+" "+str(rotating)+" "+" ".join(format(v,".17e") for v in fields))
            count+=1
    assert count==96
    args.output.write_text("\n".join(out)+"\n",encoding="ascii",newline="\n")
    print("96 joint references; maximum NumPy/high precision error",maximum)
    print("SHA256",hashlib.sha256(args.output.read_bytes()).hexdigest())

if __name__=="__main__":
    main()
