#include "../src/viewer/epoch_editor.hpp"
#include "../src/viewer/report_dialog.hpp"
#include "../src/viewer/report_plot.hpp"
#include "../src/viewer/workbench_style.hpp"
#include "../src/viewer/workspace.hpp"
#include <QAction>
#include <QApplication>
#include <QCheckBox>
#include <QClipboard>
#include <QComboBox>
#include <QDialogButtonBox>
#include <QDir>
#include <QDoubleSpinBox>
#include <QElapsedTimer>
#include <QFileDialog>
#include <QInputDialog>
#include <QKeyEvent>
#include <QLabel>
#include <QLineEdit>
#include <QListWidget>
#include <QMimeData>
#include <QMouseEvent>
#include <QPlainTextEdit>
#include <QPushButton>
#include <QScrollArea>
#include <QScrollBar>
#include <QStandardItemModel>
#include <QTabBar>
#include <QTableView>
#include <QTemporaryDir>
#include <QThread>
#include <QTimer>
#include <QToolButton>
#include <QWheelEvent>
#include <gtest/gtest.h>
#include <orbitforge/runtime/time_frames.hpp>
#include <orbitforge/runtime/two_body.hpp>

namespace orbitforge::desktop {
namespace {
QApplication &test_app() {
    static int argc = 1;
    static char name[] = "report_ui_test";
    static char *argv[] = {name, nullptr};
    static QApplication app(argc, argv);
    apply_workbench_style(app);
    return app;
}
void save_template(ReportDialog &dialog, const QString &name) {
    QTimer::singleShot(0, [name] {
        auto *naming = qobject_cast<QInputDialog *>(QApplication::activeModalWidget());
        ASSERT_TRUE(naming);
        naming->setTextValue(name);
        naming->accept();
    });
    dialog.findChild<QAction *>("saveReport")->trigger();
}
TEST(ReportDialog, AsyncGenerationSavedBindingsStaleResultsAndCancellation) {
    (void)test_app();
    const auto restore_clipboard = [](QMimeData *data) {
        QApplication::clipboard()->setMimeData(data);
    };
    std::unique_ptr<QMimeData, decltype(restore_clipboard)> clipboard(new QMimeData,
                                                                      restore_clipboard);
    if (const auto *original = QApplication::clipboard()->mimeData())
        for (const auto &format : original->formats())
            clipboard->setData(format, original->data(format));
    std::shared_ptr<const core::ITimeProvider> time = runtime::make_builtin_time_provider();
    application::Project project("报告验证", core::Epoch{0}, core::Epoch{120'000'000'001});
    project.ground_stations.push_back({"station", "沿海站", {1.2, 0.6, 350}});
    ReportDialog::Ready pending;
    std::optional<core::Epoch> sought;
    Workspace workspace;
    ReportDialog dialog(
        project, time, [&](ReportDialog::Ready ready) { pending = std::move(ready); },
        [&](auto reports) { project.reports = std::move(reports); },
        [&](core::Epoch epoch) { sought = epoch; });
    dialog.show();
    auto *library = dialog.findChild<QComboBox *>("reportLibrary");
    auto *generate = dialog.findChild<QPushButton *>("generateReport");
    auto *cancel = dialog.findChild<QPushButton *>("cancelReport");
    auto *table = dialog.findChild<QTableView *>("reportTable");

    const auto wait = [&] {
        QElapsedTimer timer;
        timer.start();
        while (!generate->isEnabled() && timer.elapsed() < 5000) {
            QCoreApplication::processEvents();
            QThread::msleep(2);
        }
        EXPECT_TRUE(generate->isEnabled());
    };
    const auto capture = [&](const QString &file) {
        for (int i = 0; i < 3; ++i)
            QCoreApplication::processEvents();
        const auto directory = qEnvironmentVariable("ORBITFORGE_REPORT_UI_CAPTURE");
        if (!directory.isEmpty()) {
            ASSERT_TRUE(QDir().mkpath(directory));
            ASSERT_TRUE(dialog.grab().save(directory + "/" + file + ".png"));
        }
        for (auto *scroll : dialog.findChildren<QScrollArea *>())
            if (scroll->isVisible())
                EXPECT_EQ(scroll->horizontalScrollBar()->maximum(), 0);
        if (dialog.findChild<QWidget *>("reportSettingsPanel")->isVisible()) {
            EXPECT_TRUE(generate->isVisible());
            EXPECT_TRUE(
                dialog.rect().contains(generate->mapTo(&dialog, generate->rect().bottomRight())));
        } else {
            EXPECT_TRUE(table->isVisible());
            EXPECT_GT(table->height(), 250);
        }
    };
    library->setCurrentIndex(2);
    dialog.findChild<QComboBox *>("reportFrame")->setCurrentIndex(1);
    capture("configuration");
    dialog.findChild<QCheckBox *>("reportFollowScenario")->setChecked(false);
    capture("custom-time-configuration");
    static_cast<EpochEditor *>(dialog.findChild<QWidget *>("reportFirst"))
        ->set_epoch(core::Epoch{60'000'000'000});
    dialog.findChild<QCheckBox *>("reportFollowScenario")->setChecked(true);
    EXPECT_TRUE(dialog.findChild<QLabel *>("reportInterval")->text().contains("11:59:28"));
    generate->click();
    ASSERT_TRUE(pending);
    EXPECT_FALSE(generate->isEnabled());
    pending(runtime::ReportContext{project, time, {}, {}}, {});
    pending = {};
    wait();
    ASSERT_EQ(table->model()->rowCount(), 4);
    EXPECT_TRUE(table->isVisible());
    EXPECT_TRUE(dialog.findChild<QWidget *>("reportSettingsPanel")->isVisible());
    EXPECT_GT(table->width(), dialog.findChild<QWidget *>("reportSettingsPanel")->width());
    EXPECT_TRUE(table->model()->headerData(2, Qt::Horizontal).toString().contains("UTC"));
    table->doubleClicked(table->model()->index(1, 2));
    ASSERT_TRUE(sought);
    EXPECT_EQ(sought->tai_nanoseconds(), 60'000'000'000);
    capture("results");
    table->selectRow(0);
    dialog.findChild<QAction *>("copyReportRows")->trigger();
    EXPECT_TRUE(QApplication::clipboard()->text().contains(QStringLiteral("沿海站")));
    EXPECT_TRUE(QApplication::clipboard()->text().contains("2000-01-01T11:59:28.000000000Z"));
    dialog.resize(370, 600);
    capture("narrow-results");
    EXPECT_FALSE(dialog.findChild<QWidget *>("reportSettingsPanel")->isVisible());
    dialog.findChild<QPushButton *>("reportToggleSettings")->click();
    capture("narrow-configuration");
    dialog.resize(1080, 730);
    save_template(dialog, QStringLiteral("沿海站位置"));
    ASSERT_EQ(project.reports.size(), 1U);
    EXPECT_EQ(project.reports[0].name, "沿海站位置");
    EXPECT_TRUE(project.reports[0].object_ids.empty());
    const auto saved = application::decode_project(application::encode_project(project));
    EXPECT_EQ(saved.reports[0].frame, core::FrameId::itrf);
    QTimer::singleShot(0, [&] {
        auto *source = qobject_cast<QDialog *>(QApplication::activeModalWidget());
        ASSERT_TRUE(source);
        EXPECT_TRUE(source->findChild<QPlainTextEdit *>("reportSource")
                        ->toPlainText()
                        .contains("builtin-reports-v2"));
        const auto directory = qEnvironmentVariable("ORBITFORGE_REPORT_UI_CAPTURE");
        if (!directory.isEmpty())
            EXPECT_TRUE(source->grab().save(directory + "/provenance.png"));
        source->accept();
    });
    dialog.findChild<QAction *>("reportShowSource")->trigger();
    project.ground_stations[0].location.height_m = 450;
    dialog.update_project(project, time);
    EXPECT_TRUE(dialog.findChild<QLabel *>("reportResultLabel")
                    ->text()
                    .contains(QStringLiteral("场景已变化")));
    // A cancelled preflight must not start a late calculation when its callback arrives.
    generate->click();
    ASSERT_TRUE(pending);
    cancel->click();
    pending(runtime::ReportContext{project, time, {}, {}}, {});
    pending = {};
    EXPECT_TRUE(generate->isEnabled());
    EXPECT_EQ(table->model()->rowCount(), 4);
    generate->click();
    ASSERT_TRUE(pending);
    pending({}, QStringLiteral("取消使用新数据"));
    pending = {};
    EXPECT_TRUE(generate->isEnabled());
    EXPECT_EQ(table->model()->rowCount(), 4);
    // A failing calculation retains the old table.
    dialog.findChild<QComboBox *>("reportFrame")->setCurrentIndex(0);
    generate->click();
    ASSERT_TRUE(pending);
    pending(runtime::ReportContext{project, time, {}, {}}, {});
    pending = {};
    wait();
    EXPECT_EQ(table->model()->rowCount(), 4);
    EXPECT_TRUE(
        dialog.findChild<QLabel *>("reportStatus")->text().contains(QStringLiteral("失败")));
    dialog.findChild<QComboBox *>("reportFrame")->setCurrentIndex(1);
    dialog.findChild<QDoubleSpinBox *>("reportStep")->setValue(0.000001);
    generate->click();
    EXPECT_FALSE(pending);
    EXPECT_TRUE(generate->isEnabled());
    EXPECT_EQ(table->model()->rowCount(), 4);
    library->setCurrentIndex(3);
    generate->click();
    ASSERT_TRUE(pending);
    pending(runtime::ReportContext{project, time, {}, {}}, {});
    pending = {};
    wait();
    EXPECT_EQ(table->model()->rowCount(), 1);
    capture("ground-stations");
    library->setCurrentIndex(static_cast<int>(application::report_kinds.size()));
    EXPECT_TRUE(library->currentText().contains(QStringLiteral("沿海站位置")));
    dialog.findChild<QAction *>("deleteReport")->trigger();
    EXPECT_TRUE(project.reports.empty());
    dialog.select_object("station", application::ReportKind::ground_stations);
    save_template(dialog, QStringLiteral("站点模板副本"));
    ASSERT_EQ(project.reports.size(), 1U);
    EXPECT_EQ(project.reports[0].object_ids, std::vector<std::string>{"station"});
    const auto original_id = project.reports[0].id;
    save_template(dialog, QStringLiteral("站点模板副本"));
    ASSERT_EQ(project.reports.size(), 2U);
    EXPECT_NE(original_id, project.reports[1].id);
    workspace.resize(370, 600);
    workspace.add_view(new QWidget, "3D", {});
    workspace.show();
    workspace.open(&dialog, false);
    for (int i = 0; i < 3; ++i)
        QCoreApplication::processEvents();
    EXPECT_TRUE(
        workspace.rect().contains(generate->mapTo(&workspace, generate->rect().bottomRight())));
    EXPECT_TRUE(generate->visibleRegion().contains(generate->rect()));
}
TEST(ReportDialog, ObjectScopeSurvivesTemplateChangesSavedConfigurationsAndProjectChanges) {
    (void)test_app();
    std::shared_ptr<const core::ITimeProvider> time = runtime::make_builtin_time_provider();
    application::Project project("对象报告", core::Epoch{0}, core::Epoch{120'000'000'000});
    project.ground_stations.push_back({"east", "东站", {1.2, 0.6, 350}});
    project.ground_stations.push_back({"west", "西站", {1.0, 0.5, 250}});
    application::ProjectSatellite satellite;
    satellite.orbit.first = project.first;
    satellite.orbit.last = project.last;
    project.satellites.push_back(satellite);
    ReportDialog dialog(
        project, time,
        [&](ReportDialog::Ready ready) {
            ready(runtime::ReportContext{project, time, {}, {}}, {});
        },
        [&](auto reports) { project.reports = std::move(reports); }, [](auto) {});
    dialog.show();
    dialog.select_object("east", application::ReportKind::ground_stations);
    auto *library = dialog.findChild<QComboBox *>("reportLibrary");
    auto *generate = dialog.findChild<QPushButton *>("generateReport");
    auto *table = dialog.findChild<QTableView *>("reportTable");
    auto *scope = dialog.findChild<QLabel *>("reportScopeLabel");
    const auto run = [&] {
        generate->click();
        QElapsedTimer timer;
        timer.start();
        while (!generate->isEnabled() && timer.elapsed() < 5000) {
            QCoreApplication::processEvents();
            QThread::msleep(2);
        }
        EXPECT_TRUE(generate->isEnabled());
    };
    EXPECT_EQ(scope->text(), QStringLiteral("东站"));
    EXPECT_FALSE(qobject_cast<QStandardItemModel *>(library->model())->item(0)->isEnabled());
    library->setCurrentIndex(1);
    run();
    ASSERT_EQ(table->model()->rowCount(), 1);
    EXPECT_EQ(table->model()->data(table->model()->index(0, 0)).toString(), "east");
    library->setCurrentIndex(2);
    dialog.findChild<QComboBox *>("reportFrame")->setCurrentIndex(1);
    run();
    ASSERT_EQ(table->model()->rowCount(), 3);
    for (int row = 0; row < 3; ++row)
        EXPECT_EQ(table->model()->data(table->model()->index(row, 0)).toString(), "east");
    const auto capture_directory = qEnvironmentVariable("ORBITFORGE_REPORT_UI_CAPTURE");
    if (!capture_directory.isEmpty()) {
        QCoreApplication::processEvents();
        EXPECT_TRUE(dialog.grab().save(capture_directory + "/object-scope-results.png"));
    }
    save_template(dialog, QStringLiteral("对象模板"));
    ASSERT_EQ(project.reports.size(), 1U);
    EXPECT_EQ(project.reports[0].object_ids, std::vector<std::string>{"east"});
    project = application::decode_project(application::encode_project(project));
    project.ground_stations.push_back({"north", "北站", {1.3, 0.8, 300}});
    dialog.update_project(project, time);
    library->setCurrentIndex(3);
    run();
    ASSERT_EQ(table->model()->rowCount(), 1);
    EXPECT_EQ(table->model()->data(table->model()->index(0, 0)).toString(), "east");
    // A new context replaces the old object, even while reusing an existing report page.
    dialog.select_object("west", application::ReportKind::ground_stations);
    library->setCurrentIndex(1);
    run();
    ASSERT_EQ(table->model()->rowCount(), 1);
    EXPECT_EQ(table->model()->data(table->model()->index(0, 0)).toString(), "west");
    // Loading a saved configuration explicitly restores its own scope.
    library->setCurrentIndex(static_cast<int>(application::report_kinds.size()));
    run();
    EXPECT_EQ(scope->text(), QStringLiteral("东站"));
    ASSERT_EQ(table->model()->rowCount(), 3);
    // A removed binding must fail, never widen the scope to all objects.
    project.ground_stations.erase(project.ground_stations.begin());
    dialog.update_project(project, time);
    run();
    EXPECT_TRUE(scope->text().contains(QStringLiteral("已删除")));
    EXPECT_EQ(table->model()->rowCount(), 3);
    EXPECT_TRUE(dialog.findChild<QLabel *>("reportStatus")->text().contains("missing"));
    // Changing scope requires an explicit choice; cancelling preserves the binding.
    QTimer::singleShot(0, [] {
        auto *chooser = qobject_cast<QDialog *>(QApplication::activeModalWidget());
        ASSERT_TRUE(chooser);
        EXPECT_FALSE(chooser->findChild<QCheckBox *>("reportScopeAll")->isChecked());
        chooser->reject();
    });
    dialog.findChild<QPushButton *>("reportChangeScope")->click();
    EXPECT_TRUE(scope->text().contains(QStringLiteral("已删除")));
    QTimer::singleShot(0, [] {
        auto *chooser = qobject_cast<QDialog *>(QApplication::activeModalWidget());
        ASSERT_TRUE(chooser);
        chooser->findChild<QCheckBox *>("reportScopeAll")->setChecked(true);
        chooser->findChild<QDialogButtonBox *>()->button(QDialogButtonBox::Ok)->click();
    });
    dialog.findChild<QPushButton *>("reportChangeScope")->click();
    library->setCurrentIndex(1);
    run();
    EXPECT_EQ(table->model()->rowCount(), 3);
    EXPECT_EQ(scope->text(), QStringLiteral("整个场景"));
    // A satellite context does not offer a ground-station-only template.
    dialog.select_object(satellite.orbit.object_id, application::ReportKind::cartesian_state);
    EXPECT_FALSE(qobject_cast<QStandardItemModel *>(library->model())->item(3)->isEnabled());
}
TEST(ReportDialog, CurvesReuseRowsSupportObjectComparisonZoomSeekAndPngExport) {
    (void)test_app();
    if (!runtime::two_body_available())
        GTEST_SKIP();
    std::shared_ptr<const core::ITimeProvider> time = runtime::make_builtin_time_provider();
    application::Project project("轨道对比", core::Epoch{0}, core::Epoch{24'000'000'000'000});
    for (int i = 0; i < 2; ++i) {
        application::ProjectSatellite satellite;
        satellite.orbit.object_id = "satellite-" + std::to_string(i);
        satellite.display_name = i == 0 ? "晨星" : "远航";
        satellite.orbit.first = project.first;
        satellite.orbit.last = project.last;
        satellite.orbit.initial =
            runtime::to_cartesian({project.first, 8e6 + i * 8e5, .12, .7, .4, .2, .5 * i},
                                  core::GravitationalParameter{satellite.orbit.mu()});
        project.satellites.push_back(satellite);
    }
    int preparations = 0;
    std::optional<core::Epoch> sought;
    ReportDialog dialog(
        project, time,
        [&](ReportDialog::Ready ready) {
            ++preparations;
            ready(runtime::ReportContext{project, time, {}, {}}, {});
        },
        [&](auto reports) { project.reports = std::move(reports); },
        [&](core::Epoch epoch) { sought = epoch; });
    dialog.show();
    auto *library = dialog.findChild<QComboBox *>("reportLibrary");
    auto *generate = dialog.findChild<QPushButton *>("generateReport");
    auto *table = dialog.findChild<QTableView *>("reportTable");
    auto *views = dialog.findChild<QTabBar *>("reportViews");
    auto *fields = dialog.findChild<QComboBox *>("reportPlotField");
    auto *objects = dialog.findChild<QComboBox *>("reportPlotObject");
    auto *plot = static_cast<ReportPlot *>(dialog.findChild<QWidget *>("reportPlot"));
    auto *export_button = dialog.findChild<QPushButton *>("exportReportCsv");
    ASSERT_EQ(library->count(), static_cast<int>(application::report_kinds.size()));
    EXPECT_FALSE(views->isTabEnabled(1));
    const auto run = [&] {
        generate->click();
        QElapsedTimer timer;
        timer.start();
        while (!generate->isEnabled() && timer.elapsed() < 5000) {
            QCoreApplication::processEvents();
            QThread::msleep(2);
        }
        ASSERT_TRUE(generate->isEnabled());
        ASSERT_TRUE(
            dialog.findChild<QLabel *>("reportStatus")->text().contains(QStringLiteral("已生成")));
    };
    library->setCurrentIndex(
        library->findData(static_cast<int>(application::ReportKind::orbital_motion)));
    run();
    ASSERT_EQ(table->model()->rowCount(), 802);
    ASSERT_TRUE(views->isTabEnabled(1));
    views->setCurrentIndex(1);
    QCoreApplication::processEvents();
    ASSERT_TRUE(plot->isVisible());
    EXPECT_EQ(fields->count(), 5);
    EXPECT_EQ(plot->series_count(), 2U);
    EXPECT_TRUE(export_button->text().contains("PNG"));
    const auto initial = plot->visible_interval();
    const QPointF pos{plot->width() * .55, plot->height() * .55};
    QWheelEvent zoom(pos, plot->mapToGlobal(pos.toPoint()), QPoint{}, QPoint{0, 120}, Qt::NoButton,
                     Qt::NoModifier, Qt::NoScrollPhase, false);
    QApplication::sendEvent(plot, &zoom);
    EXPECT_LT(plot->visible_interval().second - plot->visible_interval().first,
              initial.second - initial.first);
    QMouseEvent pick(QEvent::MouseButtonDblClick, pos, plot->mapToGlobal(pos.toPoint()),
                     Qt::LeftButton, Qt::LeftButton, Qt::NoModifier);
    QApplication::sendEvent(plot, &pick);
    ASSERT_TRUE(sought);
    EXPECT_GE(sought->tai_nanoseconds(), project.first.tai_nanoseconds());
    EXPECT_LE(sought->tai_nanoseconds(), project.last.tai_nanoseconds());
    dialog.findChild<QToolButton *>("reportPlotReset")->click();
    EXPECT_EQ(plot->visible_interval(), initial);
    objects->setCurrentIndex(objects->findData("satellite-1"));
    EXPECT_EQ(plot->series_count(), 1U);
    fields->setCurrentIndex(fields->findData("motion.radial_speed", Qt::UserRole + 1));
    EXPECT_EQ(preparations, 1); // A view change must not restart provider sampling.
    EXPECT_EQ(table->model()->rowCount(), 802);
    objects->setCurrentIndex(0);
    fields->setCurrentIndex(0);
    EXPECT_FALSE(dialog.findChild<QLineEdit *>("reportName"));
    EXPECT_FALSE(dialog.findChild<QPushButton *>("saveReport"));
    QTimer::singleShot(0, [] {
        auto *naming = qobject_cast<QInputDialog *>(QApplication::activeModalWidget());
        ASSERT_TRUE(naming);
        naming->reject();
    });
    dialog.findChild<QAction *>("saveReport")->trigger();
    EXPECT_TRUE(project.reports.empty());
    save_template(dialog, QStringLiteral("双星轨道对比"));
    ASSERT_EQ(project.reports.size(), 1U);
    const auto saved_id = project.reports[0].id;
    dialog.findChild<QDoubleSpinBox *>("reportStep")->setValue(120);
    dialog.findChild<QAction *>("updateReport")->trigger();
    ASSERT_EQ(project.reports.size(), 1U);
    EXPECT_EQ(project.reports[0].id, saved_id);
    EXPECT_EQ(project.reports[0].step_nanoseconds, 120'000'000'000);
    EXPECT_EQ(preparations, 1);
    // Return to matching settings for the gallery capture; saving never regenerates the data.
    dialog.findChild<QDoubleSpinBox *>("reportStep")->setValue(60);
    dialog.findChild<QAction *>("updateReport")->trigger();
    run();
    const auto capture = [&](const QString &name) {
        QCoreApplication::processEvents();
        const auto directory = qEnvironmentVariable("ORBITFORGE_REPORT_UI_CAPTURE");
        if (!directory.isEmpty()) {
            ASSERT_TRUE(QDir().mkpath(directory));
            ASSERT_TRUE(dialog.grab().save(directory + "/" + name + ".png"));
            ASSERT_TRUE(plot->image().save(directory + "/" + name + "-export.png"));
        }
    };
    capture("curves");
    QTemporaryDir output;
    ASSERT_TRUE(output.isValid());
    const auto file = output.filePath("comparison.png");
    const bool native_dialogs_disabled = QApplication::testAttribute(Qt::AA_DontUseNativeDialogs);
    QApplication::setAttribute(Qt::AA_DontUseNativeDialogs, true);
    QTimer::singleShot(0, [file] {
        auto *chooser = qobject_cast<QFileDialog *>(QApplication::activeModalWidget());
        ASSERT_TRUE(chooser);
        chooser->selectFile(file);
        QMetaObject::invokeMethod(chooser, "accept", Qt::QueuedConnection);
    });
    export_button->click();
    QApplication::setAttribute(Qt::AA_DontUseNativeDialogs, native_dialogs_disabled);
    const QImage png(file);
    EXPECT_FALSE(png.isNull());
    EXPECT_EQ(png.width(), plot->width() * 2);
    EXPECT_EQ(png.height(), plot->height() * 2);
    dialog.resize(460, 680);
    QCoreApplication::processEvents();
    EXPECT_TRUE(
        dialog.rect().contains(export_button->mapTo(&dialog, export_button->rect().bottomRight())));
    capture("curves-narrow");
    dialog.resize(1120, 740);
    library->setCurrentIndex(
        library->findData(static_cast<int>(application::ReportKind::classical_elements)));
    run();
    EXPECT_EQ(fields->currentData(Qt::UserRole + 1).toString(), "elements.nu");
    capture("elements-curves");
    // A summary remains a table; a stale curve must not masquerade as the new result.
    library->setCurrentIndex(0);
    run();
    EXPECT_FALSE(views->isTabEnabled(1));
    EXPECT_EQ(views->currentIndex(), 0);
    EXPECT_TRUE(table->isVisible());
}
TEST(ReportPlot, AngleWrapDoesNotDrawFalseCrossingAndPixelReductionRetainsPeaks) {
    (void)test_app();
    application::ReportDefinition d;
    d.id = "test";
    d.name = "test";
    d.time_scale = application::ReportTimeScale::tai;
    std::vector<results::Column> columns{{"object.id", "ID", ""},
                                         {"object.name", "Object", ""},
                                         {"time.epoch", "Time", "epoch"},
                                         {"geodetic.longitude", "Longitude", "rad"}};
    auto data = std::make_shared<runtime::ReportResult>(runtime::ReportResult{
        d, results::Table{
               columns,
               {{std::string("a"), std::string("A"), core::Epoch{0}, 3.12},
                {std::string("a"), std::string("A"), core::Epoch{10'000'000'000}, -3.12}}}});
    data->last = core::Epoch{10'000'000'000};
    ReportPlot plot;
    plot.resize(600, 400);
    plot.set_result(data, 3, "Longitude (deg)");
    const auto curve_pixels = [](const QImage &img) {
        int count = 0;
        for (int y = 150; y < 250; ++y)
            for (int x = 275; x < 365; ++x) {
                const auto color = img.pixelColor(x, y);
                if (color.red() > 145 && color.green() > 140 && color.blue() > 225)
                    ++count;
            }
        return count;
    };
    EXPECT_EQ(curve_pixels(plot.image(1)), 0); // ±180° must not be joined through zero.
    columns[3] = {"state.x", "X", "m"};
    std::vector<results::Row> rows;
    for (std::int64_t i = 0; i < 20000; ++i)
        rows.push_back({std::string("a"), std::string("A"), core::Epoch{i * 1'000'000'000},
                        i == 10000 ? 1000.0 : 0.0});
    data = std::make_shared<runtime::ReportResult>(
        runtime::ReportResult{d, results::Table{columns, std::move(rows)}});
    data->last = core::Epoch{19999'000'000'000};
    plot.set_result(data, 3, "X (km)");
    EXPECT_GT(curve_pixels(plot.image(1)), 10); // A one-sample peak survives pixel reduction.
    EXPECT_EQ(data->table.rows().size(), 20000U);
}
TEST(ReportDialog, AccessPairSensorScopeCurveAndDeletedBinding) {
    (void)test_app();
    if (!runtime::two_body_available())
        GTEST_SKIP();
    std::shared_ptr<const core::ITimeProvider> time = runtime::make_builtin_time_provider();
    std::shared_ptr<const core::IEarthOrientationProvider> eop =
        runtime::load_eop_snapshot(ORBITFORGE_ACCESS_EOP);
    std::shared_ptr<const core::IFrameProvider> frames = runtime::make_frame_provider();
    application::Project project{"Access UI", core::Epoch{0}, core::Epoch{120'000'000'000}};
    project.earth_orientation = eop->data_info();
    application::ProjectSatellite sat;
    sat.orbit.first = project.first;
    sat.orbit.last = project.last;
    project.satellites.push_back(sat);
    const auto fixed = core::transform_state(sat.orbit.initial, core::FrameId::itrf, *frames,
                                             eop->at(project.first));
    const auto subpoint = runtime::itrf_to_wgs84(fixed);
    project.ground_stations.push_back(
        {"station", "测控站", {subpoint.longitude_rad, subpoint.latitude_rad, 0}});
    project.sensors.push_back({"sensor", "观测仪", "satellite-1"});
    int published = 0;
    ReportDialog dialog(
        project, time,
        [&](ReportDialog::Ready ready) {
            ready(runtime::ReportContext{project, time, frames, eop}, {});
        },
        [&](auto reports) { project.reports = std::move(reports); }, [](auto) {});
    dialog.result_published = [&](auto) { ++published; };
    dialog.show();
    dialog.select_object("sensor", application::ReportKind::access_intervals);
    auto *station = dialog.findChild<QComboBox *>("accessStation");
    auto *target = dialog.findChild<QComboBox *>("accessTarget");
    auto *generate = dialog.findChild<QPushButton *>("generateReport");
    EXPECT_EQ(station->currentData().toString(), "station");
    EXPECT_EQ(target->currentData().toString(), "sensor");
    EXPECT_TRUE(target->isVisible());
    EXPECT_FALSE(dialog.findChild<QPushButton *>("reportChangeScope")->isVisible());
    const auto run = [&] {
        generate->click();
        QElapsedTimer timer;
        timer.start();
        while (!generate->isEnabled() && timer.elapsed() < 5000) {
            QCoreApplication::processEvents();
            QThread::msleep(2);
        }
        EXPECT_TRUE(generate->isEnabled());
    };
    run();
    EXPECT_EQ(published, 1);
    auto *views = dialog.findChild<QTabBar *>("reportViews");
    EXPECT_FALSE(views->isTabEnabled(1));
    dialog.findChild<QComboBox *>("reportLibrary")
        ->setCurrentIndex(static_cast<int>(application::ReportKind::aer));
    EXPECT_EQ(target->currentData().toString(), "sensor");
    run();
    EXPECT_EQ(published, 2);
    EXPECT_TRUE(views->isTabEnabled(1));
    EXPECT_GE(
        dialog.findChild<QComboBox *>("reportPlotField")->findData("aer.azimuth", Qt::UserRole + 1),
        0);
    EXPECT_EQ(
        dialog.findChild<QComboBox *>("reportPlotField")->currentData(Qt::UserRole + 1).toString(),
        "aer.elevation");
    const auto capture = qEnvironmentVariable("ORBITFORGE_REPORT_UI_CAPTURE");
    if (!capture.isEmpty()) {
        QDir().mkpath(capture);
        dialog.resize(1140, 780);
        views->setCurrentIndex(1);
        QCoreApplication::processEvents();
        dialog.grab().save(capture + "/access-aer-wide.png");
        dialog.resize(420, 700);
        QCoreApplication::processEvents();
        dialog.grab().save(capture + "/access-aer-narrow.png");
    }
    project.sensors.clear();
    dialog.update_project(project, time);
    EXPECT_EQ(target->currentData().toString(), "sensor");
    EXPECT_TRUE(target->currentText().contains(QStringLiteral("已删除")));
    run();
    EXPECT_EQ(published, 2); // Never silently substitutes the parent after deletion.
}
} // namespace
} // namespace orbitforge::desktop
