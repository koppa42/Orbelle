"""Create a two-body/J2 project for the desktop's installed historical EOP interval.

Run: python examples/python/save_project.py output.orbelle
External EOP data is selected on first desktop open, then pinned when saved.
"""
import sys
from pathlib import Path
import orbelle as of

project = of.Project('Python → Orbelle', of.Epoch(0), of.Epoch(7_200_000_000_000))
with project.edit():
    project.objects.satellite('TwoBody', orbit=of.orbits.TwoBody(initial=of.OrbitInput().initial))
    project.objects.satellite('J2', orbit=of.orbits.J2(initial=of.J2MeanElements(
        project.first, 7e6, 0.001, 0.9, 0, 0, 0)))
project.save(Path(sys.argv[1]))
print(f'Saved {len(project.objects)} satellites to {sys.argv[1]}')
