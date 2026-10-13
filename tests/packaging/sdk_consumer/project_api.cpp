#include <orbitforge/orbitforge.hpp>

namespace of = orbitforge;
using namespace std::chrono_literals;

int main(int argc, char **argv) {
    if (argc != 2)
        return 1;
    const std::filesystem::path path{argv[1]};
    of::Project project{"SDK facade", of::Epoch{0}, of::Epoch{120'000'000'000}};
    auto edit = project.edit();
    const auto station = edit.objects().create(
        of::objects::GroundStation{.name = "Station", .location = {0, 0, 100}});
    edit.commit();
    const auto analysis = project.prepare(of::DataSnapshot::from_providers({}));
    const auto trajectory = analysis.states(station, 30s, of::FrameId::itrf);
    const auto report = analysis.report(
        {.kind = of::ReportKind::cartesian_state,
         .object_ids = {station.id()},
         .frame = of::FrameId::itrf},
        {.columns = {{"state.x", "km", "Position X"}}, .time_scale = of::ReportTimeScale::tai});
    if (trajectory.states().size() != 5 ||
        report.numeric_value(0, 0) != trajectory.states().front().position_m[0] / 1000)
        return 2;
    project.objects().rename(station, "Renamed");
    project.save(path, analysis, true);
    auto plan = of::Project::plan_open(path);
    auto reopened = plan.open();
    if (project.dirty() || reopened.dirty() || reopened.name() != "SDK facade" ||
        reopened.objects().find("Renamed").size() != 1 ||
        reopened.objects().get(station.id()).kind() != of::ObjectKind::ground_station ||
        reopened.analysis_status() != of::AnalysisStatus::current)
        return 3;
    const auto current = reopened.prepare().state(reopened.objects().get(station.id()),
                                                  reopened.first(), of::FrameId::itrf);
    if (current.position_m != trajectory.states().front().position_m)
        return 4;
    of::OrbitInput numerical;
    numerical.propagator = of::OrbitPropagator::hpop;
    numerical.hpop.gravity_degree = numerical.hpop.gravity_order = 0;
    numerical.hpop.integrator = of::HpopIntegrator::rkf78;
    const auto satellite =
        reopened.objects().create(of::objects::Satellite{.name = "HPOP", .orbit = numerical});
    const auto numerical_analysis = reopened.prepare();
    const auto numerical_result = numerical_analysis.states(satellite, 30s, of::FrameId::gcrf);
    if (numerical_result.states().size() != 5 ||
        numerical_result.provenance().algorithm.find("hpop-cowell") == std::string::npos)
        return 5;
    reopened.save(path, numerical_analysis, true);
    auto numerical_file = of::Project::open(path);
    if (numerical_file.to_data().satellites.at(0).orbit.hpop != numerical.hpop)
        return 6;
    std::filesystem::remove(path);
}
