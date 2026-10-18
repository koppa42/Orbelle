"""Extend independent Orekit frame Jacobians with joint covariance references.

NumPy performs direct F P F.T; mpmath 80/100 decimal digits independently
confirm its result; SciPy LAPACK checks the active matrices. P includes optional
D/K state cross terms and mutual covariance. No OrbitForge code is imported.
"""
import argparse
import hashlib
from pathlib import Path

import mpmath as mp
import numpy as np
import scipy
from scipy.linalg import cholesky


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--transforms", type=Path, required=True)
    parser.add_argument("--output", type=Path, required=True)
    args = parser.parse_args()
    assert np.__version__ == "2.3.3"
    assert scipy.__version__ == "1.16.3"
    assert mp.__version__ == "1.3.0"
    raw = args.transforms.read_bytes()
    assert raw.startswith(b"# Orekit 13.1.2, Hipparchus 4.0.2;")
    lines = np.loadtxt(args.transforms)
    assert lines.shape == (200, 48)
    rng = np.random.default_rng(20261018)
    inputs = []
    for case in range(8):
        mask = case % 4
        active = list(range(6)) + ([6] if mask & 1 else []) + ([7] if mask & 2 else [])
        z = rng.normal(size=(len(active), len(active)))
        correlation = z @ z.T + 3 * np.eye(len(active))
        sigma = np.sqrt(np.diag(correlation))
        correlation /= np.outer(sigma, sigma)
        sigma = np.array([500, 800, 1500, .2, .4, .7, .004, .009])[active]
        p = np.zeros((8, 8))
        p[np.ix_(active, active)] = correlation * np.outer(sigma, sigma)
        inputs.append((mask, active, p))
    out = ["# Orekit 13.1.2 / Hipparchus 4.0.2; NumPy 2.3.3 / SciPy 1.16.3 / mpmath 1.3.0",
           "# Axes: 0 GCRF, 1 LVLH projected, 2 LVLH rotating, 3 Frenet projected, 4 Frenet rotating",
           "# STK LVLH=(R,I,C); Frenet=(N,T,C). Epoch=0 TAI; SI; J blocks 1,s,1/s,1.",
           "# Orekit transform SHA256 " + hashlib.sha256(raw).hexdigest(),
           "# id from to mask r[3] v[3] a[3] J[36] input_joint[64] output_joint[64]"]
    maximum = 0.
    for row in lines:
        case, source, target = map(int, row[:3])
        mask, active, p = inputs[case // 25]
        f = np.eye(8)
        f[:6, :6] = row[12:].reshape(6, 6)
        numpy_result = f @ p @ f.T
        outputs = []
        for precision in (80, 100):
            with mp.workdps(precision):
                fm = mp.matrix(f.tolist())
                high = fm * mp.matrix(p.tolist()) * fm.T
                outputs.append(np.array(high.tolist(), dtype=float))
        assert np.array_equal(*outputs), (case, "80/100 digit disagreement")
        expected = outputs[-1]
        for matrix in (p, expected):
            selected = matrix[np.ix_(active, active)]
            scale = np.sqrt(np.diag(selected))
            cholesky(selected / np.outer(scale, scale), lower=True)
        scale = np.sqrt(np.diag(expected)[active])
        error = np.max(np.abs((numpy_result-expected)[np.ix_(active, active)]) / np.outer(scale, scale))
        maximum = max(maximum, float(error))
        assert error < 3e-14, (case, error)
        values = np.concatenate((row[3:], p.ravel(), expected.ravel()))
        out.append(f"{case} {source} {target} {mask} " + " ".join(format(x, ".17e") for x in values))
    args.output.write_text("\n".join(out) + "\n", encoding="ascii", newline="\n")
    print(f"200 independent frame/covariance cases; NumPy/high precision error {maximum:.17e}")
    print("SHA256", hashlib.sha256(args.output.read_bytes()).hexdigest())


if __name__ == "__main__":
    main()
