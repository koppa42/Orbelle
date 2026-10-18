#include <algorithm>
#include <orbitforge/application/report.hpp>
#include <set>
#include <stdexcept>

namespace orbitforge::application {
std::vector<results::Column> report_columns(ReportKind kind) {
    if (kind == ReportKind::scenario_summary)
        return {{"summary.field", "Property", ""}, {"summary.value", "Value", ""}};
    std::vector<results::Column> columns{{"object.id", "Object ID", ""},
                                         {"object.name", "Object", ""}};
    if (kind == ReportKind::object_summary) {
        columns.insert(columns.end(), {{"object.type", "Type", ""}, {"object.model", "Model", ""}});
    } else if (kind == ReportKind::ground_stations) {
        columns.insert(columns.end(), {{"station.longitude", "Longitude", "rad"},
                                       {"station.latitude", "Geodetic latitude", "rad"},
                                       {"station.height", "Ellipsoid height", "m"},
                                       {"state.x", "ITRF X", "m"},
                                       {"state.y", "ITRF Y", "m"},
                                       {"state.z", "ITRF Z", "m"}});
    } else if (kind == ReportKind::orbital_events) {
        columns.insert(columns.end(), {{"time.epoch", "Time", "epoch"},
                                       {"event.kind", "Event", ""},
                                       {"motion.radius", "Geocentric distance", "m"},
                                       {"motion.speed", "Speed", "m/s"}});
    } else if (report_is_lighting(kind)) {
        columns.push_back(
            {"time.epoch", kind == ReportKind::lighting_intervals ? "Start" : "Time", "epoch"});
        if (kind == ReportKind::lighting_intervals)
            columns.insert(columns.end(), {{"lighting.stop", "Stop", "epoch"},
                                           {"lighting.duration", "Duration", "s"},
                                           {"lighting.phase", "Lighting", ""},
                                           {"lighting.start_clipped", "Start clipped", ""},
                                           {"lighting.stop_clipped", "Stop clipped", ""}});
        else
            columns.insert(columns.end(), {{"lighting.fraction", "Solar disk illumination", "1"},
                                           {"lighting.phase", "Lighting", ""}});
    } else if (report_is_access(kind)) {
        columns.push_back({"time.epoch", kind == ReportKind::aer ? "Time" : "Start", "epoch"});
        if (kind == ReportKind::access_intervals)
            columns.insert(columns.end(),
                           {{"access.stop", "Stop", "epoch"},
                            {"access.duration", "Duration", "s"},
                            {"access.start_clipped", "Start clipped", ""},
                            {"access.stop_clipped", "Stop clipped", ""},
                            {"access.max_elevation", "Maximum elevation", "rad"},
                            {"access.max_elevation_time", "Maximum elevation time", "epoch"},
                            {"access.min_range", "Minimum range", "m"},
                            {"access.min_range_time", "Minimum range time", "epoch"}});
        else
            columns.insert(columns.end(), {{"aer.azimuth", "Azimuth", "rad"},
                                           {"aer.elevation", "Elevation", "rad"},
                                           {"aer.range", "Range", "m"},
                                           {"aer.range_rate", "Range rate", "m/s"},
                                           {"access.visible", "Visible", ""},
                                           {"sensor.off_boresight", "Off boresight", "rad"}});
    } else {
        columns.insert(columns.end(),
                       {{"time.epoch", "Time", "epoch"}, {"state.frame", "Frame", ""}});
        if (kind == ReportKind::classical_elements)
            columns.insert(columns.end(), {{"elements.a", "Semi-major axis", "m"},
                                           {"elements.e", "Eccentricity", ""},
                                           {"elements.i", "Inclination", "rad"},
                                           {"elements.raan", "RAAN", "rad"},
                                           {"elements.argp", "Argument of periapsis", "rad"},
                                           {"elements.nu", "True anomaly", "rad"}});
        else if (kind == ReportKind::geodetic_position)
            columns.insert(columns.end(), {{"geodetic.longitude", "Longitude", "rad"},
                                           {"geodetic.latitude", "Geodetic latitude", "rad"},
                                           {"geodetic.height", "Ellipsoid height", "m"}});
        else if (kind == ReportKind::orbital_motion)
            columns.insert(columns.end(),
                           {{"motion.radius", "Geocentric distance", "m"},
                            {"motion.speed", "Speed", "m/s"},
                            {"motion.radial_speed", "Radial speed", "m/s"},
                            {"motion.transverse_speed", "Transverse speed", "m/s"},
                            {"motion.flight_path_angle", "Flight path angle", "rad"}});
        else if (kind == ReportKind::state_covariance) {
            const std::array<std::string, 6> axes{"x", "y", "z", "vx", "vy", "vz"};
            for (std::size_t i = 0; i < axes.size(); ++i)
                for (std::size_t j = 0; j <= i; ++j)
                    columns.push_back({"covariance." + axes[i] + "." + axes[j],
                                       "Cov(" + axes[i] + "," + axes[j] + ")",
                                       i < 3   ? "m^2"
                                       : j < 3 ? "m^2/s"
                                               : "m^2/s^2"});
        } else if (kind == ReportKind::cartesian_state)
            columns.insert(columns.end(), {{"state.x", "X", "m"},
                                           {"state.y", "Y", "m"},
                                           {"state.z", "Z", "m"},
                                           {"state.vx", "Vx", "m/s"},
                                           {"state.vy", "Vy", "m/s"},
                                           {"state.vz", "Vz", "m/s"}});
        else
            throw std::invalid_argument("Invalid report kind");
    }
    return columns;
}
bool report_uses_utc(const ReportTemplate &format, ReportKind kind) {
    const auto schema = report_columns(kind);
    for (const auto &field : schema) {
        const auto spec = std::ranges::find(format.columns, field.id, &ReportColumnSpec::field);
        if (!format.columns.empty() && spec == format.columns.end())
            continue;
        if (field.unit == "epoch") {
            if (spec != format.columns.end() && !spec->unit.empty()) {
                if (spec->unit == "UTC")
                    return true;
            } else if (format.time_scale == ReportTimeScale::utc)
                return true;
        }
        if (kind == ReportKind::scenario_summary && field.id == "summary.value" &&
            format.time_scale == ReportTimeScale::utc)
            return true;
    }
    return false;
}
void validate_report_query(const ReportQuery &d) {
    if (d.kind < ReportKind::scenario_summary || d.kind > ReportKind::orbital_events ||
        (d.scope != ReportScope::objects && d.scope != ReportScope::all_compatible) ||
        (d.frame != core::FrameId::gcrf && d.frame != core::FrameId::itrf) ||
        d.step_nanoseconds < 1 || d.step_nanoseconds > 86400LL * 1'000'000'000 ||
        (!d.follow_scenario && d.first.tai_nanoseconds() > d.last.tai_nanoseconds()) ||
        d.object_ids.size() > 10000)
        throw std::invalid_argument("Invalid report query");
    if (d.scope == ReportScope::all_compatible && !d.object_ids.empty())
        throw std::invalid_argument("All-object report scope cannot include object IDs");
    if (d.kind == ReportKind::scenario_summary && !d.object_ids.empty())
        throw std::invalid_argument("Scenario summary does not accept object bindings");
    if (d.kind != ReportKind::scenario_summary && d.scope == ReportScope::objects &&
        d.object_ids.empty())
        throw std::invalid_argument(
            "Report object selection cannot be empty; select all explicitly");
    if (report_is_access(d.kind) && (d.scope != ReportScope::objects || d.object_ids.size() != 2 ||
                                     d.step_nanoseconds < 1'000'000))
        throw std::invalid_argument("Access requires a station/target pair and step >= 1 ms");
    if (d.event_tolerance_nanoseconds < 1 ||
        d.event_tolerance_nanoseconds > 86400LL * 1'000'000'000 ||
        (d.event_search_mode != core::AccessSearchMode::fixed_grid &&
         d.event_search_mode != core::AccessSearchMode::extremum_assisted) ||
        (report_uses_event_search(d.kind) && d.event_tolerance_nanoseconds > d.step_nanoseconds))
        throw std::invalid_argument("Invalid event search mode or boundary tolerance");
    if (d.lighting_ephemeris.empty() || d.lighting_ephemeris.size() > 16384)
        throw std::invalid_argument("Lighting ephemeris selection is required");
    std::set<std::string> ids;
    for (const auto &id : d.object_ids)
        if (id.empty() || id.size() > 16384 || !ids.insert(id).second)
            throw std::invalid_argument("Invalid or duplicate report object ID");
}
void validate_report_template(const ReportTemplate &format, ReportKind kind) {
    if (format.units < ReportUnits::si || format.units > ReportUnits::engineering ||
        format.time_scale < ReportTimeScale::utc || format.time_scale > ReportTimeScale::tt ||
        format.columns.size() > 64)
        throw std::invalid_argument("Invalid report template");
    const auto schema = report_columns(kind);
    std::set<std::string> fields;
    for (const auto &column : format.columns) {
        const auto field = std::ranges::find(schema, column.field, &results::Column::id);
        if (field == schema.end() || !fields.insert(column.field).second ||
            column.title.size() > 16384)
            throw std::invalid_argument("Unknown or duplicate report field, or invalid title");
        const auto &unit = column.unit;
        const auto &dimension = field->unit;
        if (!unit.empty() && unit != dimension && !(dimension == "1" && unit == "%") &&
            !(dimension == "m" && unit == "km") && !(dimension == "m/s" && unit == "km/s") &&
            !(dimension == "rad" && unit == "deg") && !(dimension == "m^2" && unit == "km^2") &&
            !(dimension == "m^2/s" && unit == "km^2/s") &&
            !(dimension == "m^2/s^2" && unit == "km^2/s^2") &&
            !(dimension == "epoch" && (unit == "UTC" || unit == "TAI" || unit == "TT")))
            throw std::invalid_argument("Incompatible report field unit");
        if (unit == "epoch")
            throw std::invalid_argument("Choose UTC, TAI or TT for epoch display");
    }
}
void validate_report_definition(const ReportDefinition &d) {
    if (d.id.empty() || d.name.empty() || d.id.size() > 16384 || d.name.size() > 16384)
        throw std::invalid_argument("Invalid report definition");
    validate_report_query(d.query);
    validate_report_template(d.format, d.query.kind);
}
} // namespace orbitforge::application
