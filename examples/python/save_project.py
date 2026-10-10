"""Create a two-body/J2 project for the desktop's installed historical EOP interval.

Run: python examples/python/save_project.py output.orbitforge
External EOP data is selected on first desktop open, then pinned when saved.
"""
import sys
from pathlib import Path
import orbitforge as of

project = of.Project('Python → OrbitForge', of.Epoch(0), of.Epoch(7_200_000_000_000))
for name, kind in [('TwoBody', of.OrbitPropagator.TWO_BODY), ('J2', of.OrbitPropagator.J2_SECULAR)]:
    orbit = of.OrbitInput()
    orbit.object_id = name
    orbit.propagator = kind
    project.add_satellite(orbit)
project.save(Path(sys.argv[1]))
print(f'Saved {len(project.satellites)} satellites to {sys.argv[1]}')
