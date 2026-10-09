"""Run with the installed orbitforge wheel, independently of the desktop."""
import csv
import sys
import orbitforge as of

result = of.make_circular_demo_session().sample(of.demo_epochs())
writer = csv.writer(sys.stdout, lineterminator="\n")
writer.writerow(["tai_ns", "x_m", "y_m", "z_m", "vx_mps", "vy_mps", "vz_mps"])
for state in result.states:
    writer.writerow([state.epoch.tai_nanoseconds, *state.position_m, *state.velocity_mps])

