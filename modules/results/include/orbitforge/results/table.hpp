#pragma once
#include <cstdint>
#include <orbitforge/core/time/epoch.hpp>
#include <string>
#include <variant>
#include <vector>

namespace orbitforge::results {
// Values retain SI and exact epochs. Formatting never changes numerical results.
using Cell = std::variant<std::monostate, std::string, std::int64_t, double, core::Epoch>;
struct Column {
    std::string id, title, unit;
};
using Row = std::vector<Cell>;
class Table {
  public:
    Table(std::vector<Column> columns, std::vector<Row> rows);
    [[nodiscard]] const std::vector<Column> &columns() const noexcept { return columns_; }
    [[nodiscard]] const std::vector<Row> &rows() const noexcept { return rows_; }

  private:
    std::vector<Column> columns_;
    std::vector<Row> rows_;
};
} // namespace orbitforge::results
