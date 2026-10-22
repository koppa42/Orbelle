#pragma once
#include <cstdint>
#include <memory>
#include <orbelle/core/time/epoch.hpp>
#include <string>
#include <variant>
#include <vector>

namespace orbelle::results {
// Values retain SI and exact epochs. Formatting never changes numerical results.
using Cell = std::variant<std::monostate, std::string, std::int64_t, double, core::Epoch>;
struct Column {
    std::string id, title, unit;
};
using Row = std::vector<Cell>;
class Table {
  public:
    Table(std::vector<Column> columns, std::vector<Row> rows);
    [[nodiscard]] const std::vector<Column> &columns() const noexcept { return storage_->columns; }
    [[nodiscard]] const std::vector<Row> &rows() const noexcept { return storage_->rows; }

  private:
    struct Storage {
        std::vector<Column> columns;
        std::vector<Row> rows;
    };
    std::shared_ptr<const Storage> storage_;
};
} // namespace orbelle::results
