"""Fixed-input orbit queries. Optional: python analysis.py --csv trajectory.csv"""
import argparse
from pathlib import Path
import orbelle as ob
from orbelle import units as u


def main() -> None:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--csv", type=Path, help="Export owned results; refuses an existing file")
    parser.add_argument("--data", type=Path, help="Select current local catalog data for ITRF output; never downloads")
    parser.add_argument("--project", type=Path, help="Save inputs with this analysis's data provenance; refuses an existing file")
    args = parser.parse_args()
    project = ob.Project("Observation", ob.Epoch(0), ob.Epoch(300_000_000_000))
    satellite = project.objects.satellite("Observer", orbit=ob.orbits.TwoBody(
        initial=ob.orbits.Keplerian(
            epoch=project.first, semi_major_axis=u.km(7000), eccentricity=0.001,
            inclination=u.deg(53), raan=u.deg(0), argument_of_periapsis=u.deg(0),
        ),
    ))
    data = ob.DataCatalog(args.data) if args.data else None
    analysis = project.prepare(data=data)
    frame = ob.FrameId.ITRF if args.data else ob.FrameId.GCRF
    trajectory = analysis.states(satellite, step=u.s(40), frame=frame)
    # A later edit does not rename or alter the earlier analysis.
    satellite.name = "Updated name"
    print(f"{trajectory.object_name}: {len(trajectory)} states, {trajectory.frame.name}")
    print("Exact endpoint TAI ns:", trajectory.epoch.tai_nanoseconds[-1])
    print("Initial position (m):", trajectory.position.values[0])
    if args.csv:
        trajectory.to_csv(args.csv)
    if args.project:
        project.save(args.project, analysis=analysis)
        reopened = ob.Project.open(args.project, data=data)
        print("Reopened:", reopened.name, reopened.analysis_status.name)


if __name__ == "__main__":
    main()
