#pragma once
#include <QColor>
#include <QIcon>
class QApplication;
class QDockWidget;
namespace orbitforge::desktop {
enum class WorkbenchIcon {
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
    clock
};
void apply_workbench_style(QApplication &app);
QIcon workbench_icon(WorkbenchIcon icon, const QColor &color = QColor("#91aac3"));
void style_dock(QDockWidget *dock, WorkbenchIcon icon);
} // namespace orbitforge::desktop
