#include <cmath>
#include <orbelle/results/table.hpp>
#include <set>
#include <stdexcept>

namespace orbelle::results {
Table::Table(std::vector<Column> columns, std::vector<Row> rows) {
    std::set<std::string> ids;
    for (const auto &column : columns)
        if (column.id.empty() || !ids.insert(column.id).second)
            throw std::invalid_argument("Table column IDs must be nonempty and unique");
    for (const auto &row : rows) {
        if (row.size() != columns.size())
            throw std::invalid_argument("Table row does not match its schema");
        for (const auto &cell : row)
            if (const auto *number = std::get_if<double>(&cell); number && !std::isfinite(*number))
                throw std::invalid_argument("Nonfinite table value; use an explicit missing cell");
    }
    storage_ = std::make_shared<const Storage>(Storage{std::move(columns), std::move(rows)});
}
} // namespace orbelle::results
