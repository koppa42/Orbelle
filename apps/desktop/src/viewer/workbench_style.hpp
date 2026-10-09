#pragma once
#include <QColor>
#include <QIcon>
class QApplication;
class QDockWidget;
namespace orbitforge::desktop {
enum class WorkbenchIcon {
    new_scenario,
    orbit,
    earth,
    map,
    satellite,
    properties,
    fit,
    focus,
    views,
    play,
    pause,
    first,
    last,
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
QIcon workbench_icon(WorkbenchIcon icon, const QColor &color = QColor("#a7b6c7"));
void style_dock(QDockWidget *dock, WorkbenchIcon icon);
} // namespace orbitforge::desktop
