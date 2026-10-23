#pragma once
#include "data_inventory.hpp"
#include <QDate>
#include <functional>
#include <vector>
class QWidget;

namespace orbelle::desktop {
class EopCatalog;
[[nodiscard]] std::vector<DataProduct> initial_data_products();
// UTC must cover today's midnight through tomorrow's midnight (the default scenario).
// Other resources require a complete local file. An invalid day selects today's UTC date.
[[nodiscard]] bool initial_data_ready(const EopCatalog &catalog, DataProduct product,
                                      QDate utc_day = {});
// Blocks entry to the workbench until required resources are ready. The modal
// event loop remains responsive while catalog downloads and validation run in background.
// False means cancellation; completed resources remain cached for the next launch.
bool prepare_initial_data(QWidget *parent, EopCatalog &catalog,
                          const std::vector<DataProduct> &required = initial_data_products(),
                          const std::function<void(DataProduct)> &download = {},
                          const std::function<void()> &visible_ready = {}, QDate utc_day = {});
} // namespace orbelle::desktop
