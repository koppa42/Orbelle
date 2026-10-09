#include <cmath>
#include <iostream>
#include <orbitforge/runtime/demo.hpp>
#include <orbitforge/runtime/ephemeris.hpp>
#include <orbitforge/runtime/time_frames.hpp>
int main(int argc, char **argv) {
    const auto result = orbitforge::runtime::make_circular_demo_session().sample(
        orbitforge::runtime::demo_epochs());
    if (result.states().size() != 6 || result.states().front().position_m[0] != 7e6) {
        return 1;
    }
    if (orbitforge::runtime::time_frames_available() !=
        static_cast<bool>(ORBITFORGE_EXPECT_TIME_FRAMES)) {
        return 2;
    }
    if (orbitforge::runtime::time_frames_available()) {
        if (argc < 2) {
            return 4;
        }
        const auto time = orbitforge::runtime::make_builtin_time_provider();
        const auto epoch = time->from_utc({2016, 12, 31, 23, 59, 60, 123456789});
        const auto utc = time->to_utc(epoch);
        const auto frames = orbitforge::runtime::make_frame_provider();
        const auto eop = orbitforge::runtime::load_eop_snapshot(argv[1]);
        const auto transform = frames->gcrf_to_itrf_state(epoch, eop->at(epoch));
        if (utc.second != 60 || utc.nanosecond != 123456789 ||
            transform.rotation.to != orbitforge::core::FrameId::itrf) {
            return 3;
        }
        const orbitforge::core::CartesianState ground{epoch,
                                                      orbitforge::core::OriginId::earth,
                                                      orbitforge::core::FrameId::itrf,
                                                      {6378137, 0, 0},
                                                      {0, 0, 0}};
        const auto inertial = transform.inverse(ground);
        const auto back = transform.apply(inertial);
        if (std::abs(std::hypot(inertial.velocity_mps[0], inertial.velocity_mps[1],
                                inertial.velocity_mps[2]) -
                     465.1011) > 0.001) {
            return 5;
        }
        for (std::size_t k = 0; k < 3; ++k) {
            if (std::abs(back.position_m[k] - ground.position_m[k]) > 5e-8 ||
                std::abs(back.velocity_mps[k]) > 5e-10) {
                return 6;
            }
        }
    }
    if (orbitforge::runtime::ephemeris_available() != static_cast<bool>(ORBITFORGE_EXPECT_SPICE)) {
        return 7;
    }
    if (orbitforge::runtime::ephemeris_available()) {
        if (argc != 4) {
            return 8;
        }
        const auto provider = orbitforge::runtime::load_spice_snapshot(argv[2], argv[3]);
        const auto epoch = provider->data_info().first_epoch;
        const auto moon = provider->geometric_state(301, 399, epoch);
        const auto range = std::hypot(moon.position_m[0], moon.position_m[1], moon.position_m[2]);
        if (moon.frame != orbitforge::core::FrameId::spice_j2000 ||
            moon.velocity_time_scale != orbitforge::core::TimeScale::tdb || range < 3.5e8 ||
            range > 4.1e8 ||
            std::abs(provider->from_tdb(provider->to_tdb(epoch)).seconds_since(epoch)) > 1e-6) {
            return 9;
        }
    }
    std::cout << "Installed SDK consumer passed\n";
}
