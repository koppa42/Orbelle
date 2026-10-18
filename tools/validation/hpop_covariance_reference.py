"""Independent joint covariance: pinned Orekit STM + NumPy/SciPy + 80/100-digit algebra.

python tools/validation/hpop_covariance_reference.py OUTPUT
No OrbitForge extension or production algorithm is imported.
"""
from pathlib import Path
import hashlib
import sys
import numpy as np
import scipy
from scipy.linalg import cholesky
import mpmath as mp

EXPECTED = ("2.3.3", "1.16.3", "1.3.0")
if (np.__version__, scipy.__version__, mp.__version__) != EXPECTED:
    raise RuntimeError(f"Use numpy/scipy/mpmath versions {EXPECTED}")
SOURCE_SHA = "409e9d3b37838d74fb3d70ebbde35afad542916c4e285f9004fdb764b3ecc023"
source = Path(__file__).resolve().parents[2] / "tests/reference/propagators/hpop_variational.txt"
assert hashlib.sha256(source.read_bytes()).hexdigest() == SOURCE_SHA
rng = np.random.default_rng(20261018)
rows = [list(map(float, line.split())) for line in source.read_text().splitlines()
        if line and not line.startswith("#")]
output = ["# Joint covariance: Orekit 13.1.2 / Hipparchus 4.0.2 fixed STM and D/K",
          "# NumPy 2.3.3 / SciPy 1.16.3 LAPACK; mpmath 1.3.0 80/100 decimal digits",
          "# id orekit_id active_mask input_joint[8x8] output_joint[8x8] initial/parameter/cross[3x6x6]",
          "# inactive D/K rows and columns are zero; state/parameters are Earth GCRF and SI"]
maximum = 0.0
for row in rows:
    reference_id = int(row[0])
    phi = np.array(row[17:53]).reshape(6, 6)
    # The original reference columns are 5 metadata + 6 initial + 6 final state.
    sensitivities = np.array([row[53:59], row[59:65]]).T
    for pattern in range(2):
        mask = (reference_id + pattern) % 4
        active = list(range(6)) + ([6] if mask & 1 else []) + ([7] if mask & 2 else [])
        n = len(active)
        sigmas = np.array([1000, 2000, 700, .2, .5, .7, .004, .003])
        if pattern:
            sigmas[:6] *= .01
            sigmas[6:] *= 10
        scales = sigmas[active]
        if pattern:
            seed = np.eye(n) + rng.uniform(-.35, .35, (n, n))
            correlation = seed @ seed.T
            standard = np.sqrt(np.diag(correlation))
            correlation /= standard[:, None] * standard[None, :]
        else:
            correlation = np.eye(n)
        initial = correlation * scales[:, None] * scales[None, :]
        initial = (initial + initial.T) / 2
        cholesky(initial / scales[:, None] / scales[None, :], lower=True)
        f = np.eye(8)
        f[:6, :6], f[:6, 6:] = phi, sensitivities
        f = f[np.ix_(active, active)]

        def evaluate(digits):
            with mp.workdps(digits):
                p, a = mp.matrix(initial.tolist()), mp.matrix(f.tolist())
                mp.cholesky(p)
                total = a * p * a.T
                state = a[:6, :6] * p[:6, :6] * a[:6, :6].T
                if n > 6:
                    s = a[:6, 6:]
                    parameter = s * p[6:, 6:] * s.T
                    cross = a[:6, :6] * p[:6, 6:] * s.T
                    cross += cross.T
                else:
                    parameter, cross = mp.zeros(6), mp.zeros(6)
                return [np.array(matrix.tolist(), dtype=float) for matrix in (total, state, parameter, cross)]

        precise = evaluate(80)
        assert all(np.array_equal(a, b) for a, b in zip(precise, evaluate(100)))
        ordinary = [f @ initial @ f.T, phi @ initial[:6, :6] @ phi.T]
        if n > 6:
            s = f[:6, 6:]
            cross = phi @ initial[:6, 6:] @ s.T
            ordinary.extend([s @ initial[6:, 6:] @ s.T, cross + cross.T])
        else:
            ordinary.extend([np.zeros((6, 6)), np.zeros((6, 6))])
        for actual, expected in zip(ordinary, precise):
            error = np.max(np.abs(actual - expected)) / max(np.max(np.abs(expected)), 1e-300)
            maximum = max(maximum, error)
            assert error < 3e-14, (reference_id, pattern, error)
        norm = np.sqrt(np.diag(precise[0]))
        cholesky(precise[0] / norm[:, None] / norm[None, :], lower=True)
        full_input, full_output = np.zeros((8, 8)), np.zeros((8, 8))
        full_input[np.ix_(active, active)] = initial
        full_output[np.ix_(active, active)] = precise[0]
        values = np.concatenate([full_input.ravel(), full_output.ravel(),
                                 *(m.ravel() for m in precise[1:])])
        output.append(f"{2*reference_id+pattern} {reference_id} {mask}" +
                      "".join(f" {x:.17e}" for x in values))
Path(sys.argv[1]).write_text("\n".join(output) + "\n", encoding="ascii", newline="\n")
print(f"cases={2*len(rows)} numpy_relative={maximum:.17g}; 80/100-digit rounded results identical")
