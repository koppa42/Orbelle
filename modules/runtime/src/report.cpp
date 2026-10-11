#include <algorithm>
#include <chrono>
#include <cmath>
#include <fstream>
#include <iomanip>
#include <locale>
#include <numbers>
#include <orbitforge/runtime/project.hpp>
#include <orbitforge/runtime/report.hpp>
#include <picosha2.h>
#include <random>
#include <set>
#include <sstream>
#include <system_error>
#ifdef _WIN32
#define NOMINMAX
#include <Windows.h>
#endif

namespace orbitforge::runtime {
namespace {
using namespace application;
using results::Column;
using results::Row;
void check(bool ok, const char *message) {
    if (!ok)
        throw std::invalid_argument(message);
}
void cancelled(const std::atomic_bool *value) {
    if (value && value->load())
        throw ReportCancelled{};
}
struct Selection {
    std::string id, name, kind;
    const ProjectGroundStation *station = nullptr;
    const ProjectSatellite *satellite = nullptr;
};
std::vector<Selection> select(const Project &project, const ReportDefinition &d) {
    std::vector<Selection> values;
    std::set<std::string> remaining(d.object_ids.begin(), d.object_ids.end());
    const auto wanted = [&](const std::string &id) {
        return d.object_ids.empty() || remaining.erase(id) != 0;
    };
    if (d.kind != ReportKind::ground_stations)
        for (const auto &s : project.satellites)
            if (wanted(s.orbit.object_id))
                values.push_back({s.orbit.object_id, s.name(), "Satellite", nullptr, &s});
    for (const auto &s : project.ground_stations)
        if (wanted(s.object_id))
            values.push_back({s.object_id, s.name(), "Ground station", &s});
    check(remaining.empty(), "A report object is missing or incompatible with this template");
    return values;
}
std::pair<core::Epoch, core::Epoch> interval(const Project &p, const ReportDefinition &d) {
    if (d.kind != ReportKind::cartesian_state || d.follow_scenario)
        return {p.first, p.last};
    check(d.first.tai_nanoseconds() >= p.first.tai_nanoseconds() &&
              d.last.tai_nanoseconds() <= p.last.tai_nanoseconds(),
          "Report interval is outside the scenario");
    return {d.first, d.last};
}
std::uint64_t ticks_between(core::Epoch first, core::Epoch last) {
    return static_cast<std::uint64_t>(last.tai_nanoseconds()) -
           static_cast<std::uint64_t>(first.tai_nanoseconds());
}
std::string epoch_text(core::Epoch epoch, ReportTimeScale scale, const core::ITimeProvider *time) {
    core::UtcDateTime value{};
    if (scale == ReportTimeScale::utc) {
        check(time != nullptr, "UTC report requires a time provider");
        value = time->to_utc(epoch);
    } else {
        constexpr std::int64_t day = 86400LL * 1'000'000'000;
        auto days = epoch.tai_nanoseconds() / day;
        auto within = epoch.tai_nanoseconds() % day + 43200LL * 1'000'000'000;
        if (scale == ReportTimeScale::tt)
            within += 32'184'000'000;
        if (within < 0) {
            within += day;
            --days;
        }
        if (within >= day) {
            within -= day;
            ++days;
        }
        const std::chrono::year_month_day date{
            std::chrono::sys_days{std::chrono::year{2000} / 1 / 1} + std::chrono::days{days}};
        value = {static_cast<int>(date.year()),
                 static_cast<int>(static_cast<unsigned>(date.month())),
                 static_cast<int>(static_cast<unsigned>(date.day())),
                 static_cast<int>(within / 3'600'000'000'000),
                 static_cast<int>(within / 60'000'000'000 % 60),
                 static_cast<int>(within / 1'000'000'000 % 60),
                 static_cast<int>(within % 1'000'000'000)};
    }
    std::ostringstream out;
    out.imbue(std::locale::classic());
    out << std::setfill('0') << std::setw(4) << value.year << '-' << std::setw(2) << value.month
        << '-' << std::setw(2) << value.day << 'T' << std::setw(2) << value.hour << ':'
        << std::setw(2) << value.minute << ':' << std::setw(2) << value.second << '.'
        << std::setw(9) << value.nanosecond;
    if (scale == ReportTimeScale::utc)
        out << 'Z';
    return out.str();
}
std::string unit(const ReportDefinition &d, std::string value) {
    if (value == "epoch")
        return d.time_scale == ReportTimeScale::utc   ? "UTC"
               : d.time_scale == ReportTimeScale::tai ? "TAI"
                                                      : "TT";
    if (d.units == ReportUnits::engineering) {
        if (value == "m")
            return "km";
        if (value == "m/s")
            return "km/s";
        if (value == "rad")
            return "deg";
    }
    return value;
}
void csv_field(std::ostream &out, const std::string &value) {
    out << '"';
    for (const char c : value) {
        if (c == '"')
            out << '"';
        out << c;
    }
    out << '"';
}
} // namespace
std::string report_input_sha256(const application::Project &project) {
    auto p = project;
    p.reports.clear();
    p.current = p.first;
    for (auto &s : p.satellites) {
        s.display = {};
        s.model_kind = 0;
        s.model_path.clear();
        s.model_diameter_m = 1'000'000;
        s.orbit.input_frame = core::FrameId::gcrf;
        s.orbit.step_seconds = 60;
    }
    const auto bytes = application::encode_project(p);
    return picosha2::hash256_hex_string(bytes);
}
std::size_t estimate_report_rows(const Project &p, const ReportDefinition &d) {
    validate_project(p);
    validate_report_definition(d);
    const auto selected = select(p, d);
    if (d.kind == ReportKind::scenario_summary)
        return 7;
    check(!selected.empty(), "This report has no compatible objects");
    if (d.kind != ReportKind::cartesian_state)
        return selected.size();
    const auto [first, last] = interval(p, d);
    const auto span = ticks_between(first, last),
               step = static_cast<std::uint64_t>(d.step_nanoseconds);
    // Include start and exact stop, appending one shorter final interval when necessary.
    check(span / step < max_report_rows,
          "Report exceeds 200000 rows; increase the step or shorten the interval");
    const auto count = span / step + 1 + (span % step != 0 ? 1 : 0);
    check(count <= max_report_rows / selected.size(),
          "Report exceeds 200000 rows; select fewer objects or increase the step");
    return static_cast<std::size_t>(count * selected.size());
}
ReportResult generate_report(const ReportContext &context, const ReportDefinition &d,
                             const std::atomic_bool *cancel,
                             const std::function<void(std::size_t, std::size_t)> &progress) {
    cancelled(cancel);
    const auto total = estimate_report_rows(context.project, d);
    const auto [first, last] = interval(context.project, d);
    if (context.project.earth_orientation && context.time)
        check(context.time->data_info().sha256 ==
                  context.project.earth_orientation->time_data.sha256,
              "Report time provider differs from the confirmed project data");
    if (context.eop) {
        check(
            context.project.earth_orientation &&
                matching_project_data(*context.project.earth_orientation, context.eop->data_info()),
            "Report EOP provider differs from the confirmed project data");
        check(context.time &&
                  context.time->data_info().sha256 == context.eop->data_info().time_data.sha256,
              "Report time and EOP providers must use the same UTC data");
    }
    if (d.time_scale == ReportTimeScale::utc &&
        (d.kind == ReportKind::cartesian_state || d.kind == ReportKind::scenario_summary)) {
        (void)epoch_text(first, d.time_scale, context.time.get());
        (void)epoch_text(last, d.time_scale, context.time.get());
    }
    std::vector<Column> columns;
    std::vector<Row> rows;
    rows.reserve(total);
    const auto selected = select(context.project, d);
    const auto tick = [&] {
        cancelled(cancel);
        if (progress)
            progress(rows.size(), total);
    };
    if (d.kind == ReportKind::scenario_summary) {
        columns = {{"summary.field", "Property", ""}, {"summary.value", "Value", ""}};
        rows = {{std::string("Scenario"), context.project.name},
                {std::string("Start"), epoch_text(first, d.time_scale, context.time.get())},
                {std::string("Stop"), epoch_text(last, d.time_scale, context.time.get())},
                {std::string("Time scale"), unit(d, "epoch")},
                {std::string("Satellites"),
                 static_cast<std::int64_t>(context.project.satellites.size())},
                {std::string("Ground stations"),
                 static_cast<std::int64_t>(context.project.ground_stations.size())},
                {std::string("Central body"), std::string("Earth")}};
    } else {
        columns = {{"object.id", "Object ID", ""}, {"object.name", "Object", ""}};
        if (d.kind == ReportKind::object_summary) {
            columns.push_back({"object.type", "Type", ""});
            columns.push_back({"object.model", "Model", ""});
            for (const auto &object : selected) {
                const std::string model =
                    object.station
                        ? "WGS84 fixed"
                        : (object.satellite->orbit.propagator == OrbitPropagator::two_body
                               ? "Two body"
                               : "J2 secular");
                rows.push_back({object.id, object.name, object.kind, model});
            }
        } else if (d.kind == ReportKind::ground_stations) {
            columns.insert(columns.end(), {{"station.longitude", "Longitude", "rad"},
                                           {"station.latitude", "Geodetic latitude", "rad"},
                                           {"station.height", "Ellipsoid height", "m"},
                                           {"state.x", "ITRF X", "m"},
                                           {"state.y", "ITRF Y", "m"},
                                           {"state.z", "ITRF Z", "m"}});
            for (const auto &object : selected) {
                cancelled(cancel);
                const auto &location = object.station->location;
                const auto p = core::ground_station_position(location);
                rows.push_back({object.id, object.name, location.longitude_rad,
                                location.latitude_rad, location.height_m, p[0], p[1], p[2]});
            }
        } else {
            columns.insert(columns.end(), {{"time.epoch", "Time", "epoch"},
                                           {"state.frame", "Frame", ""},
                                           {"state.x", "X", "m"},
                                           {"state.y", "Y", "m"},
                                           {"state.z", "Z", "m"},
                                           {"state.vx", "Vx", "m/s"},
                                           {"state.vy", "Vy", "m/s"},
                                           {"state.vz", "Vz", "m/s"}});
            const auto per_object = total / selected.size();
            for (const auto &object : selected) {
                cancelled(cancel);
                const auto session = object.station ? make_ground_station_session(*object.station)
                                                    : make_orbit_session(object.satellite->orbit);
                auto next = first;
                for (std::size_t offset = 0; offset < per_object; offset += 512) {
                    tick();
                    std::vector<core::Epoch> epochs;
                    const auto count = std::min<std::size_t>(512, per_object - offset);
                    for (std::size_t i = 0; i < count; ++i) {
                        epochs.push_back(next);
                        if (ticks_between(next, last) <=
                            static_cast<std::uint64_t>(d.step_nanoseconds))
                            next = last;
                        else
                            next = core::Epoch{next.tai_nanoseconds() + d.step_nanoseconds};
                    }
                    const auto batch = session.sample(epochs);
                    check(batch.states().size() == epochs.size(),
                          "State provider returned the wrong row count");
                    for (std::size_t i = 0; i < epochs.size(); ++i) {
                        cancelled(cancel);
                        auto state = batch.states()[i];
                        check(state.epoch == epochs[i] && state.origin == core::OriginId::earth,
                              "Invalid report state epoch or origin");
                        if (state.frame != d.frame) {
                            check(context.eop && context.frames,
                                  "Reference-frame conversion requires confirmed EOP data");
                            const auto transform = context.frames->gcrf_to_itrf_state(
                                state.epoch, context.eop->at(state.epoch));
                            state = d.frame == core::FrameId::itrf ? transform.apply(state)
                                                                   : transform.inverse(state);
                        }
                        const auto &p = state.position_m, &v = state.velocity_mps;
                        rows.push_back(
                            {object.id, object.name, state.epoch,
                             std::string(d.frame == core::FrameId::gcrf ? "GCRF" : "ITRF"), p[0],
                             p[1], p[2], v[0], v[1], v[2]});
                    }
                }
            }
        }
    }
    tick();
    return {d,
            results::Table{std::move(columns), std::move(rows)},
            report_input_sha256(context.project),
            context.project.data_lock().sha256(),
            context.time ? context.time->data_info().sha256 : "",
            "builtin-reports-v1",
            first,
            last,
            context.time};
}
std::string ReportResult::column_title(std::size_t index) const {
    const auto &column = table.columns().at(index);
    const auto suffix = unit(definition, column.unit);
    return column.title + (suffix.empty() ? "" : " (" + suffix + ")");
}
std::string ReportResult::cell_text(std::size_t row, std::size_t column) const {
    const auto &cell = table.rows().at(row).at(column);
    if (const auto *text = std::get_if<std::string>(&cell))
        return *text;
    if (const auto *integer = std::get_if<std::int64_t>(&cell))
        return std::to_string(*integer);
    if (const auto *epoch = std::get_if<core::Epoch>(&cell))
        return epoch_text(*epoch, definition.time_scale, time.get());
    if (const auto *real = std::get_if<double>(&cell)) {
        auto value = *real;
        if (definition.units == ReportUnits::engineering) {
            const auto &dimension = table.columns().at(column).unit;
            if (dimension == "m" || dimension == "m/s")
                value /= 1000;
            if (dimension == "rad")
                value *= 180 / std::numbers::pi;
        }
        std::ostringstream out;
        out.imbue(std::locale::classic());
        out << std::setprecision(17) << value;
        return out.str();
    }
    return {};
}
void export_report_csv(const ReportResult &result, const std::filesystem::path &path,
                       bool overwrite, const std::atomic_bool *cancel) {
    cancelled(cancel);
    const auto destination = std::filesystem::absolute(path);
    check(!std::filesystem::exists(destination) || overwrite, "CSV destination already exists");
    std::filesystem::path staging;
    std::random_device random;
    for (int attempt = 0; attempt < 32; ++attempt) {
        const auto candidate =
            destination.parent_path() / (".orbitforge-report-" + std::to_string(random()));
        if (std::filesystem::create_directory(candidate)) {
            staging = candidate;
            break;
        }
    }
    check(!staging.empty(), "Cannot create temporary CSV directory");
    const auto temporary = staging / "report.csv";
    try {
        std::ofstream out(temporary, std::ios::binary);
        out.exceptions(std::ios::badbit | std::ios::failbit);
        out << "\xEF\xBB\xBF";
        for (std::size_t c = 0; c < result.table.columns().size(); ++c) {
            if (c)
                out << ',';
            csv_field(out, result.column_title(c));
        }
        out << "\r\n";
        for (std::size_t r = 0; r < result.table.rows().size(); ++r) {
            cancelled(cancel);
            for (std::size_t c = 0; c < result.table.columns().size(); ++c) {
                if (c)
                    out << ',';
                csv_field(out, result.cell_text(r, c));
            }
            out << "\r\n";
        }
        out.close();
        cancelled(cancel);
#ifdef _WIN32
        if (!MoveFileExW(temporary.c_str(), destination.c_str(),
                         MOVEFILE_WRITE_THROUGH | (overwrite ? MOVEFILE_REPLACE_EXISTING : 0)))
            throw std::system_error(static_cast<int>(GetLastError()), std::system_category(),
                                    "Cannot publish CSV");
#else
        if (overwrite)
            std::filesystem::rename(temporary, destination);
        else {
            std::filesystem::create_hard_link(temporary, destination);
            std::filesystem::remove(temporary);
        }
#endif
        std::filesystem::remove(staging);
    } catch (...) {
        std::error_code ignored;
        std::filesystem::remove(temporary, ignored);
        std::filesystem::remove(staging, ignored);
        throw;
    }
}
} // namespace orbitforge::runtime
