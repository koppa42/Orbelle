#pragma once
#include <QColor>
#include <QIcon>
class QApplication;
class QDockWidget;
namespace orbitforge::desktop {
enum class WorkbenchIcon {
    new_scenario,
    folder,
    save,
    download,
    database,
    orbit,
    earth,
    map,
    satellite,
    ground_station,
    properties,
    fit,
    focus,
    views,
    play,
    pause,
    first,
    last,
    slower,
    faster,
    search,
    close,
    detach,
    calendar,
    clock,
    zoom_in,
    zoom_out,
    time_fit,
    chevron_up,
    chevron_down
};
void apply_workbench_style(QApplication &app);
QIcon workbench_icon(WorkbenchIcon icon, const QColor &color = QColor("#b6ccdc"));
void style_dock(QDockWidget *dock, WorkbenchIcon icon);
} // namespace orbitforge::desktop
