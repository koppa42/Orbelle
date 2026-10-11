#include <cmath>
#include <iostream>
#include <orbitforge/application/scenario.hpp>
#include <orbitforge/runtime/data_catalog.hpp>
#include <orbitforge/runtime/demo.hpp>
#include <orbitforge/runtime/ephemeris.hpp>
#include <orbitforge/runtime/project.hpp>
#include <orbitforge/runtime/report.hpp>
#include <orbitforge/runtime/time_frames.hpp>
#include <orbitforge/runtime/two_body.hpp>
int main(int argc, char **argv) {
    const orbitforge::application::Project project("SDK project", orbitforge::core::Epoch{123},
                                                   orbitforge::core::Epoch{10'000'000'123});
    const auto restored =
        orbitforge::application::decode_project(orbitforge::application::encode_project(project));
    auto report_project = project;
    report_project.ground_stations.push_back({"station", "SDK station", {0, 0, 0}});
    orbitforge::application::ReportDefinition report;
    report.id = "sdk-report";
    report.name = "SDK report";
    report.frame = orbitforge::core::FrameId::itrf;
    report.time_scale = orbitforge::application::ReportTimeScale::tai;
    const auto table = orbitforge::runtime::generate_report({report_project, {}, {}, {}}, report);
    if (table.table.rows().size() != 2 || std::get<double>(table.table.rows()[0][4]) != 6378137)
        return 18;
    if (restored.first != project.first || restored.last != project.last ||
        !orbitforge::runtime::make_project_scenario(restored).objects().empty() ||
        restored.data_lock() != project.data_lock())
        return 14;
    const orbitforge::application::Scenario empty("SDK mission", orbitforge::core::Epoch{0},
                                                  orbitforge::core::Epoch{10'000'000'000});
    const auto populated = empty.with_object(orbitforge::runtime::make_circular_demo_session());
    const auto removed = populated.without_object(populated.objects()[0].provenance().object_id);
    if (!removed.objects().empty() || removed.revision() != populated.revision() + 1)
        return 15;
    auto named = project;
    orbitforge::application::ProjectSatellite satellite;
    satellite.orbit.first = project.first;
    satellite.orbit.last = project.last;
    named.satellites.push_back(satellite);
    named = named.renamed_satellite("satellite-1", "Observer")
                .with_satellite_copy("satellite-1", "satellite-2", "Observer copy")
                .without_satellite("satellite-1");
    if (named.satellites.size() != 1 || named.satellites[0].name() != "Observer copy")
        return 16;
    if (!empty.objects().empty() || populated.objects().size() != 1 ||
        populated.sample(std::array{orbitforge::core::Epoch{0}}).size() != 1)
        return 12;
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
        if (orbitforge::runtime::data_catalog_available()) {
            orbitforge::runtime::DataCatalog catalog("sdk-data-catalog");
            const auto entry = catalog.install_eop(argv[1]);
            const auto lock = orbitforge::application::eop_data_lock(eop->data_info());
            if (entry.lock_sha256 != lock.sha256() ||
                catalog.resolve_eop(lock)->data_info().id != eop->data_info().id)
                return 17;
        }
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
    if (orbitforge::runtime::two_body_available() != static_cast<bool>(ORBITFORGE_EXPECT_SPICE))
        return 10;
    if (orbitforge::runtime::two_body_available()) {
        using namespace orbitforge;
        const core::KeplerianElements elements{core::Epoch{1}, 7e6, 0.1, 0.3, 0.2, 0.4, 0.5};
        const auto converted =
            runtime::to_cartesian(elements, core::GravitationalParameter{3.986004418e14});
        const auto recovered =
            runtime::to_keplerian(converted, core::GravitationalParameter{3.986004418e14});
        if (recovered.epoch != elements.epoch || std::abs(recovered.semi_major_axis_m - 7e6) > 1e-6)
            return 13;
        const core::CartesianState initial{
            core::Epoch{0}, core::OriginId::earth, core::FrameId::gcrf, {3, 0, 0}, {0, 2, 0}};
        const auto states = runtime::make_two_body_session(initial, core::GravitationalParameter{6},
                                                           "sdk-two-body", 3)
                                .sample(std::array{core::Epoch{4'000'000'000}});
        if (std::abs(states.states().front().position_m[1] - 6) > 1e-8 ||
            states.provenance().input_revision != 3)
            return 11;
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
