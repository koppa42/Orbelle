#include "application_icon.hpp"
#include "ui_text.hpp"
#include <QApplication>
#include <QDockWidget>
#include <QHeaderView>
#include <QMainWindow>
#include <QStatusBar>
#include <QTableWidget>
#include <QTimer>
#include <QTreeWidget>
#include <orbelle/runtime/demo.hpp>
#include <orbelle/visualization/snapshot.hpp>
#ifdef ORBELLE_DESKTOP_GRAPHICS
#include "viewer/graphics_workbench.hpp"
#endif

namespace ui = orbelle::desktop::ui;

int main(int argc, char **argv) {
    QApplication app(argc, argv);
    app.setApplicationName("Orbelle");
    app.setApplicationVersion(ORBELLE_APPLICATION_VERSION);
    app.setApplicationDisplayName(ui::text("brand.name"));
    app.setWindowIcon(orbelle::desktop::application_icon());
#ifdef ORBELLE_DESKTOP_GRAPHICS
    return orbelle::desktop::run_graphics_workbench(app);
#endif
    const auto session = orbelle::runtime::make_circular_demo_session();
    const auto series = session.sample(orbelle::runtime::demo_epochs());
    const auto snapshot = orbelle::visualization::make_snapshot(series, 0);

    QMainWindow window;
    window.setWindowTitle(ui::text("main.snapshot"));
    window.resize(1080, 640);
    auto *tree = new QTreeWidget;
    tree->setHeaderLabel(ui::text("common.scenario"));
    auto *scenario = new QTreeWidgetItem(tree, {ui::text("main.scenario")});
    new QTreeWidgetItem(scenario, {QStringLiteral("demo-satellite")});
    tree->expandAll();
    auto *dock = new QDockWidget(ui::text("common.objects"), &window);
    dock->setWidget(tree);
    window.addDockWidget(Qt::LeftDockWidgetArea, dock);

    auto *table = new QTableWidget(static_cast<int>(series.states().size()), 7);
    table->setHorizontalHeaderLabels(
        {ui::text("main.table"), "X (m)", "Y (m)", "Z (m)", "Vx (m/s)", "Vy (m/s)", "Vz (m/s)"});
    table->setEditTriggers(QAbstractItemView::NoEditTriggers);
    table->horizontalHeader()->setSectionResizeMode(QHeaderView::Stretch);
    for (std::size_t row = 0; row < series.states().size(); ++row) {
        const auto &state = series.states()[row];
        const std::array<double, 7> values{state.epoch.seconds_since(orbelle::core::Epoch{0}),
                                           state.position_m[0],
                                           state.position_m[1],
                                           state.position_m[2],
                                           state.velocity_mps[0],
                                           state.velocity_mps[1],
                                           state.velocity_mps[2]};
        for (std::size_t col = 0; col < values.size(); ++col) {
            table->setItem(static_cast<int>(row), static_cast<int>(col),
                           new QTableWidgetItem(QString::number(values[col], 'f', 3)));
        }
    }
    window.setCentralWidget(table);
    window.statusBar()->showMessage(ui::text("main.label"));
    window.show();
    if (app.arguments().contains("--smoke-test")) {
        QTimer::singleShot(0, &app, [&]() {
            app.exit(table->rowCount() == 6 && snapshot.markers.size() == 1 &&
                             snapshot.markers[0].position_m[0] == 7e6
                         ? 0
                         : 1);
        });
    }
    return app.exec();
}
