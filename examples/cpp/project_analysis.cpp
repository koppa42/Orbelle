#include <iostream>
#include <orbelle/orbelle.hpp>

namespace of = orbelle;
using namespace std::chrono_literals;

int main() {
    of::Project project{"Observation", of::Epoch{0}, of::Epoch{120'000'000'000}};
    auto edit = project.edit();
    const auto satellite = edit.objects().create(of::objects::Satellite{"Observer", {}, {}});
    const auto station =
        edit.objects().create(of::objects::GroundStation{"Station", {0, 0, 100}, 0});
    const auto sensor =
        edit.objects().create(of::objects::Sensor{"Camera", satellite, 0.2, {station}});
    edit.commit();
    // The default orbit is a GCRF two-body state in metres and metres per second.
    // Pointing and station angles are radians; station height is ellipsoidal metres.
    const auto analysis = project.prepare();
    const auto trajectory = analysis.states(satellite, 30s, of::FrameId::gcrf);
    const auto report =
        analysis.report({.kind = of::ReportKind::cartesian_state, .object_ids = {satellite.id()}},
                        {.columns = {{"time.epoch", "", "TAI"}, {"state.x", "km", "Position X"}},
                         .time_scale = of::ReportTimeScale::tai});
    std::cout << project.name() << " (" << sensor.id() << "): " << trajectory.states().size()
              << " states\n";
    for (std::size_t row = 0; row < report.table.rows().size(); ++row)
        std::cout << report.cell_text(row, 0) << ", " << report.cell_text(row, 1) << "\n";
    // project.save("observation.orbelle", analysis);
    // auto reopened = of::Project::open("observation.orbelle");
    // For Earth-fixed queries, explicitly supply a DataSnapshot or DataCatalog
    // to prepare()/open(). Neither operation downloads files.
}
