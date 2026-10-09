"""Create a scenario, then insert and sample a satellite (no GUI required)."""
import orbitforge as of
from math import radians


scenario = of.Scenario("Mission", of.Epoch(0), of.Epoch(7_200_000_000_000))
# Classical osculating elements: SI lengths and radians, Earth-centred GCRF.
elements = of.KeplerianElements(of.Epoch(0), 7_000_000, 0.01, radians(53), 0, 0, 0)
initial = of.to_cartesian(elements, 3.986004418e14)
satellite = of.make_two_body_session(initial, 3.986004418e14, "Satellite1")
scenario = scenario.with_object(satellite)
states = scenario.sample([of.Epoch(0), of.Epoch(3_600_000_000_000)])[0]
print(scenario.name, scenario.object_count, states.provenance.object_id)
for state in states.states:
    print(state.epoch.tai_nanoseconds, state.position_m, state.velocity_mps)
