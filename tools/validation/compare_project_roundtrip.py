"""Check a Python-created project after the real desktop opened and saved it.

Run with the installed wheel, passing the original and desktop-saved .orbelle files.
Only the formerly unbound EOP reference may be added by the desktop.
"""
import argparse
import json
from pathlib import Path
import orbelle as ob


def fingerprint(satellite):
    orbit, display = satellite.orbit, satellite.display
    state = orbit.initial
    return (orbit.object_id, satellite.display_name, orbit.propagator, orbit.input_frame, orbit.step_seconds,
            orbit.first.tai_nanoseconds, orbit.last.tai_nanoseconds,
            state.epoch.tai_nanoseconds, state.origin, state.frame,
            state.position_m, state.velocity_mps, display.extent,
            display.past_minutes, display.future_minutes, display.track_2d,
            display.labels_2d, display.track_3d, satellite.model_kind,
            satellite.model_diameter_m, satellite.model_path)


def compare(original_path, restored_path):
    original = ob.load_project_data(original_path)
    restored = ob.load_project_data(restored_path)
    for field in ('name', 'first', 'last', 'current'):
        left, right = getattr(original, field), getattr(restored, field)
        if field != 'name':
            left, right = left.tai_nanoseconds, right.tai_nanoseconds
        if left != right:
            raise AssertionError(f'Project {field} changed')
    if list(map(fingerprint, original.satellites)) != list(map(fingerprint, restored.satellites)):
        raise AssertionError('Satellite inputs or display settings changed')
    if restored.earth_orientation is None:
        raise AssertionError('Desktop did not pin its EOP data')
    epochs = [original.first, original.current, original.last]
    expected = original.to_scenario().sample(epochs)
    actual = restored.to_scenario().sample(epochs)
    for a, b in zip(expected, actual):
        if a.provenance.algorithm != b.provenance.algorithm:
            raise AssertionError('Propagation algorithm changed')
        for x, y in zip(a.states, b.states):
            if x.position_m != y.position_m or x.velocity_mps != y.velocity_mps:
                raise AssertionError('Computed state changed')
    return {'passed': True, 'satellites': len(actual), 'sample_epochs': len(epochs),
            'eop_snapshot': restored.earth_orientation.id}


if __name__ == '__main__':
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('original', type=Path)
    parser.add_argument('restored', type=Path)
    arguments = parser.parse_args()
    print(json.dumps(compare(arguments.original, arguments.restored), ensure_ascii=False))
