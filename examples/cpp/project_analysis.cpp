#include <iostream>
#include <orbelle/orbelle.hpp>

using namespace std::chrono_literals;

int main() {
    orbelle::Project project{"Observation", orbelle::Epoch{0}, orbelle::Epoch{120'000'000'000}};
    auto edit = project.edit();
    const auto satellite = edit.objects().create(orbelle::objects::Satellite{"Observer", {}, {}});
    const auto station =
        edit.objects().create(orbelle::objects::GroundStation{"Station", {0, 0, 100}, 0});
    const auto sensor =
        edit.objects().create(orbelle::objects::Sensor{"Camera", satellite, 0.2, {station}});
    edit.commit();
    // The default orbit is a GCRF two-body state in metres and metres per second.
    // Pointing and station angles are radians; station height is ellipsoidal metres.
    const auto analysis = project.prepare();
    const auto trajectory = analysis.states(satellite, 30s, orbelle::FrameId::gcrf);
    const auto report = analysis.report(
        {.kind = orbelle::ReportKind::cartesian_state, .object_ids = {satellite.id()}},
        {.columns = {{"time.epoch", "", "TAI"}, {"state.x", "km", "Position X"}},
         .time_scale = orbelle::ReportTimeScale::tai});
    std::cout << project.name() << " (" << sensor.id() << "): " << trajectory.states().size()
              << " states\n";
    for (std::size_t row = 0; row < report.table.rows().size(); ++row)
        std::cout << report.cell_text(row, 0) << ", " << report.cell_text(row, 1) << "\n";
    // project.save("observation.orbelle", analysis);
    // auto reopened = orbelle::Project::open("observation.orbelle");
    // For Earth-fixed queries, explicitly supply a DataSnapshot or DataCatalog
    // to prepare()/open(). Neither operation downloads files.
}
