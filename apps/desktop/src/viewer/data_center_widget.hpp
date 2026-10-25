#pragma once
#include <functional>
class QWidget;
class QTreeWidget;
class QTreeWidgetItem;
namespace orbelle::desktop {
class EopCatalog;
struct DataPreferences;
struct DataCenterContext {
    // The owner keeps the draft alive until the widget is destroyed. A null draft
    // preserves the standalone data inspector's immediate preference editing.
    DataPreferences *draft = nullptr;
    std::function<void()> changed;
    QTreeWidget *navigation = nullptr;
    QTreeWidgetItem *navigation_root = nullptr;
};
QWidget *create_data_center_widget(QWidget *parent, EopCatalog &catalog,
                                   const DataCenterContext &context = {});
} // namespace orbelle::desktop
