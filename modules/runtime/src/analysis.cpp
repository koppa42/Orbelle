#include <algorithm>
#include <map>
#include <orbitforge/runtime/analysis.hpp>
#include <orbitforge/runtime/data_catalog.hpp>
#include <orbitforge/runtime/project.hpp>
#include <orbitforge/runtime/report.hpp>
#include <orbitforge/runtime/time_frames.hpp>

namespace orbitforge::runtime {
using namespace application;
namespace {
void check_epoch(const Project &project, core::Epoch epoch) {
    if (epoch.tai_nanoseconds() < project.first.tai_nanoseconds() ||
        epoch.tai_nanoseconds() > project.last.tai_nanoseconds())
        throw std::out_of_range("Query epoch is outside the prepared analysis interval");
}
void check_frame(core::FrameId frame) {
    if (frame != core::FrameId::gcrf && frame != core::FrameId::itrf &&
        frame != core::FrameId::teme)
        throw AnalysisCapabilityError("Analysis states support GCRF, ITRF and TEME");
}
std::uint64_t distance(core::Epoch first, core::Epoch last) {
    return static_cast<std::uint64_t>(last.tai_nanoseconds()) -
           static_cast<std::uint64_t>(first.tai_nanoseconds());
}
std::string frame_name(core::FrameId frame) {
    switch (frame) {
    case core::FrameId::gcrf:
        return "GCRF";
    case core::FrameId::itrf:
        return "ITRF";
    case core::FrameId::teme:
        return "TEME";
    default:
        throw AnalysisCapabilityError("Unsupported state frame");
    }
}
} // namespace
DataSnapshot::DataSnapshot(std::shared_ptr<const core::ITimeProvider> time,
                           std::shared_ptr<const core::IFrameProvider> frames,
                           std::shared_ptr<const core::IEarthOrientationProvider> eop)
    : time_(std::move(time)), frames_(std::move(frames)), eop_(std::move(eop)) {
    if (static_cast<bool>(frames_) != static_cast<bool>(eop_) || (eop_ && !time_))
        throw std::invalid_argument(
            "Frame conversion requires time, EOP and frame providers together");
    if (time_)
        lock_ = time_data_lock(time_->data_info());
    if (eop_) {
        if (time_data_lock(eop_->data_info().time_data) != lock_)
            throw std::invalid_argument("EOP and time providers use different UTC data");
        lock_ = eop_data_lock(eop_->data_info());
    }
}
DataSnapshot DataSnapshot::builtin_time() { return DataSnapshot{make_builtin_time_provider()}; }
DataSnapshot DataSnapshot::load(const std::filesystem::path &manifest,
                                const std::filesystem::path &time_file, int days) {
    if (manifest.empty() && time_file.empty())
        throw std::invalid_argument(
            "Select an EOP or time file, or use DataSnapshot.builtin_time()");
    if (days < 0 || days > core::max_eop_extrapolation_days || (manifest.empty() && days != 0))
        throw std::invalid_argument(
            "EOP extrapolation requires EOP data and a limit of 0..30 days");
    std::shared_ptr<const core::ITimeProvider> time =
        time_file.empty() ? make_builtin_time_provider() : load_time_snapshot(time_file);
    if (manifest.empty())
        return DataSnapshot{std::move(time)};
    std::shared_ptr<const core::IEarthOrientationProvider> eop =
        load_eop_snapshot_with_time(manifest, *time);
    if (days)
        eop = with_eop_extrapolation(std::move(eop), days, time);
    return DataSnapshot{std::move(time), make_frame_provider(), std::move(eop)};
}

struct Analysis::Impl {
    struct Entry {
        Session session;
        core::FrameId frame;
        std::string name, input_sha256;
    };
    ProjectSnapshot snapshot;
    DataSnapshot data;
    std::map<std::string, Entry> entries;
    Impl(ProjectSnapshot s, DataSnapshot d) : snapshot(std::move(s)), data(std::move(d)) {
        const auto &p = *snapshot.project;
        if (data.time()) {
            (void)data.time()->to_utc(p.first);
            (void)data.time()->to_utc(p.last);
        }
        if (data.eop()) {
            const auto &info = data.eop()->data_info();
            for (auto epoch : {p.first, p.last}) {
                if (!core::eop_covers(info, epoch))
                    throw core::EopCoverageError(
                        "Analysis requires TAI ns [" + std::to_string(p.first.tai_nanoseconds()) +
                        ", " + std::to_string(p.last.tai_nanoseconds()) +
                        "]; EOP original range [" +
                        std::to_string(info.first_epoch.tai_nanoseconds()) + ", " +
                        std::to_string(info.last_epoch.tai_nanoseconds()) +
                        "], extrapolation days=" + std::to_string(info.extrapolation_days));
                (void)data.eop()->at(epoch);
            }
        }
        for (const auto &satellite : p.satellites) {
            const auto &input = satellite.orbit;
            if (input.propagator == OrbitPropagator::sgp4 && !data.time())
                throw AnalysisCapabilityError("SGP4 requires explicitly selected UTC time data");
            Project numerical{"analysis-state-input-v1", p.first, p.last};
            numerical.satellites.push_back(satellite);
            numerical.satellites.back().display_name.clear();
            if (input.propagator != OrbitPropagator::sgp4)
                numerical.satellites.back().orbit.tle = {}; // Inactive TLE draft is not an input.
            entries.emplace(input.object_id,
                            Entry{make_orbit_session(input, snapshot.revision, data.time().get()),
                                  input.propagator == OrbitPropagator::sgp4 ? core::FrameId::teme
                                                                            : core::FrameId::gcrf,
                                  satellite.name(), report_input_sha256(numerical)});
        }
        for (const auto &station : p.ground_stations) {
            Project numerical{"analysis-state-input-v1", p.first, p.last};
            numerical.ground_stations.push_back(station);
            numerical.ground_stations.back().display_name.clear();
            numerical.ground_stations.back().minimum_elevation_rad = 0; // Not a state dependency.
            entries.emplace(station.object_id,
                            Entry{make_ground_station_session(station, snapshot.revision),
                                  core::FrameId::itrf, station.name(),
                                  report_input_sha256(numerical)});
        }
    }
    const Entry &entry(const ObjectRef &ref, core::FrameId target) const {
        if (!snapshot.owns(ref))
            throw ProjectError("Object '" + ref.id() + "' belongs to another project");
        if (ref.kind() == ObjectKind::sensor)
            throw AnalysisCapabilityError("Query the parent satellite for a sensor's position");
        const auto it = entries.find(ref.id());
        if (it == entries.end())
            throw ObjectNotFoundError("Object '" + ref.id() + "' is absent from this analysis");
        check_frame(target);
        if (target != it->second.frame && !data.frames())
            throw AnalysisCapabilityError("Prepare with EOP data to convert reference frames");
        return it->second;
    }
    std::string dependency_hash(const Entry &entry, core::FrameId target) const {
        if (target != entry.frame)
            return data.data_lock().sha256();
        if (entry.frame == core::FrameId::teme)
            return time_data_lock(data.time()->data_info()).sha256();
        return DataLock{}.sha256();
    }
};
Analysis::Analysis(ProjectSnapshot snapshot, DataSnapshot data)
    : impl_(std::make_shared<const Impl>(std::move(snapshot), std::move(data))) {}
const ProjectSnapshot &Analysis::snapshot() const { return impl_->snapshot; }
const DataSnapshot &Analysis::data() const { return impl_->data; }
core::CartesianState Analysis::state(const ObjectRef &ref, core::Epoch at,
                                     core::FrameId frame) const {
    return states(ref, std::array{at}, frame).states().front();
}
results::Trajectory Analysis::states(const ObjectRef &ref, std::span<const core::Epoch> times,
                                     core::FrameId frame) const {
    const auto &entry = impl_->entry(ref, frame);
    if (times.size() > max_analysis_samples)
        throw std::invalid_argument("State query exceeds 200000 samples; split the query");
    for (auto epoch : times)
        check_epoch(*impl_->snapshot.project, epoch);
    auto series = entry.session.sample(times);
    auto provenance = series.provenance();
    if (frame != entry.frame) {
        auto values = series.states();
        for (auto &state : values)
            state = core::transform_state(state, frame, *impl_->data.frames(),
                                          impl_->data.eop()->at(state.epoch));
        provenance.algorithm += ";frames=state-frame-routing-v1;target=" + frame_name(frame);
        series = results::StateSeries{std::move(provenance), std::move(values)};
    }
    return results::Trajectory{std::move(series),     entry.name,
                               core::OriginId::earth, frame,
                               entry.input_sha256,    impl_->dependency_hash(entry, frame)};
}
results::Trajectory Analysis::states(const ObjectRef &ref, std::chrono::nanoseconds step,
                                     core::FrameId frame, std::optional<core::Epoch> first,
                                     std::optional<core::Epoch> last) const {
    (void)impl_->entry(ref, frame);
    const auto &p = *snapshot().project;
    const auto begin = first.value_or(p.first), end = last.value_or(p.last);
    check_epoch(p, begin);
    check_epoch(p, end);
    if (step.count() <= 0 || begin.tai_nanoseconds() > end.tai_nanoseconds())
        throw std::invalid_argument("Sampling needs a positive step and an ordered subinterval");
    const auto span = distance(begin, end), stride = static_cast<std::uint64_t>(step.count());
    if (span / stride >= max_analysis_samples)
        throw std::invalid_argument(
            "State query exceeds 200000 samples; increase step or shorten interval");
    const auto count = span / stride + 1 + (span % stride != 0 ? 1 : 0);
    if (count > max_analysis_samples)
        throw std::invalid_argument(
            "State query exceeds 200000 samples; increase step or shorten interval");
    std::vector<core::Epoch> epochs;
    epochs.reserve(static_cast<std::size_t>(count));
    for (auto at = begin;;) {
        epochs.push_back(at);
        if (at == end)
            break;
        at = distance(at, end) <= stride ? end : core::Epoch{at.tai_nanoseconds() + step.count()};
    }
    return states(ref, epochs, frame);
}
DataChangedError::DataChangedError(AnalysisPlan plan)
    : std::runtime_error("Selected calculation data differ from the project's recorded data"),
      plan_(std::make_shared<const AnalysisPlan>(std::move(plan))) {}
AnalysisPlan::AnalysisPlan(const ProjectDocument &project, DataSnapshot data)
    : AnalysisPlan(project, Analysis{project.snapshot(), std::move(data)}) {}
AnalysisPlan::AnalysisPlan(const ProjectDocument &project, const DataCatalog &catalog)
    : AnalysisPlan(project, [&] {
          const auto captured = project.snapshot();
          return Analysis{captured,
                          catalog.snapshot(captured.project->first, captured.project->last)};
      }()) {}
AnalysisPlan::AnalysisPlan(const ProjectDocument &project, Analysis analysis)
    : owner_(project), analysis_(std::move(analysis)) {
    const auto recorded = snapshot().project->data_lock();
    for (const auto &selected : analysis_.data().data_lock().references()) {
        const auto previous =
            std::ranges::find(recorded.references(), selected.role, &DataReference::role);
        if (previous != recorded.references().end() && *previous != selected)
            changes_.push_back({selected.role, *previous, selected});
    }
}
Analysis AnalysisPlan::prepare(bool accept) const {
    if (owner_.snapshot().revision != snapshot().revision)
        throw RevisionConflictError(
            "Project changed since preparation; create a new analysis plan");
    if (!accept && !changes_.empty())
        throw DataChangedError{*this};
    return analysis_;
}
Analysis prepare(const ProjectDocument &project, DataSnapshot data) {
    return AnalysisPlan{project, std::move(data)}.prepare();
}
Analysis prepare(const ProjectDocument &project, const DataCatalog &catalog) {
    return AnalysisPlan{project, catalog}.prepare();
}
namespace {
void record_data(Project &project, const DataSnapshot &data) {
    project.earth_orientation = data.eop() ? std::optional{data.eop()->data_info()} : std::nullopt;
    project.time_data =
        !data.eop() && data.time() ? std::optional{data.time()->data_info()} : std::nullopt;
}
} // namespace
void save_project(const ProjectDocument &project, const std::filesystem::path &path,
                  const Analysis &analysis, bool overwrite) {
    auto candidate = project.snapshot();
    if (candidate.scope != analysis.snapshot().scope)
        throw ProjectError("Analysis belongs to another project");
    const auto hash = analysis_input_sha256(*candidate.project);
    if (hash != analysis_input_sha256(*analysis.snapshot().project))
        throw RevisionConflictError(
            "Calculation inputs changed; prepare a new analysis before saving it");
    auto saved = std::make_shared<Project>(*candidate.project);
    record_data(*saved, analysis.data());
    saved->analysis_record = AnalysisRecord{hash, analysis.data().data_lock().sha256()};
    candidate.project = std::move(saved);
    project.save(path, candidate, overwrite);
}
ProjectOpenPlan::ProjectOpenPlan(const std::filesystem::path &path, DataSnapshot data)
    : ProjectOpenPlan(
          AnalysisPlan{ProjectDocument{application::load_project(path)}, std::move(data)}) {}
ProjectOpenPlan::ProjectOpenPlan(const std::filesystem::path &path, const DataCatalog &catalog)
    : ProjectOpenPlan(AnalysisPlan{ProjectDocument{application::load_project(path)}, catalog}) {}
ProjectOpenPlan::ProjectOpenPlan(AnalysisPlan plan) : plan_(std::move(plan)) {
    const auto &stored = *snapshot().project;
    if (stored.earth_orientation && !data().eop())
        throw AnalysisCapabilityError(
            "Opening this project requires EOP data; supply data=DataCatalog or DataSnapshot");
    if (stored.time_data_info() && !data().time())
        throw AnalysisCapabilityError(
            "Opening this project requires UTC data; supply data=DataCatalog or DataSnapshot");
}
ProjectDocument ProjectOpenPlan::open(bool accept) const {
    if (!accept && !data_changes().empty())
        throw OpenDataChangedError{*this};
    (void)plan_.prepare(accept);
    ProjectDocument result{*snapshot().project};
    if (snapshot().project->data_lock() != data().data_lock()) {
        auto selected = *snapshot().project;
        record_data(selected, data());
        auto edit = result.edit();
        edit.analysis_data(selected.earth_orientation, selected.time_data,
                           selected.analysis_record);
        edit.commit();
    }
    return result;
}
OpenDataChangedError::OpenDataChangedError(ProjectOpenPlan plan)
    : std::runtime_error("Selected calculation data differ from the project's recorded data"),
      plan_(std::make_shared<const ProjectOpenPlan>(std::move(plan))) {}
ProjectDocument open_project(const std::filesystem::path &path, DataSnapshot data) {
    return ProjectOpenPlan{path, std::move(data)}.open();
}
ProjectDocument open_project(const std::filesystem::path &path, const DataCatalog &catalog) {
    return ProjectOpenPlan{path, catalog}.open();
}
void export_trajectory_csv(const results::Trajectory &result, const std::filesystem::path &path,
                           bool overwrite) {
    std::vector<results::Row> rows;
    rows.reserve(result.states().size());
    for (const auto &s : result.states())
        rows.push_back({s.epoch.tai_nanoseconds(), s.position_m[0], s.position_m[1],
                        s.position_m[2], s.velocity_mps[0], s.velocity_mps[1], s.velocity_mps[2],
                        std::string("EARTH"), frame_name(result.frame())});
    results::Table table{{{"epoch_tai_ns", "epoch_tai_ns", ""},
                          {"position.x", "position.x", "m"},
                          {"position.y", "position.y", "m"},
                          {"position.z", "position.z", "m"},
                          {"velocity.x", "velocity.x", "m/s"},
                          {"velocity.y", "velocity.y", "m/s"},
                          {"velocity.z", "velocity.z", "m/s"},
                          {"origin", "origin", ""},
                          {"frame", "frame", ""}},
                         std::move(rows)};
    ReportDefinition definition;
    definition.units = ReportUnits::si;
    const ReportResult report{definition,
                              std::move(table),
                              result.input_sha256(),
                              result.data_sha256(),
                              "",
                              result.provenance().algorithm,
                              core::Epoch{0},
                              core::Epoch{0},
                              {}};
    export_report_csv(report, path, overwrite);
}
} // namespace orbitforge::runtime
