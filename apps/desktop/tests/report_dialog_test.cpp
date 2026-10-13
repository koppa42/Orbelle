#include "../src/viewer/epoch_editor.hpp"
#include "../src/viewer/object_selection.hpp"
#include "../src/viewer/report_dialog.hpp"
#include "../src/viewer/report_plot.hpp"
#include "../src/viewer/report_selection.hpp"
#include "../src/viewer/workbench_style.hpp"
#include "../src/viewer/workspace.hpp"
#include "window_show_observer.hpp"
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
#include <QMimeData>
#include <QMouseEvent>
#include <QPlainTextEdit>
#include <QPushButton>
#include <QScrollArea>
#include <QScrollBar>
#include <QSplitter>
#include <QListWidget>
#include <QTabBar>
#include <QTableView>
#include <QTemporaryDir>
#include <QThread>
#include <QTimer>
#include <QToolButton>
#include <QTreeWidget>
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
void choose_report(ReportDialog &dialog, int row) {
    QTimer::singleShot(0, [row] {
        auto *chooser = QApplication::activeModalWidget();
        ASSERT_TRUE(chooser);
        auto *list = chooser->findChild<QListWidget *>("reportSelectionList");
        ASSERT_TRUE(list);
        ASSERT_LT(row, list->count());
        list->setCurrentRow(row);
        auto *confirm = chooser->findChild<QDialogButtonBox *>()->button(QDialogButtonBox::Ok);
        EXPECT_TRUE(confirm->isEnabled());
        if (confirm->isEnabled())
            confirm->click();
        else
            qobject_cast<QDialog *>(chooser)->reject();
    });
    dialog.findChild<QPushButton *>("reportLibrary")->click();
}
bool report_available(ReportDialog &dialog, int row) {
    bool available = false;
    QTimer::singleShot(0, [&] {
        auto *chooser = qobject_cast<QDialog *>(QApplication::activeModalWidget());
        ASSERT_TRUE(chooser);
        chooser->findChild<QListWidget *>("reportSelectionList")->setCurrentRow(row);
        available =
            chooser->findChild<QDialogButtonBox *>()->button(QDialogButtonBox::Ok)->isEnabled();
        chooser->reject();
    });
    dialog.findChild<QPushButton *>("reportLibrary")->click();
    return available;
}
void wait_for_report(ReportDialog &dialog) {
    const auto *generate = dialog.findChild<QPushButton *>("generateReport");
    QElapsedTimer timer;
    timer.start();
    while (!generate->isEnabled() && timer.elapsed() < 5000) {
        QCoreApplication::processEvents();
        QThread::msleep(2);
    }
    ASSERT_TRUE(generate->isEnabled());
}
TEST(ReportDialog, StalePreparationGenerationAndCancellationPreservePublishedResult) {
    (void)test_app();
    const auto data = runtime::DataSnapshot::builtin_time();
    application::Project project("Fixed inputs", core::Epoch{0}, core::Epoch{120'000'000'000});
    project.ground_stations.push_back({"station", "Station", {0, 0, 0}});
    ReportDialog::Ready pending;
    ReportDialog dialog(
        project, data.time(), [&](auto ready) { pending = std::move(ready); }, [](auto) {},
        [](auto) {});
    int publications = 0;
    dialog.result_published = [&](auto result, const runtime::Analysis &analysis) {
        ++publications;
        EXPECT_EQ(result->input_revision, analysis.snapshot().revision);
        EXPECT_EQ(analysis.data().time(), data.time());
        EXPECT_EQ(analysis.snapshot().project->ground_stations[0].location.height_m, 0);
    };
    dialog.select_object("station", application::ReportKind::cartesian_state);
    dialog.findChild<QComboBox *>("reportFrame")->setCurrentIndex(1);
    auto *generate = dialog.findChild<QPushButton *>("generateReport");
    auto *table = dialog.findChild<QTableView *>("reportTable");
    auto *status = dialog.findChild<QLabel *>("reportStatus");
    const auto release = [&](application::Project input) {
        ASSERT_TRUE(pending);
        auto ready = std::move(pending);
        pending = {};
        ready(ReportInput{std::move(input), data}, {});
    };
    generate->click();
    release(project);
    wait_for_report(dialog);
    ASSERT_EQ(publications, 1);
    ASSERT_EQ(table->model()->rowCount(), 3);
    const auto original = table->model()->data(table->model()->index(0, 3));

    // Reject a delayed preflight callback carrying an older scene.
    generate->click();
    auto changed = project;
    changed.ground_stations[0].location.height_m = 400;
    dialog.update_project(changed, data.time());
    release(project);
    EXPECT_TRUE(generate->isEnabled());
    EXPECT_TRUE(status->text().contains(QStringLiteral("准备期间已变化")));
    EXPECT_EQ(publications, 1);
    dialog.update_project(project, data.time());

    // Even changing inputs and then restoring them cannot revive an old job.
    // No GUI polling occurs between starting the worker and the two changes.
    generate->click();
    release(project);
    dialog.update_project(changed, data.time());
    dialog.update_project(project, data.time());
    wait_for_report(dialog);
    EXPECT_EQ(publications, 1);
    EXPECT_TRUE(status->text().contains(QStringLiteral("未发布过期报告")));
    EXPECT_EQ(table->model()->data(table->model()->index(0, 3)), original);

    generate->click();
    release(project);
    dialog.findChild<QPushButton *>("cancelReport")->click();
    wait_for_report(dialog);
    EXPECT_EQ(publications, 1);
    EXPECT_TRUE(status->text().contains(QStringLiteral("已取消")));
    EXPECT_EQ(table->model()->rowCount(), 3);

    // Playback does not change computation inputs and must not discard a report.
    generate->click();
    release(project);
    project.current = core::Epoch{1};
    dialog.update_project(project, data.time());
    wait_for_report(dialog);
    EXPECT_EQ(publications, 2);

    generate->click();
    release(project);
    project.ground_stations.clear();
    dialog.update_project(project, data.time());
    wait_for_report(dialog);
    EXPECT_EQ(publications, 2);
    EXPECT_TRUE(status->text().contains(QStringLiteral("未发布过期报告")));
    EXPECT_EQ(table->model()->rowCount(), 3);
}
TEST(ReportDialog, AnalysisDoesNotSilentlyAcceptChangedDataAndUsesConfirmedPreflight) {
    (void)test_app();
    auto data = runtime::DataSnapshot::load(ORBITFORGE_ACCESS_EOP);
    application::Project project("Confirmed data", core::Epoch{0}, core::Epoch{120'000'000'000});
    project.earth_orientation = data.eop()->data_info();
    project.ground_stations.push_back({"station", "Station", {0, 0, 0}});
    ReportDialog::Ready pending;
    ReportDialog dialog(
        project, data.time(), [&](auto ready) { pending = std::move(ready); }, [](auto) {},
        [](auto) {});
    int publications = 0;
    dialog.result_published = [&](auto, const runtime::Analysis &analysis) {
        ++publications;
        EXPECT_EQ(analysis.data().eop(), data.eop());
        EXPECT_EQ(analysis.data().data_lock(), project.data_lock());
    };
    dialog.select_object("station", application::ReportKind::cartesian_state);
    auto *generate = dialog.findChild<QPushButton *>("generateReport");
    const auto release = [&] {
        ASSERT_TRUE(pending);
        auto ready = std::move(pending);
        pending = {};
        ready(ReportInput{project, data}, {});
        wait_for_report(dialog);
    };
    generate->click();
    release();
    ASSERT_EQ(publications, 1);
    data = runtime::DataSnapshot::load(ORBITFORGE_ACCESS_EOP, {}, 2);
    generate->click();
    release();
    EXPECT_EQ(publications, 1);
    EXPECT_TRUE(
        dialog.findChild<QLabel *>("reportStatus")->text().contains(QStringLiteral("失败")));
    EXPECT_EQ(dialog.findChild<QTableView *>("reportTable")->model()->rowCount(), 3);

    generate->click();
    // The workbench publishes the approved scene before returning the preflight.
    project.earth_orientation = data.eop()->data_info();
    dialog.update_project(project, data.time());
    release();
    EXPECT_EQ(publications, 2);
}
TEST(ReportDialog, LatePreflightAfterWindowDestructionIsIgnored) {
    (void)test_app();
    const auto data = runtime::DataSnapshot::builtin_time();
    application::Project project("Closed report", core::Epoch{0}, core::Epoch{10});
    ReportDialog::Ready pending;
    {
        ReportDialog dialog(
            project, data.time(), [&](auto ready) { pending = std::move(ready); }, [](auto) {},
            [](auto) {});
        dialog.findChild<QPushButton *>("generateReport")->click();
        ASSERT_TRUE(pending);
    }
    EXPECT_NO_THROW(pending(ReportInput{project, data}, {}));
}
TEST(ReportDialog, PreparingTemplatesAndDockingDoesNotShowTemporaryWindows) {
    (void)test_app();
    WindowShowObserver windows;
    Workspace workspace;
    application::Project project("Scenario", core::Epoch{0}, core::Epoch{120'000'000'000});
    project.ground_stations.push_back({"station", "Station", {1.2, 0.6, 350}});
    ReportDialog dialog(
        project, runtime::make_builtin_time_provider(), [](ReportDialog::Ready) {}, [](auto) {},
        [](core::Epoch) {});
    auto *library = dialog.findChild<QPushButton *>("reportLibrary");
    ASSERT_NE(library, nullptr);
    for (auto kind : application::report_kinds) {
        dialog.select_object("station", kind);
        auto *follow = dialog.findChild<QCheckBox *>("reportFollowScenario");
        ASSERT_NE(follow, nullptr);
        follow->setChecked(false);
        follow->setChecked(true);
    }
    workspace.open(&dialog, false);
    QCoreApplication::processEvents();
    EXPECT_FALSE(dialog.isVisible());
    EXPECT_TRUE(windows.shown.isEmpty()) << windows.shown.join(", ").toStdString();
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
    dialog.resize(1120, 740); // Establish the wide case after native DPI negotiation on show.
    auto *library = dialog.findChild<QPushButton *>("reportLibrary");
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
    choose_report(dialog, 2);
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
    pending(ReportInput{project, runtime::DataSnapshot::from_providers(time)}, {});
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
    const auto result_label = dialog.findChild<QLabel *>("reportResultLabel")->text();
    QTimer::singleShot(0, [] {
        auto *chooser = qobject_cast<QDialog *>(QApplication::activeModalWidget());
        ASSERT_TRUE(chooser);
        chooser->findChild<QListWidget *>("reportSelectionList")->setCurrentRow(3);
        chooser->reject();
    });
    library->click();
    EXPECT_EQ(table->model()->rowCount(), 4);
    EXPECT_EQ(dialog.findChild<QLabel *>("reportResultLabel")->text(), result_label);
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
    EXPECT_TRUE(project.reports[0].query.object_ids.empty());
    const auto saved = application::decode_project(application::encode_project(project));
    EXPECT_EQ(saved.reports[0].query.frame, core::FrameId::itrf);
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
    pending(ReportInput{project, runtime::DataSnapshot::from_providers(time)}, {});
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
    pending(ReportInput{project, runtime::DataSnapshot::from_providers(time)}, {});
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
    choose_report(dialog, 3);
    generate->click();
    ASSERT_TRUE(pending);
    pending(ReportInput{project, runtime::DataSnapshot::from_providers(time)}, {});
    pending = {};
    wait();
    EXPECT_EQ(table->model()->rowCount(), 1);
    capture("ground-stations");
    choose_report(dialog, static_cast<int>(application::report_kinds.size()));
    EXPECT_TRUE(library->text().contains(QStringLiteral("沿海站位置")));
    dialog.findChild<QAction *>("deleteReport")->trigger();
    EXPECT_TRUE(project.reports.empty());
    dialog.select_object("station", application::ReportKind::ground_stations);
    save_template(dialog, QStringLiteral("站点模板副本"));
    ASSERT_EQ(project.reports.size(), 1U);
    EXPECT_EQ(project.reports[0].query.object_ids, std::vector<std::string>{"station"});
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
            ready(ReportInput{project, runtime::DataSnapshot::from_providers(time)}, {});
        },
        [&](auto reports) { project.reports = std::move(reports); }, [](auto) {});
    dialog.show();
    dialog.select_object("east", application::ReportKind::ground_stations);
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
    EXPECT_FALSE(report_available(dialog, 0));
    choose_report(dialog, 1);
    run();
    ASSERT_EQ(table->model()->rowCount(), 1);
    EXPECT_EQ(table->model()->data(table->model()->index(0, 0)).toString(), "east");
    choose_report(dialog, 2);
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
    EXPECT_EQ(project.reports[0].query.object_ids, std::vector<std::string>{"east"});
    project = application::decode_project(application::encode_project(project));
    project.ground_stations.push_back({"north", "北站", {1.3, 0.8, 300}});
    dialog.update_project(project, time);
    choose_report(dialog, 3);
    run();
    ASSERT_EQ(table->model()->rowCount(), 1);
    EXPECT_EQ(table->model()->data(table->model()->index(0, 0)).toString(), "east");
    // A new context replaces the old object, even while reusing an existing report page.
    dialog.select_object("west", application::ReportKind::ground_stations);
    choose_report(dialog, 1);
    run();
    ASSERT_EQ(table->model()->rowCount(), 1);
    EXPECT_EQ(table->model()->data(table->model()->index(0, 0)).toString(), "west");
    // Loading a saved configuration explicitly restores its own scope.
    choose_report(dialog, static_cast<int>(application::report_kinds.size()));
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
        EXPECT_FALSE(chooser->findChild<QCheckBox *>("objectSelectionAll")->isChecked());
        chooser->reject();
    });
    dialog.findChild<QPushButton *>("reportChangeScope")->click();
    EXPECT_TRUE(scope->text().contains(QStringLiteral("已删除")));
    QTimer::singleShot(0, [] {
        auto *chooser = qobject_cast<QDialog *>(QApplication::activeModalWidget());
        ASSERT_TRUE(chooser);
        chooser->findChild<QCheckBox *>("objectSelectionAll")->setChecked(true);
        chooser->findChild<QDialogButtonBox *>()->button(QDialogButtonBox::Ok)->click();
    });
    dialog.findChild<QPushButton *>("reportChangeScope")->click();
    choose_report(dialog, 1);
    run();
    EXPECT_EQ(table->model()->rowCount(), 3);
    EXPECT_EQ(scope->text(), QStringLiteral("整个场景"));
    // A satellite context does not offer a ground-station-only template.
    dialog.select_object(satellite.orbit.object_id, application::ReportKind::cartesian_state);
    EXPECT_FALSE(report_available(dialog, 3));
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
            ready(ReportInput{project, runtime::DataSnapshot::from_providers(time)}, {});
        },
        [&](auto reports) { project.reports = std::move(reports); },
        [&](core::Epoch epoch) { sought = epoch; });
    dialog.show();
    dialog.resize(1120, 740);
    auto *library = dialog.findChild<QPushButton *>("reportLibrary");
    auto *generate = dialog.findChild<QPushButton *>("generateReport");
    auto *table = dialog.findChild<QTableView *>("reportTable");
    auto *views = dialog.findChild<QTabBar *>("reportViews");
    auto *fields = dialog.findChild<QComboBox *>("reportPlotField");
    auto *objects = dialog.findChild<ObjectSelectionField *>("reportPlotObject");
    auto *plot = static_cast<ReportPlot *>(dialog.findChild<QWidget *>("reportPlot"));
    auto *export_button = dialog.findChild<QPushButton *>("exportReportCsv");
    ASSERT_NE(library, nullptr);
    EXPECT_EQ(dialog.findChild<QComboBox *>("reportLibrary"), nullptr);
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
    choose_report(dialog, static_cast<int>(application::ReportKind::orbital_motion));
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
    QTimer::singleShot(0, [] {
        auto *chooser = qobject_cast<ObjectSelectionDialog *>(QApplication::activeModalWidget());
        ASSERT_TRUE(chooser);
        chooser->findChild<QCheckBox *>("objectSelectionAll")->setChecked(false);
        auto *tree = chooser->findChild<QTreeWidget *>("objectSelectionTree");
        ASSERT_EQ(tree->topLevelItemCount(), 2);
        tree->setCurrentItem(tree->topLevelItem(1));
        chooser->findChild<QDialogButtonBox *>()->button(QDialogButtonBox::Ok)->click();
    });
    objects->click();
    EXPECT_EQ(plot->series_count(), 1U);
    fields->setCurrentIndex(fields->findData("motion.radial_speed", Qt::UserRole + 1));
    EXPECT_EQ(preparations, 1); // A view change must not restart provider sampling.
    EXPECT_EQ(table->model()->rowCount(), 802);
    objects->set_selection({{}, true});
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
    EXPECT_EQ(project.reports[0].query.step_nanoseconds, 120'000'000'000);
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
    auto *split = dialog.findChild<QSplitter *>("reportSplit");
    ASSERT_TRUE(split);
    split->setSizes({370, 746});
    split->splitterMoved(split->sizes().front(), 1);
    const auto wide_sizes = split->sizes();
    const auto selected_field = fields->currentData();
    const auto selected_object = objects->id();
    const auto interval_before_resize = plot->visible_interval();
    const int preparations_before_resize = preparations;
    for (int width : {360, 780, 1120, 360, 1120}) {
        dialog.resize(width, 740);
        for (int i = 0; i < 8; ++i)
            QCoreApplication::processEvents();
        EXPECT_EQ(dialog.width(), width);
        EXPECT_EQ(fields->currentData(), selected_field);
        EXPECT_EQ(objects->id(), selected_object);
        EXPECT_EQ(plot->visible_interval(), interval_before_resize);
        EXPECT_EQ(table->model()->rowCount(), 802);
        EXPECT_EQ(preparations, preparations_before_resize);
        EXPECT_TRUE(plot->isVisible());
        EXPECT_TRUE(fields->parentWidget()->rect().contains(
            fields->mapTo(fields->parentWidget(), fields->rect().bottomRight())));
        if (width == 1120)
            EXPECT_NEAR(split->sizes().front(), wide_sizes.front(), 2);
    }
    // Dragging the splitter changes plot width without resizing the dialog.
    dialog.resize(860, 740);
    split->setSizes({380, 476});
    for (int i = 0; i < 8; ++i)
        QCoreApplication::processEvents();
    EXPECT_GT(objects->y(), fields->y());
    split->setSizes({290, 566});
    for (int i = 0; i < 8; ++i)
        QCoreApplication::processEvents();
    EXPECT_EQ(objects->geometry().center().y(), fields->geometry().center().y());
    dialog.resize(1120, 740);
    for (int i = 0; i < 8; ++i)
        QCoreApplication::processEvents();
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
    choose_report(dialog, static_cast<int>(application::ReportKind::classical_elements));
    run();
    EXPECT_EQ(fields->currentData(Qt::UserRole + 1).toString(), "elements.nu");
    capture("elements-curves");
    // A summary remains a table; a stale curve must not masquerade as the new result.
    choose_report(dialog, 0);
    run();
    EXPECT_FALSE(views->isTabEnabled(1));
    EXPECT_EQ(views->currentIndex(), 0);
    EXPECT_TRUE(table->isVisible());
}
TEST(ReportSelection, SearchSourcesStableKeysAndDeletedChoices) {
    (void)test_app();
    std::vector<ReportChoice> choices{
        {"builtin:0", QStringLiteral("场景摘要"), QStringLiteral("摘要"),
         QStringLiteral("场景时段与对象数量"), QStringLiteral("当前对象"), false,
         QStringLiteral("不适用于当前对象范围")},
        {"builtin:2", QStringLiteral("位置与速度"), QStringLiteral("时间序列"),
         QStringLiteral("GCRF 或 ITRF"), QStringLiteral("卫星")}};
    for (int i = 0; i < 200; ++i)
        choices.push_back({QString("template:%1").arg(i),
                           QStringLiteral("同名模板"),
                           QStringLiteral("位置与速度"),
                           QStringLiteral("时间序列表与曲线"),
                           QStringLiteral("卫星 %1").arg(i),
                           true,
                           {}});
    ReportSelectionDialog dialog(choices, "template:100");
    dialog.show();
    auto *list = dialog.findChild<QListWidget *>("reportSelectionList");
    auto *search = dialog.findChild<QLineEdit *>("reportSelectionSearch");
    auto *source = dialog.findChild<QTabBar *>("reportSelectionSource");
    auto *confirm = dialog.findChild<QDialogButtonBox *>()->button(QDialogButtonBox::Ok);
    EXPECT_EQ(dialog.selection(), "template:100");
    list->setCurrentRow(0);
    EXPECT_FALSE(confirm->isEnabled());
    EXPECT_TRUE(dialog.findChild<QLabel *>("reportSelectionWarning")->isVisible());
    search->setText("gcrf");
    EXPECT_EQ(dialog.selection(), QString{});
    EXPECT_FALSE(confirm->isEnabled());
    EXPECT_FALSE(list->item(1)->isHidden());
    EXPECT_TRUE(list->item(0)->isHidden());
    list->setCurrentRow(1);
    source->setCurrentIndex(2);
    EXPECT_FALSE(confirm->isEnabled());
    EXPECT_TRUE(dialog.findChild<QLabel *>("reportSelectionEmpty")->isVisible());
    search->clear();
    EXPECT_TRUE(list->item(1)->isHidden());
    list->setCurrentRow(102);
    EXPECT_TRUE(list->currentItem()
                    ->data(Qt::AccessibleDescriptionRole)
                    .toString()
                    .contains(QStringLiteral("卫星 100")));
    list->setFocus();
    QKeyEvent down(QEvent::KeyPress, Qt::Key_Down, Qt::NoModifier);
    QApplication::sendEvent(list, &down);
    EXPECT_EQ(dialog.selection(), "template:101");
    list->setCurrentRow(102);
    std::reverse(choices.begin(), choices.end());
    dialog.set_choices(choices);
    EXPECT_EQ(dialog.selection(), "template:100");
    EXPECT_TRUE(confirm->isEnabled());
    auto &long_name = *std::ranges::find(choices, QString("template:100"), &ReportChoice::key);
    long_name.name = QStringLiteral("沿海观测任务 / 卫星位置与速度的自定义报告 · ").repeated(8);
    dialog.set_choices(choices);
    dialog.resize(360, 540);
    QCoreApplication::processEvents();
    for (auto *scroll : dialog.findChildren<QScrollArea *>())
        EXPECT_EQ(scroll->horizontalScrollBar()->maximum(), 0);
    const auto directory = qEnvironmentVariable("ORBITFORGE_REPORT_UI_CAPTURE");
    if (!directory.isEmpty()) {
        QDir().mkpath(directory);
        EXPECT_TRUE(dialog.grab().save(directory + "/report-selector-long-name.png"));
    }
    std::erase_if(choices, [](const auto &choice) { return choice.key == "template:100"; });
    dialog.set_choices(choices);
    EXPECT_TRUE(dialog.selection().isEmpty());
    EXPECT_FALSE(confirm->isEnabled());
    dialog.accept();
    EXPECT_TRUE(dialog.isVisible());
    dialog.reject();
}
TEST(ReportDialog, ReportChoiceCancellationSameSelectionAndLiveTemplateUpdates) {
    (void)test_app();
    const std::shared_ptr<const core::ITimeProvider> time = runtime::make_builtin_time_provider();
    application::Project project("报告选择", core::Epoch{0}, core::Epoch{120'000'000'000});
    project.ground_stations.push_back({"east", "东站", {1.2, 0.6, 350}});
    project.ground_stations.push_back({"west", "西站", {1.3, 0.6, 350}});
    for (const auto *id : {"east", "west"}) {
        application::ReportDefinition definition;
        definition.id = id;
        definition.name = "同名模板";
        definition.query.object_ids = {id};
        definition.query.kind = application::ReportKind::cartesian_state;
        definition.query.frame = core::FrameId::itrf;
        project.reports.push_back(definition);
    }
    ReportDialog dialog(project, time, [](ReportDialog::Ready) {}, [](auto) {}, [](auto) {});
    dialog.show();
    dialog.select_object("east", application::ReportKind::cartesian_state);
    auto *step = dialog.findChild<QDoubleSpinBox *>("reportStep");
    auto *button = dialog.findChild<QPushButton *>("reportLibrary");
    step->setValue(17.5);
    QTimer::singleShot(0, [&] {
        auto *chooser = qobject_cast<QDialog *>(QApplication::activeModalWidget());
        ASSERT_TRUE(chooser);
        chooser->findChild<QListWidget *>("reportSelectionList")->setCurrentRow(3);
        QKeyEvent escape(QEvent::KeyPress, Qt::Key_Escape, Qt::NoModifier);
        QApplication::sendEvent(chooser, &escape);
    });
    button->click();
    EXPECT_EQ(step->value(), 17.5);
    choose_report(dialog, static_cast<int>(application::ReportKind::cartesian_state));
    EXPECT_EQ(step->value(), 17.5);
    QTimer::singleShot(0, [&] {
        auto *chooser = qobject_cast<QDialog *>(QApplication::activeModalWidget());
        ASSERT_TRUE(chooser);
        auto *list = chooser->findChild<QListWidget *>("reportSelectionList");
        list->setCurrentRow(static_cast<int>(application::report_kinds.size()) + 1);
        std::reverse(project.reports.begin(), project.reports.end());
        dialog.update_project(project, time);
        EXPECT_EQ(list->currentItem()->data(Qt::UserRole).toString(), "template:west");
        const auto directory = qEnvironmentVariable("ORBITFORGE_REPORT_UI_CAPTURE");
        if (!directory.isEmpty()) {
            QDir().mkpath(directory);
            const auto capture = [&](const QString &name) {
                QCoreApplication::processEvents();
                EXPECT_TRUE(chooser->grab().save(directory + "/" + name + ".png"));
            };
            list->setCurrentRow(static_cast<int>(application::ReportKind::cartesian_state));
            chooser->resize(840, 580);
            capture("report-selector-builtin");
            list->setCurrentRow(0);
            capture("report-selector-unavailable");
            auto *search = chooser->findChild<QLineEdit *>("reportSelectionSearch");
            search->setText("no matching report");
            capture("report-selector-empty");
            search->clear();
            list->setCurrentRow(static_cast<int>(application::report_kinds.size()));
            chooser->resize(780, 540);
            QCoreApplication::processEvents();
            EXPECT_TRUE(chooser->grab().save(directory + "/report-selector-wide.png"));
            chooser->resize(360, 540);
            QCoreApplication::processEvents();
            QCoreApplication::processEvents();
            EXPECT_TRUE(
                list->viewport()->rect().contains(list->visualItemRect(list->currentItem())));
            EXPECT_EQ(chooser->findChild<QSplitter *>("reportSelectionSplit")->orientation(),
                      Qt::Vertical);
            EXPECT_TRUE(chooser->grab().save(directory + "/report-selector-narrow.png"));
            for (auto *scroll : chooser->findChildren<QScrollArea *>())
                EXPECT_EQ(scroll->horizontalScrollBar()->maximum(), 0);
            chooser->resize(360, 440);
            capture("report-selector-minimum");
            const auto *confirm =
                chooser->findChild<QDialogButtonBox *>()->button(QDialogButtonBox::Ok);
            EXPECT_TRUE(
                chooser->rect().contains(confirm->mapTo(chooser, confirm->rect().bottomRight())));
            chooser->resize(840, 580);
            capture("report-selector-restored");
        }
        chooser->findChild<QDialogButtonBox *>()->button(QDialogButtonBox::Ok)->click();
    });
    button->click();
    EXPECT_EQ(dialog.findChild<QLabel *>("reportScopeLabel")->text(), QStringLiteral("西站"));
    EXPECT_EQ(step->value(), 60);
}
TEST(ReportPlot, AngleWrapDoesNotDrawFalseCrossingAndPixelReductionRetainsPeaks) {
    (void)test_app();
    application::ReportDefinition d;
    d.id = "test";
    d.name = "test";
    d.format.time_scale = application::ReportTimeScale::tai;
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
    project.sensors.push_back(
        {"sensor", "观测仪", "satellite-1", 0.7853981633974483,
         application::SensorPointing::nadir, {}});
    int published = 0;
    ReportDialog dialog(
        project, time,
        [&](ReportDialog::Ready ready) {
            ready(ReportInput{project, runtime::DataSnapshot::from_providers(time, frames, eop)},
                  {});
        },
        [&](auto reports) { project.reports = std::move(reports); }, [](auto) {});
    dialog.result_published = [&](auto, const auto &) { ++published; };
    dialog.show();
    dialog.select_object("sensor", application::ReportKind::access_intervals);
    auto *station = dialog.findChild<ObjectSelectionField *>("accessStation");
    auto *target = dialog.findChild<ObjectSelectionField *>("accessTarget");
    auto *generate = dialog.findChild<QPushButton *>("generateReport");
    EXPECT_EQ(station->id(), "station");
    EXPECT_EQ(target->id(), "sensor");
    EXPECT_TRUE(target->isVisible());
    QTimer::singleShot(0, [] {
        auto *chooser = qobject_cast<ObjectSelectionDialog *>(QApplication::activeModalWidget());
        ASSERT_TRUE(chooser);
        auto *tree = chooser->findChild<QTreeWidget *>("objectSelectionTree");
        ASSERT_EQ(tree->topLevelItemCount(), 1);
        ASSERT_EQ(tree->topLevelItem(0)->childCount(), 1);
        EXPECT_EQ(tree->topLevelItem(0)->child(0)->data(0, Qt::UserRole).toString(), "sensor");
        tree->setCurrentItem(tree->topLevelItem(0));
        chooser->reject();
    });
    target->click();
    EXPECT_EQ(target->id(), "sensor");
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
    choose_report(dialog, static_cast<int>(application::ReportKind::aer));
    EXPECT_EQ(target->id(), "sensor");
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
    EXPECT_EQ(target->id(), "sensor");
    EXPECT_TRUE(target->text().contains(QStringLiteral("已删除")));
    run();
    EXPECT_EQ(published, 2); // Never silently substitutes the parent after deletion.
}
TEST(ReportDialog, TemplateUnitsReuseResultsAndReorderedFieldsKeepPlotsAndSelection) {
    (void)test_app();
    std::shared_ptr<const core::ITimeProvider> time = runtime::make_builtin_time_provider();
    application::Project project("Templates", core::Epoch{0}, core::Epoch{120'000'000'000});
    project.ground_stations.push_back({"station", "Station", {0, 0, 0}});
    application::ReportDefinition definition;
    definition.id = "custom";
    definition.name = "Custom";
    definition.query.object_ids = {"station"};
    definition.query.frame = core::FrameId::itrf;
    definition.format.time_scale = application::ReportTimeScale::tai;
    definition.format.columns = {{"state.x", "", "Distance"},
                                 {"time.epoch", "", ""},
                                 {"object.name", "", ""},
                                 {"object.id", "", ""}};
    project.reports.push_back(definition);
    int preparations = 0;
    std::shared_ptr<const runtime::ReportResult> published;
    ReportDialog dialog(
        project, time,
        [&](ReportDialog::Ready ready) {
            ++preparations;
            ready(ReportInput{project, runtime::DataSnapshot::from_providers(time)}, {});
        },
        [](auto) {}, [](auto) {});
    dialog.result_published = [&](auto result, const runtime::Analysis &analysis) {
        EXPECT_EQ(result->input_revision, analysis.snapshot().revision);
        EXPECT_EQ(analysis.data().time(), time);
        published = std::move(result);
    };
    choose_report(dialog, 9);
    auto *generate = dialog.findChild<QPushButton *>("generateReport");
    auto *table = dialog.findChild<QTableView *>("reportTable");
    generate->click();
    QElapsedTimer timer;
    timer.start();
    while (!generate->isEnabled() && timer.elapsed() < 10000) {
        QCoreApplication::processEvents();
        QThread::msleep(2);
    }
    ASSERT_TRUE(published);
    EXPECT_EQ(preparations, 1);
    EXPECT_FALSE(table->isColumnHidden(0));
    EXPECT_FALSE(table->isColumnHidden(3));
    EXPECT_EQ(table->model()->columnCount(), 4);
    const auto original = published;
    dialog.findChild<QComboBox *>("reportUnits")->setCurrentIndex(1);
    EXPECT_EQ(preparations, 1);
    EXPECT_DOUBLE_EQ(table->model()->data(table->model()->index(0, 0)).toString().toDouble(),
                     6378137);
    EXPECT_EQ(published->input_sha256, original->input_sha256);
    EXPECT_EQ(published->source_table->rows(), original->source_table->rows());
    EXPECT_TRUE(table->model()->headerData(0, Qt::Horizontal).toString().contains("(m)"));
    EXPECT_EQ(published, original); // Formatting does not republish the scenario's access result.
    EXPECT_EQ(original->column_unit(0), "km");
    auto *plot = static_cast<ReportPlot *>(dialog.findChild<QWidget *>("reportPlot"));
    EXPECT_FALSE(plot->image(1).isNull());
    project.ground_stations[0].location.height_m = 100;
    dialog.update_project(project, time);
    dialog.findChild<QComboBox *>("reportUnits")->setCurrentIndex(0);
    EXPECT_TRUE(table->model()->headerData(0, Qt::Horizontal).toString().contains("(m)"));
    EXPECT_EQ(preparations, 1);
}
} // namespace
} // namespace orbitforge::desktop
