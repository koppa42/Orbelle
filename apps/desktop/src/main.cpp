#include <QApplication>
#include <QDockWidget>
#include <QHeaderView>
#include <QMainWindow>
#include <QStatusBar>
#include <QTableWidget>
#include <QTimer>
#include <QTreeWidget>
#include <orbitforge/runtime/demo.hpp>
#include <orbitforge/visualization/snapshot.hpp>
#ifdef ORBITFORGE_DESKTOP_GRAPHICS
#include "viewer/graphics_workbench.hpp"
#endif

int main(int argc, char **argv) {
    QApplication app(argc, argv);
    app.setApplicationName("OrbitForge");
#ifdef ORBITFORGE_DESKTOP_GRAPHICS
    return orbitforge::desktop::run_graphics_workbench(app);
#endif
    const auto session = orbitforge::runtime::make_circular_demo_session();
    const auto series = session.sample(orbitforge::runtime::demo_epochs());
    const auto snapshot = orbitforge::visualization::make_snapshot(series, 0);

    QMainWindow window;
    window.setWindowTitle(QStringLiteral("OrbitForge — 圆轨道示例"));
    window.resize(1080, 640);
    auto *tree = new QTreeWidget;
    tree->setHeaderLabel(QStringLiteral("场景"));
    auto *scenario = new QTreeWidgetItem(tree, {QStringLiteral("圆轨道示例")});
    new QTreeWidgetItem(scenario, {QStringLiteral("demo-satellite")});
    tree->expandAll();
    auto *dock = new QDockWidget(QStringLiteral("对象"), &window);
    dock->setWidget(tree);
    window.addDockWidget(Qt::LeftDockWidgetArea, dock);

    auto *table = new QTableWidget(static_cast<int>(series.states().size()), 7);
    table->setHorizontalHeaderLabels({QStringLiteral("相对历元 (s)"), "X (m)", "Y (m)", "Z (m)",
                                      "Vx (m/s)", "Vy (m/s)", "Vz (m/s)"});
    table->setEditTriggers(QAbstractItemView::NoEditTriggers);
    table->horizontalHeader()->setSectionResizeMode(QHeaderView::Stretch);
    for (std::size_t row = 0; row < series.states().size(); ++row) {
        const auto &state = series.states()[row];
        const std::array<double, 7> values{state.epoch.seconds_since(orbitforge::core::Epoch{0}),
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
    window.statusBar()->showMessage(
        QStringLiteral("地心 GCRF | 历元：2000-01-01 12:00:00 TAI | 6 个采样点"));
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
