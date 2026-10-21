#pragma once
#include <algorithm>
#include <map>
#include <orbitforge/runtime/report.hpp>

namespace orbitforge::desktop {
// Relabel an immutable result without executing its query. Return no value when
// nothing changed; source IDs also resolve names when a template hides the ID column.
inline std::shared_ptr<runtime::ReportResult> renamed_report(const runtime::ReportResult &result,
                                                             const application::Project &project) {
    if (application::report_is_coverage(result.definition.query.kind))
        return {}; // Coverage rows describe the report/grid, not individual objects.
    std::map<std::string, std::string> names;
    for (const auto &s : project.satellites)
        names.emplace(s.orbit.object_id, s.name());
    for (const auto &s : project.ground_stations)
        names.emplace(s.object_id, s.name());
    for (const auto &s : project.sensors)
        names.emplace(s.object_id, s.name());
    const auto &query = result.definition.query;
    if ((application::report_is_access(query.kind) ||
         application::report_is_intersatellite(query.kind)) &&
        query.object_ids.size() == 2 && names.contains(query.object_ids[0]) &&
        names.contains(query.object_ids[1]))
        names[query.object_ids[1]] =
            names.at(query.object_ids[0]) + " → " + names.at(query.object_ids[1]);
    const auto &source = result.source_table ? *result.source_table : result.table;
    const auto &columns = source.columns();
    const auto id = std::ranges::find(columns, "object.id", &results::Column::id);
    const auto name = std::ranges::find(columns, "object.name", &results::Column::id);
    if (id == columns.end() || name == columns.end())
        return {};
    const auto id_index = static_cast<std::size_t>(id - columns.begin());
    const auto name_index = static_cast<std::size_t>(name - columns.begin());
    const auto replacement = [&](const results::Row &row) -> const std::string * {
        const auto *key = std::get_if<std::string>(&row[id_index]);
        const auto found = key ? names.find(*key) : names.end();
        return found == names.end() ? nullptr : &found->second;
    };
    if (std::ranges::none_of(source.rows(), [&](const auto &row) {
            const auto *value = replacement(row);
            return value && row[name_index] != results::Cell{*value};
        }))
        return {};
    auto rows = source.rows();
    for (auto &row : rows)
        if (const auto *value = replacement(row))
            row[name_index] = *value;
    auto updated = result;
    updated.source_table = std::make_shared<const results::Table>(columns, std::move(rows));
    return std::make_shared<runtime::ReportResult>(updated.with_template(result.definition.format));
}
} // namespace orbitforge::desktop
