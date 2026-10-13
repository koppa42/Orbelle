"""Fixed-input orbit queries. Optional: python analysis.py --csv trajectory.csv"""
import argparse
from pathlib import Path
import orbitforge as of
from orbitforge import units as u


def main() -> None:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--csv", type=Path, help="Export owned results; refuses an existing file")
    args = parser.parse_args()
    project = of.Project("Observation", of.Epoch(0), of.Epoch(300_000_000_000))
    satellite = project.objects.satellite("Observer", orbit=of.orbits.TwoBody(
        initial=of.orbits.Keplerian(
            epoch=project.first, semi_major_axis=u.km(7000), eccentricity=0.001,
            inclination=u.deg(53), raan=u.deg(0), argument_of_periapsis=u.deg(0),
        ),
    ))
    analysis = project.prepare()
    trajectory = analysis.states(satellite, step=u.s(40), frame=of.FrameId.GCRF)
    # A later edit does not rename or alter the earlier analysis.
    satellite.name = "Updated name"
    print(f"{trajectory.object_name}: {len(trajectory)} states, {trajectory.frame.name}")
    print("Exact endpoint TAI ns:", trajectory.epoch.tai_nanoseconds[-1])
    print("Initial position (m):", trajectory.position.values[0])
    if args.csv:
        trajectory.to_csv(args.csv)


if __name__ == "__main__":
    main()
