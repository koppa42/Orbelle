"""Create a scenario, then insert and sample a satellite (no GUI required)."""
import orbitforge as of


scenario = of.Scenario("Mission", of.Epoch(0), of.Epoch(7_200_000_000_000))
initial = of.CartesianState(of.Epoch(0), of.OriginId.EARTH, of.FrameId.GCRF,
                            [7_000_000, 0, 0], [0, 6_200, 5_300])
satellite = of.make_two_body_session(initial, 3.986004418e14, "Satellite1")
scenario = scenario.with_object(satellite)
states = scenario.sample([of.Epoch(0), of.Epoch(3_600_000_000_000)])[0]
print(scenario.name, scenario.object_count, states.provenance.object_id)
for state in states.states:
    print(state.epoch.tai_nanoseconds, state.position_m, state.velocity_mps)
