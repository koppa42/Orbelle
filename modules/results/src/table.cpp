#include <cmath>
#include <orbitforge/results/table.hpp>
#include <set>
#include <stdexcept>

namespace orbitforge::results {
Table::Table(std::vector<Column> columns, std::vector<Row> rows)
    : columns_(std::move(columns)), rows_(std::move(rows)) {
    std::set<std::string> ids;
    for (const auto &column : columns_)
        if (column.id.empty() || !ids.insert(column.id).second)
            throw std::invalid_argument("Table column IDs must be nonempty and unique");
    for (const auto &row : rows_) {
        if (row.size() != columns_.size())
            throw std::invalid_argument("Table row does not match its schema");
        for (const auto &cell : row)
            if (const auto *number = std::get_if<double>(&cell); number && !std::isfinite(*number))
                throw std::invalid_argument("Nonfinite table value; use an explicit missing cell");
    }
}
} // namespace orbitforge::results
