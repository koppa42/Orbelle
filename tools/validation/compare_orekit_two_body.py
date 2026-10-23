"""Compare the installed Python two-body API with the frozen independent Orekit implementation."""
import argparse
import hashlib
import json
import math
import os
from pathlib import Path
import subprocess
import orbelle as ob


def sha(path):
    return hashlib.sha256(path.read_bytes()).hexdigest()


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--jar-dir', type=Path, required=True)
    parser.add_argument('--output', type=Path, required=True)
    args = parser.parse_args()
    reference = Path(__file__).resolve().parent / 'orekit'
    dependencies = json.loads((reference / 'dependencies.json').read_text())
    jars = []
    for item in dependencies:
        path = args.jar_dir / item['filename']
        if sha(path) != item['sha256']:
            raise ValueError(f'Reference hash mismatch: {path}')
        jars.append(str(path.resolve()))
    args.output.mkdir(parents=True, exist_ok=False)
    classpath = os.pathsep.join(jars)
    java = reference / 'OrbelleTwoBodyReference.java'
    subprocess.run(['javac', '-cp', classpath, '-d', str(args.output), str(java)], check=True)
    mu = 3.986004418e14
    cases = [
        ('leo', [7e6,0,0], [0,7500,0]),
        ('inclined_eccentric', [7e6,0,0], [0,6200,5300]),
        ('geo_retrograde', [42164000,0,0], [0,-math.sqrt(mu/42164000),0]),
        ('high_eccentricity', [7e6,0,0], [0,9700,1000]),
        ('polar_retrograde', [0,8e6,0], [100,0,7000]),
        ('hyperbolic', [7e6,0,0], [0,8000,9000]),
        ('general_cartesian', [4e6,5e6,3e6], [-5500,2500,4500])]
    shifts = [-86400_000_000_000,-6000_000_000_000,-1000_000_000_000,-1,0,1,
              1000_000_000_000,6000_000_000_000,86400_000_000_000]
    rows, actual_rows = [], []
    for index, (name, r, v) in enumerate(cases):
        epoch = 0 if index % 2 == 0 else 8_000_000_000_000_000_000
        initial = ob.CartesianState(ob.Epoch(epoch), ob.OriginId.EARTH, ob.FrameId.GCRF, r, v)
        session = ob.make_two_body_session(initial, mu, name)
        series = session.sample([ob.Epoch(epoch + dt) for dt in shifts])
        states = series.states
        if len(states) != len(shifts):
            raise RuntimeError(f'Incomplete state series for {name}')
        provenance = series.provenance
        if (provenance.object_id != name or provenance.input_revision != 1 or
                provenance.algorithm != 'two-body-cspice-n0067-prop2b-tai-v1'):
            raise RuntimeError(f'Wrong state provenance for {name}')
        for dt, state in zip(shifts, states, strict=True):
            if (state.epoch.tai_nanoseconds != epoch + dt or state.frame != ob.FrameId.GCRF or
                    state.origin != ob.OriginId.EARTH or len(state.position_m) != 3 or len(state.velocity_mps) != 3):
                raise RuntimeError(f'Wrong state metadata for {name} at {dt}')
            rows.append(','.join(map(str, [name, mu, epoch, dt, *r, *v])))
            actual_rows.append([name, dt, *state.position_m, *state.velocity_mps])
    inputs = '\n'.join(rows) + '\n'
    result = subprocess.run(['java', '-cp', str(args.output.resolve()) + os.pathsep + classpath,
                             'OrbelleTwoBodyReference'], input=inputs, text=True,
                            capture_output=True, check=True)
    expected = [line.split(',') for line in result.stdout.splitlines()]
    if len(expected) != 63 or len(actual_rows) != 63 or any(len(row) != 8 for row in expected):
        raise RuntimeError('Incomplete independent reference output')
    errors = []
    for a, b in zip(actual_rows, expected):
        if a[:2] != [b[0], int(b[1])]:
            raise RuntimeError('Reference case or epoch order mismatch')
        values = list(map(float, b[2:]))
        if not all(math.isfinite(x) for x in values + a[2:]):
            raise RuntimeError('Nonfinite result')
        errors.append({'case': a[0], 'elapsed_ns': a[1],
                       'max_position_component_error_m': max(abs(x-y) for x,y in zip(a[2:5],values[:3])),
                       'max_velocity_component_error_mps': max(abs(x-y) for x,y in zip(a[5:],values[3:]))})
    max_p = max(e['max_position_component_error_m'] for e in errors)
    max_v = max(e['max_velocity_component_error_mps'] for e in errors)
    native = next(p for p in Path(ob.__file__).parent.glob('_native.*') if p.suffix in ('.pyd', '.so'))
    report = {'reference': 'Orekit 13.1.2 / Hipparchus 4.0.2 KeplerianPropagator',
              'model': 'Earth GCRF, explicit SI mu, elapsed TAI seconds', 'cases': len(cases),
              'states': len(errors), 'position_tolerance_m': .01, 'velocity_tolerance_mps': 1e-5,
              'max_position_component_error_m': max_p, 'max_velocity_component_error_mps': max_v,
              'passed': max_p <= .01 and max_v <= 1e-5, 'errors': errors,
              'native_module_sha256': sha(native), 'reference_source_sha256': sha(java),
              'checker_sha256': sha(Path(__file__)), 'dependencies': dependencies}
    for name, content in [('input.csv', inputs), ('reference.csv', result.stdout),
                          ('actual.json', json.dumps(actual_rows, indent=2) + '\n'),
                          ('report.json', json.dumps(report, indent=2) + '\n')]:
        (args.output / name).write_text(content, encoding='utf-8', newline='\n')
    print(json.dumps({key: value for key, value in report.items() if key not in ('errors','dependencies')}, indent=2))
    if not report['passed']:
        raise RuntimeError('Independent two-body comparison failed')


if __name__ == '__main__':
    main()
