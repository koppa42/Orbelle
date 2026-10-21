#include "../src/viewer/coverage_map.hpp"
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
#include <QKeyEvent>
#include <QLabel>
#include <QLineEdit>
#include <QListWidget>
#include <QMimeData>
#include <QPlainTextEdit>
#include <QPushButton>
#include <QScrollArea>
#include <QScrollBar>
#include <QSplitter>
#include <QTableView>
#include <QThread>
#include <QTimer>
#include <algorithm>
#include <cmath>
#include <gtest/gtest.h>
#include <numbers>
#include <orbitforge/runtime/ephemeris.hpp>
#include <orbitforge/runtime/time_frames.hpp>
#include <orbitforge/runtime/two_body.hpp>
#include <stdexcept>

#include "support/report_test_support.hpp"

namespace orbitforge::desktop {
namespace {
using namespace report_test;
TEST(ReportDialog, StalePreparationGenerationAndCancellationPreservePublishedResult) {
    (void)test_app();
    const auto data = runtime::DataSnapshot::builtin_time();
    application::Project project("Fixed inputs", core::Epoch{0}, core::Epoch{120'000'000'000});
    project.ground_stations.push_back({"station", "Station", {0, 0, 0}});
    ReportDialog::Ready pending;
    ReportDialog dialog(
        project, data.time(),
        [&](const application::ReportQuery &, auto ready) { pending = std::move(ready); },
        [](auto) {}, [](auto) {});
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
    EXPECT_TRUE(status->text().contains(QStringLiteral("准备期间已改变")));
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
    EXPECT_TRUE(status->text().contains(QStringLiteral("本次生成的结果不再适用")));
    EXPECT_EQ(table->model()->data(table->model()->index(0, 3)), original);

    generate->click();
    release(project);
    dialog.findChild<QPushButton *>("cancelReport")->click();
    wait_for_report(dialog);
    EXPECT_EQ(publications, 1);
    EXPECT_TRUE(status->text().contains(QStringLiteral("已取消")));
    EXPECT_EQ(table->model()->rowCount(), 3);

    // Playback and a rename during generation must retain the numerical job.
    generate->click();
    release(project);
    project.current = core::Epoch{1};
    project.ground_stations[0].display_name = "Renamed while generating";
    dialog.update_project(project, data.time());
    wait_for_report(dialog);
    EXPECT_EQ(publications, 2);
    EXPECT_EQ(table->model()->index(0, 0).data().toString(), "Renamed while generating");

    generate->click();
    release(project);
    project.ground_stations.clear();
    dialog.update_project(project, data.time());
    wait_for_report(dialog);
    EXPECT_EQ(publications, 2);
    EXPECT_TRUE(status->text().contains(QStringLiteral("本次生成的结果不再适用")));
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
        project, data.time(),
        [&](const application::ReportQuery &, auto ready) { pending = std::move(ready); },
        [](auto) {}, [](auto) {});
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
            project, data.time(),
            [&](const application::ReportQuery &, auto ready) { pending = std::move(ready); },
            [](auto) {}, [](auto) {});
        dialog.findChild<QPushButton *>("generateReport")->click();
        ASSERT_TRUE(pending);
    }
    EXPECT_NO_THROW(pending(ReportInput{project, data}, {}));
}

TEST(ReportDialog, RepeatedQueriesReuseAnalysisAndChangedInputsOrDataReplaceIt) {
    (void)test_app();
    auto data = runtime::DataSnapshot::load(ORBITFORGE_ACCESS_EOP);
    application::Project project{"HPOP reports", core::Epoch{0}, core::Epoch{120'000'000'000}};
    project.earth_orientation = data.eop()->data_info();
    application::ProjectSatellite satellite;
    satellite.orbit.propagator = application::OrbitPropagator::hpop;
    satellite.orbit.initial.epoch = project.first;
    satellite.orbit.first = project.first;
    satellite.orbit.last = project.last;
    project.satellites.push_back(satellite);
    int preflights = 0, publications = 0;
    ReportDialog dialog(
        project, data.time(),
        [&](const application::ReportQuery &, auto ready) {
            ++preflights;
            ready(ReportInput{project, data}, {});
        },
        [](auto) {}, [](auto) {});
    std::shared_ptr<const application::Project> captured;
    std::shared_ptr<const runtime::ReportResult> result;
    dialog.result_published = [&](auto report, const runtime::Analysis &analysis) {
        ++publications;
        captured = analysis.snapshot().project;
        result = std::move(report);
        EXPECT_EQ(analysis.data().data_lock(), data.data_lock());
    };
    dialog.select_object(satellite.orbit.object_id, application::ReportKind::cartesian_state);
    auto *generate = dialog.findChild<QPushButton *>("generateReport");
    generate->click();
    wait_for_report(dialog);
    ASSERT_EQ(publications, 1);
    ASSERT_EQ(result->table.rows().size(), 3);
    const auto first_analysis = captured;
    const auto original_end = result->table.rows().back();

    project.current = core::Epoch{1};
    project.satellites[0].display.line_color = 0x123456;
    project.reports = {result->definition};
    project.reports[0].id = "saved-report";
    dialog.update_project(project, data.time());
    dialog.findChild<QDoubleSpinBox *>("reportStep")->setValue(30);
    generate->click();
    wait_for_report(dialog);
    ASSERT_EQ(publications, 2) << dialog.findChild<QLabel *>("reportStatus")->text().toStdString();
    EXPECT_EQ(captured, first_analysis);
    EXPECT_EQ(result->table.rows().size(), 5);
    EXPECT_EQ(result->table.rows().back(), original_end);

    project.satellites[0].display_name = "Renamed satellite";
    dialog.update_project(project, data.time());
    EXPECT_EQ(publications, 2);
    EXPECT_EQ(preflights, 2);
    EXPECT_FALSE(dialog.findChild<QLabel *>("reportResultLabel")
                     ->text()
                     .contains(QStringLiteral("场景已变化")));
    auto *renamed_table = dialog.findChild<QTableView *>("reportTable");
    ASSERT_NE(renamed_table, nullptr);
    EXPECT_EQ(renamed_table->model()->index(0, 0).data().toString(), "Renamed satellite");
    EXPECT_EQ(result->table.rows().back(), original_end); // Old snapshots remain immutable.
    generate->click();
    wait_for_report(dialog);
    ASSERT_EQ(publications, 3);
    EXPECT_NE(captured, first_analysis);
    EXPECT_EQ(captured->satellites[0].name(), "Renamed satellite");
    const auto renamed = captured;

    project.satellites[0].orbit.initial.velocity_mps[1] += 1;
    dialog.update_project(project, data.time());
    generate->click();
    wait_for_report(dialog);
    ASSERT_EQ(publications, 4);
    EXPECT_NE(captured, renamed);
    EXPECT_EQ(captured->satellites[0].orbit.initial.velocity_mps[1],
              project.satellites[0].orbit.initial.velocity_mps[1]);
    const auto changed = captured;

    data = runtime::DataSnapshot::load(ORBITFORGE_ACCESS_EOP, {}, 2);
    project.earth_orientation = data.eop()->data_info();
    dialog.update_project(project, data.time());
    generate->click();
    wait_for_report(dialog);
    ASSERT_EQ(publications, 5);
    EXPECT_NE(captured, changed);
    EXPECT_EQ(preflights, 5); // Reuse must not bypass current resource selection.
}

TEST(ReportDialog, PreparingTemplatesAndDockingDoesNotShowTemporaryWindows) {
    (void)test_app();
    WindowShowObserver windows;
    Workspace workspace;
    application::Project project("Scenario", core::Epoch{0}, core::Epoch{120'000'000'000});
    project.ground_stations.push_back({"station", "Station", {1.2, 0.6, 350}});
    ReportDialog dialog(
        project, runtime::make_builtin_time_provider(),
        [](const application::ReportQuery &, ReportDialog::Ready) {}, [](auto) {},
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

TEST(ReportDialog, FirstResultFillsInitiallyNarrowWorkspace) {
    (void)test_app();
    std::shared_ptr<const core::ITimeProvider> time = runtime::make_builtin_time_provider();
    application::Project project("窄分屏报告", core::Epoch{0}, core::Epoch{120'000'000'000});
    Workspace workspace;
    workspace.resize(460, 640);
    workspace.show();
    auto *dialog = new ReportDialog(
        project, time,
        [&](const application::ReportQuery &, ReportDialog::Ready ready) {
            ready(ReportInput{project, runtime::DataSnapshot::from_providers(time)}, {});
        },
        [](auto) {}, [](auto) {}, &workspace);
    workspace.open(dialog, false);
    for (int i = 0; i < 8; ++i)
        QCoreApplication::processEvents();
    ASSERT_LT(dialog->width(), 800);
    auto *settings = dialog->findChild<QWidget *>("reportSettingsPanel");
    auto *result = dialog->findChild<QWidget *>("reportResultPanel");
    auto *table = dialog->findChild<QTableView *>("reportTable");
    ASSERT_TRUE(settings->isVisible());
    dialog->findChild<QPushButton *>("generateReport")->click();
    wait_for_report(*dialog);
    for (int repeat = 0; repeat < 3; ++repeat) {
        for (int i = 0; i < 8; ++i)
            QCoreApplication::processEvents();
        ASSERT_GT(table->model()->rowCount(), 0);
        EXPECT_TRUE(table->isVisible());
        EXPECT_FALSE(settings->isVisible());
        EXPECT_GT(result->width(), dialog->width() - 20);
        EXPECT_GT(table->width(), dialog->width() - 60);
        EXPECT_GT(table->height(), 250);
        auto *toggle = dialog->findChild<QPushButton *>("reportToggleSettings");
        toggle->click();
        QCoreApplication::processEvents();
        EXPECT_GT(settings->width(), dialog->width() - 20);
        toggle->click();
    }
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
        project, time,
        [&](const application::ReportQuery &, ReportDialog::Ready ready) {
            pending = std::move(ready);
        },
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
    const auto epoch_column = table_column(table, "time.epoch");
    EXPECT_TRUE(
        table->model()->headerData(epoch_column, Qt::Horizontal).toString().contains("UTC"));
    table->doubleClicked(table->model()->index(1, epoch_column));
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
    EXPECT_FALSE(dialog.isWindow());
    for (int i = 0; i < 3; ++i)
        QCoreApplication::processEvents();
    const auto directory = qEnvironmentVariable("ORBITFORGE_REPORT_UI_CAPTURE");
    if (!directory.isEmpty())
        EXPECT_TRUE(workspace.grab().save(directory + "/docked-settings.png"));
    EXPECT_TRUE(
        workspace.rect().contains(generate->mapTo(&workspace, generate->rect().bottomRight())))
        << "workspace=" << workspace.width() << "x" << workspace.height()
        << ", report=" << dialog.width() << "x" << dialog.height()
        << ", generate=" << generate->mapTo(&workspace, generate->rect().bottomRight()).x() << ","
        << generate->mapTo(&workspace, generate->rect().bottomRight()).y();
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
        [&](const application::ReportQuery &, ReportDialog::Ready ready) {
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
    EXPECT_EQ(table->model()
                  ->data(table->model()->index(0, table_column(table, "object.name")))
                  .toString(),
              QStringLiteral("东站"));
    choose_report(dialog, 2);
    dialog.findChild<QComboBox *>("reportFrame")->setCurrentIndex(1);
    run();
    ASSERT_EQ(table->model()->rowCount(), 3);
    for (int row = 0; row < 3; ++row)
        EXPECT_EQ(table->model()
                      ->data(table->model()->index(row, table_column(table, "object.name")))
                      .toString(),
                  QStringLiteral("东站"));
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
    EXPECT_EQ(table->model()
                  ->data(table->model()->index(0, table_column(table, "object.name")))
                  .toString(),
              QStringLiteral("东站"));
    // A new context replaces the old object, even while reusing an existing report page.
    dialog.select_object("west", application::ReportKind::ground_stations);
    choose_report(dialog, 1);
    run();
    ASSERT_EQ(table->model()->rowCount(), 1);
    EXPECT_EQ(table->model()
                  ->data(table->model()->index(0, table_column(table, "object.name")))
                  .toString(),
              QStringLiteral("西站"));
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
    ReportDialog dialog(
        project, time, [](const application::ReportQuery &, ReportDialog::Ready) {}, [](auto) {},
        [](auto) {});
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
} // namespace
} // namespace orbitforge::desktop
