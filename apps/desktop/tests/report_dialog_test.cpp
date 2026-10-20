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
#include <QFileDialog>
#include <QHeaderView>
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
#include <QSortFilterProxyModel>
#include <QSplitter>
#include <QTabBar>
#include <QTableView>
#include <QTemporaryDir>
#include <QThread>
#include <QTimer>
#include <QToolButton>
#include <QTreeWidget>
#include <QWheelEvent>
#include <cmath>
#include <gtest/gtest.h>
#include <numbers>
#include <orbitforge/runtime/ephemeris.hpp>
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
QStringList available_curves(ReportDialog &dialog) {
    QStringList ids;
    QTimer::singleShot(0, [&] {
        auto *chooser = qobject_cast<QDialog *>(QApplication::activeModalWidget());
        ASSERT_TRUE(chooser);
        auto *list = chooser->findChild<QTreeWidget *>("reportCurveList");
        ASSERT_TRUE(list);
        for (int i = 0; i < list->topLevelItemCount(); ++i)
            ids << list->topLevelItem(i)->data(0, Qt::UserRole).toString();
        chooser->reject();
    });
    dialog.findChild<QPushButton *>("reportPlotFields")->click();
    return ids;
}
void select_curves(ReportDialog &dialog, const QStringList &ids, bool accept = true,
                   bool from_table = false) {
    QTimer::singleShot(0, [ids, accept] {
        auto *chooser = qobject_cast<QDialog *>(QApplication::activeModalWidget());
        ASSERT_TRUE(chooser);
        auto *list = chooser->findChild<QTreeWidget *>("reportCurveList");
        ASSERT_TRUE(list);
        for (int i = 0; i < list->topLevelItemCount(); ++i) {
            auto *item = list->topLevelItem(i);
            item->setCheckState(0, ids.contains(item->data(0, Qt::UserRole).toString())
                                       ? Qt::Checked
                                       : Qt::Unchecked);
        }
        if (!accept) {
            chooser->reject();
            return;
        }
        auto *confirm = chooser->findChild<QDialogButtonBox *>()->button(QDialogButtonBox::Ok);
        EXPECT_TRUE(confirm->isEnabled());
        if (confirm->isEnabled())
            confirm->click();
        else
            chooser->reject();
    });
    if (from_table)
        dialog.findChild<QAction *>("reportChooseCurves")->trigger();
    else
        dialog.findChild<QPushButton *>("reportPlotFields")->click();
}
void wait_for_report(ReportDialog &dialog, int timeout = 5000) {
    const auto *generate = dialog.findChild<QPushButton *>("generateReport");
    QElapsedTimer timer;
    timer.start();
    while (!generate->isEnabled() && timer.elapsed() < timeout) {
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
        [&](const application::ReportQuery &, ReportDialog::Ready ready) {
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
    auto *fields = dialog.findChild<QPushButton *>("reportPlotFields");
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
    EXPECT_EQ(available_curves(dialog).size(), 5);
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
    dialog.findChild<QAction *>("reportPlotZoomIn")->trigger();
    const auto comparison_interval = plot->visible_interval();
    EXPECT_LT(comparison_interval.second - comparison_interval.first,
              initial.second - initial.first);
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
    EXPECT_EQ(plot->visible_interval(), comparison_interval);
    select_curves(dialog, {"motion.radial_speed"});
    EXPECT_EQ(plot->visible_interval(), comparison_interval);
    dialog.findChild<QAction *>("reportPlotPan")->trigger();
    EXPECT_EQ(plot->cursor().shape(), Qt::OpenHandCursor);
    const auto move = pos + QPointF(40, 0);
    QMouseEvent press(QEvent::MouseButtonPress, pos, plot->mapToGlobal(pos.toPoint()),
                      Qt::LeftButton, Qt::LeftButton, Qt::NoModifier);
    QMouseEvent drag(QEvent::MouseMove, move, plot->mapToGlobal(move.toPoint()), Qt::NoButton,
                     Qt::LeftButton, Qt::NoModifier);
    QApplication::sendEvent(plot, &press);
    QApplication::sendEvent(plot, &drag);
    EXPECT_LT(plot->visible_interval().first, comparison_interval.first);
    EXPECT_NEAR(plot->visible_interval().second - plot->visible_interval().first,
                comparison_interval.second - comparison_interval.first, 1e-6);
    QKeyEvent escape(QEvent::KeyPress, Qt::Key_Escape, Qt::NoModifier);
    QApplication::sendEvent(plot, &escape);
    EXPECT_EQ(plot->visible_interval(), comparison_interval);
    QMouseEvent release(QEvent::MouseButtonRelease, move, plot->mapToGlobal(move.toPoint()),
                        Qt::LeftButton, Qt::NoButton, Qt::NoModifier);
    QApplication::sendEvent(plot, &release);
    EXPECT_EQ(plot->visible_interval(), comparison_interval);
    dialog.findChild<QAction *>("reportPlotBox")->trigger();
    EXPECT_EQ(plot->cursor().shape(), Qt::CrossCursor);
    dialog.findChild<QAction *>("reportPlotZoomOut")->trigger();
    EXPECT_NEAR(plot->visible_interval().first, initial.first, 1e-6);
    EXPECT_NEAR(plot->visible_interval().second, initial.second, 1e-6);
    EXPECT_EQ(preparations, 1); // A view change must not restart provider sampling.
    EXPECT_EQ(table->model()->rowCount(), 802);
    objects->set_selection({{}, true});
    select_curves(dialog, {"motion.radius"});
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
    const int preparations_before_selection = preparations;
    select_curves(dialog, {"motion.speed", "motion.radial_speed", "motion.transverse_speed"});
    EXPECT_EQ(plot->series_count(), 6U);
    EXPECT_EQ(plot->panel_count(), 1U);
    plot->zoom(.8);
    const auto compared_interval = plot->visible_interval();
    select_curves(dialog, {"motion.radius", "motion.speed", "motion.radial_speed"});
    EXPECT_EQ(plot->series_count(), 6U);
    EXPECT_EQ(plot->panel_count(), 2U);
    EXPECT_EQ(plot->visible_interval(), compared_interval);
    select_curves(dialog, {"motion.transverse_speed"}, false);
    EXPECT_EQ(plot->fields().size(), 3U);
    EXPECT_EQ(plot->visible_interval(), compared_interval);
    EXPECT_EQ(preparations, preparations_before_selection);
    views->setCurrentIndex(0);
    select_curves(dialog, {"motion.radius", "motion.speed", "motion.radial_speed"}, true, true);
    EXPECT_EQ(views->currentIndex(), 1);
    auto *display_units = dialog.findChild<QComboBox *>("reportUnits");
    QTimer::singleShot(0, [&] {
        auto *chooser = qobject_cast<QDialog *>(QApplication::activeModalWidget());
        ASSERT_TRUE(chooser);
        auto *list = chooser->findChild<QTreeWidget *>("reportCurveList");
        for (int i = 0; i < list->topLevelItemCount(); ++i)
            list->topLevelItem(i)->setCheckState(0, i == 0 ? Qt::Checked : Qt::Unchecked);
        display_units->setCurrentIndex(1); // A new presentation arrives while this draft is open.
        chooser->accept();
    });
    fields->click();
    EXPECT_EQ(plot->fields().size(), 3U); // The stale dialog cannot overwrite the new presentation.
    EXPECT_EQ(plot->fields().front().unit, "m");
    EXPECT_EQ(plot->panel_count(), 2U);
    EXPECT_EQ(plot->visible_interval(), compared_interval);
    EXPECT_TRUE(
        dialog.findChild<QLabel *>("reportStatus")->text().contains(QStringLiteral("请重新选择")));
    display_units->setCurrentIndex(0);
    EXPECT_EQ(plot->fields().front().unit, "km");
    EXPECT_EQ(plot->visible_interval(), compared_interval);
    EXPECT_EQ(preparations, preparations_before_selection);
    capture("curves-multiple");
    auto *split = dialog.findChild<QSplitter *>("reportSplit");
    ASSERT_TRUE(split);
    split->setSizes({370, 746});
    split->splitterMoved(split->sizes().front(), 1);
    const auto wide_sizes = split->sizes();
    const auto selected_field = plot->fields().front().id;
    const auto selected_object = objects->id();
    const auto interval_before_resize = plot->visible_interval();
    const int preparations_before_resize = preparations;
    for (int width : {360, 780, 1120, 360, 1120}) {
        dialog.resize(width, 740);
        for (int i = 0; i < 8; ++i)
            QCoreApplication::processEvents();
        EXPECT_EQ(dialog.width(), width);
        EXPECT_EQ(plot->fields().front().id, selected_field);
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
    EXPECT_EQ(plot->fields().front().id, "elements.nu");
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
TEST(ReportSelection, CurveFieldsSearchKeepsCheckedItemsAndValidatesLimit) {
    (void)test_app();
    const std::vector<ReportCurveField> fields{
        {0, "state.x", "X (km)", "km"},
        {1, "state.y", "Y (km)", "km"},
        {2, "state.z", "Z (km)", "km"},
        {3, "state.vx", "Vx (km/s)", "km/s"},
        {4, "state.vy", "Vy (km/s)", "km/s"},
        {5, "state.vz", "Vz (km/s)", "km/s"},
        {6, "motion.speed", QStringLiteral("速率 (km/s)"), "km/s"}};
    ReportCurveSelectionDialog chooser(fields, {"state.x", "state.y"});
    chooser.show();
    auto *list = chooser.findChild<QTreeWidget *>("reportCurveList");
    auto *search = chooser.findChild<QLineEdit *>("reportCurveSearch");
    auto *confirm = chooser.findChild<QDialogButtonBox *>()->button(QDialogButtonBox::Ok);
    search->setText("vx");
    EXPECT_TRUE(list->topLevelItem(0)->isHidden());
    EXPECT_FALSE(list->topLevelItem(3)->isHidden());
    list->topLevelItem(3)->setCheckState(0, Qt::Checked);
    EXPECT_EQ(chooser.selection(), (QStringList{"state.x", "state.y", "state.vx"}));
    search->setText("no-such-field");
    EXPECT_TRUE(chooser.findChild<QLabel *>("reportCurveCount")
                    ->text()
                    .contains(QStringLiteral("没有匹配")));
    EXPECT_TRUE(confirm->isEnabled()); // Searching does not discard hidden checked fields.
    search->clear();
    const auto capture = qEnvironmentVariable("ORBITFORGE_REPORT_UI_CAPTURE");
    for (int width : {560, 360}) {
        chooser.resize(width, 480);
        QCoreApplication::processEvents();
        EXPECT_EQ(list->horizontalScrollBar()->maximum(), 0);
        if (!capture.isEmpty()) {
            QDir().mkpath(capture);
            EXPECT_TRUE(
                chooser.grab().save(capture + QString("/curve-selector-%1.png").arg(width)));
        }
    }
    for (int i = 0; i < list->topLevelItemCount(); ++i)
        list->topLevelItem(i)->setCheckState(0, Qt::Checked);
    EXPECT_FALSE(confirm->isEnabled());
    chooser.accept();
    EXPECT_EQ(chooser.result(), QDialog::Rejected);
    chooser.findChild<QPushButton *>("reportCurveClear")->click();
    EXPECT_TRUE(chooser.selection().isEmpty());
    EXPECT_FALSE(confirm->isEnabled());
    list->topLevelItem(0)->setCheckState(0, Qt::Checked);
    EXPECT_TRUE(confirm->isEnabled());
    chooser.accept();
    EXPECT_EQ(chooser.result(), QDialog::Accepted);
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
TEST(ReportPlot, CachedPaintingMatchesExportAfterViewDataFontAndOverlayChanges) {
    (void)test_app();
    application::ReportDefinition definition;
    definition.id = definition.name = "cache";
    definition.format.time_scale = application::ReportTimeScale::tai;
    std::vector<results::Column> columns{
        {"object.id", "ID", ""}, {"object.name", "Object", ""}, {"time.epoch", "Time", "epoch"},
        {"state.x", "X", "m"},   {"state.y", "Y", "m"},         {"state.vx", "Vx", "m/s"}};
    std::vector<results::Row> rows;
    for (const auto &object : {std::string("a"), std::string("b")})
        for (std::int64_t i = 0; i < 2000; ++i)
            rows.push_back({object, object, core::Epoch{i * 1'000'000'000},
                            i == 1001 ? 10000.0 : 1000 * std::sin(i * .03), 800 * std::cos(i * .03),
                            100 * std::sin(i * .02)});
    rows[513][3] = std::monostate{};
    rows[2513][3] = std::monostate{};
    auto result = std::make_shared<runtime::ReportResult>(
        runtime::ReportResult{definition, results::Table{columns, std::move(rows)}});
    result->last = core::Epoch{1999'000'000'000};
    const std::vector<ReportCurveField> fields{
        {3, {}, "X", {}}, {4, {}, "Y", {}}, {5, {}, "Vx", {}}};
    ReportPlot plot;
    plot.resize(760, 560);
    plot.set_result(result, fields);
    plot.show();
    QCoreApplication::processEvents();
    const auto check = [&] {
        QEvent leave(QEvent::Leave);
        QCoreApplication::sendEvent(&plot, &leave);
        const auto expected =
            plot.image(plot.devicePixelRatioF()).convertToFormat(QImage::Format_RGB32);
        // The screen cache must reproduce the independent export render, including
        // first paint and reuse, without baking interaction overlays into the result.
        for (int i = 0; i < 2; ++i) {
            auto actual = plot.grab().toImage().convertToFormat(QImage::Format_RGB32);
            actual.setDevicePixelRatio(expected.devicePixelRatio());
            EXPECT_EQ(actual, expected);
        }
    };
    check();
    const auto exported = plot.image(1);
    QMouseEvent hover(QEvent::MouseMove, QPointF(300, 180), QPointF(300, 180), Qt::NoButton,
                      Qt::NoButton, Qt::NoModifier);
    QCoreApplication::sendEvent(&plot, &hover);
    (void)plot.grab();
    EXPECT_EQ(plot.image(1), exported);
    check();
    plot.zoom(.25);
    check();
    plot.set_fields({fields[2], fields[0]});
    check();
    plot.set_object("b");
    check();
    auto font = plot.font();
    font.setPixelSize(17);
    plot.setFont(font);
    check();
    plot.resize(430, 710);
    QCoreApplication::processEvents();
    check();
    auto converted = std::make_shared<runtime::ReportResult>(*result);
    converted->definition.format.units = application::ReportUnits::si;
    plot.set_result(converted, fields, {}, true);
    check();
    plot.set_result(nullptr, fields);
    check();
    plot.set_result(result, fields);
    check();
    const auto grouped = plot.image(1);
    std::vector<results::Row> interleaved;
    for (std::size_t i = 0; i < 2000; ++i) {
        interleaved.push_back(result->table.rows()[i]);
        interleaved.push_back(result->table.rows()[i + 2000]);
    }
    auto reordered = std::make_shared<runtime::ReportResult>(*result);
    reordered->table = results::Table{columns, std::move(interleaved)};
    plot.set_result(reordered, fields);
    EXPECT_EQ(plot.image(1), grouped);
    plot.set_object("b");
    const auto single = plot.image(1);
    plot.set_result(result, fields, "b");
    EXPECT_EQ(plot.image(1), single);
}
TEST(ReportPlot, IndexedExtremaMatchVisibleSamplesAcrossPartialBlocks) {
    (void)test_app();
    application::ReportDefinition definition;
    definition.id = definition.name = "extrema";
    definition.format.time_scale = application::ReportTimeScale::tai;
    const std::vector<results::Column> columns{{"object.id", "ID", ""},
                                               {"object.name", "Object", ""},
                                               {"time.epoch", "Time", "epoch"},
                                               {"state.x", "X", "m"}};
    std::vector<results::Row> rows;
    for (std::int64_t i = 0; i < 12000; ++i) {
        const double spike = i == 127 || i == 128 || i == 7001 || i == 9001 ? 10000 : 0;
        rows.push_back({std::string("a"), std::string("A"), core::Epoch{i * 1'000'000'000},
                        100 * std::sin(i * .03) + spike});
    }
    const auto data = [&](std::vector<results::Row> samples) {
        auto result = std::make_shared<runtime::ReportResult>(
            runtime::ReportResult{definition, results::Table{columns, std::move(samples)}});
        result->last = core::Epoch{11999'000'000'000};
        return result;
    };
    ReportPlot full;
    full.resize(650, 420);
    full.set_result(data(rows), 3, "X (km)");
    for (double factor : {.77, .5, .1}) {
        full.reset_view();
        full.zoom(factor);
        const auto [left, right] = full.visible_interval();
        const auto from = static_cast<std::size_t>(std::ceil(left)) - 1;
        const auto to = static_cast<std::size_t>(std::floor(right)) + 2;
        ReportPlot subset;
        subset.resize(full.size());
        subset.set_result(data({rows.begin() + from, rows.begin() + to}), 3, "X (km)");
        subset.zoom(factor);
        // The subset changes only the footer's first-sample timestamp. Axes and
        // curves must match even when block alignment and excluded spikes differ.
        const QRect chart(0, 0, full.width(), full.height() - 45);
        EXPECT_EQ(full.image(1).copy(chart), subset.image(1).copy(chart)) << factor;
    }
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
    project.sensors.push_back({"sensor",
                               "观测仪",
                               "satellite-1",
                               0.7853981633974483,
                               application::SensorPointing::nadir,
                               {}});
    int published = 0;
    std::optional<core::Epoch> sought;
    std::shared_ptr<const runtime::ReportResult> pass_result;
    ReportDialog dialog(
        project, time,
        [&](const application::ReportQuery &, ReportDialog::Ready ready) {
            ready(ReportInput{project, runtime::DataSnapshot::from_providers(time, frames, eop)},
                  {});
        },
        [&](auto reports) { project.reports = std::move(reports); },
        [&](auto epoch) { sought = epoch; });
    dialog.result_published = [&](auto result, const auto &) {
        ++published;
        pass_result = result;
    };
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
    ASSERT_TRUE(pass_result);
    ASSERT_TRUE(pass_result->access_statistics);
    EXPECT_EQ(pass_result->access_statistics->count, 1U);
    EXPECT_TRUE(dialog.findChild<QWidget *>("accessPassSummary")->isVisible());
    EXPECT_EQ(dialog.findChild<QLabel *>("accessMetric0")->text(), "1");
    auto *table = dialog.findChild<QTableView *>("reportTable");
    ASSERT_TRUE(table);
    const auto expected_epoch = std::get<core::Epoch>(pass_result->table.rows()[0][8]);
    EXPECT_TRUE(QMetaObject::invokeMethod(table, "doubleClicked", Qt::DirectConnection,
                                          Q_ARG(QModelIndex, table->model()->index(0, 7))));
    ASSERT_TRUE(sought);
    EXPECT_EQ(*sought, expected_epoch);
    auto *search = dialog.findChild<QComboBox *>("accessSearchMode");
    auto *tolerance = dialog.findChild<QDoubleSpinBox *>("accessTimeTolerance");
    EXPECT_EQ(search->currentIndex(), 1);
    const auto settle_layout = [] {
        for (int i = 0; i < 8; ++i)
            QCoreApplication::processEvents();
    };
    const auto access_capture = qEnvironmentVariable("ORBITFORGE_REPORT_UI_CAPTURE");
    if (!access_capture.isEmpty())
        QDir().mkpath(access_capture);
    dialog.resize(1140, 780);
    settle_layout();
    if (!access_capture.isEmpty())
        dialog.grab().save(access_capture + "/access-passes-wide.png");
    dialog.resize(420, 700);
    settle_layout();
    EXPECT_EQ(search->currentIndex(), 1);
    EXPECT_DOUBLE_EQ(tolerance->value(), .001);
    EXPECT_EQ(table->model()->rowCount(), 1);
    const auto *first_card = dialog.findChild<QLabel *>("accessMetric0")->parentWidget();
    const auto *third_card = dialog.findChild<QLabel *>("accessMetric2")->parentWidget();
    EXPECT_LT(first_card->geometry().top(), third_card->geometry().top());
    if (!access_capture.isEmpty())
        dialog.grab().save(access_capture + "/access-passes-narrow.png");
    dialog.resize(1140, 780);
    settle_layout();
    EXPECT_EQ(first_card->geometry().top(), third_card->geometry().top());
    // Invalid search settings keep the previous, valid result and its summary.
    tolerance->setValue(61);
    generate->click();
    EXPECT_EQ(published, 1);
    EXPECT_EQ(table->model()->rowCount(), 1);
    tolerance->setValue(.001);
    auto *views = dialog.findChild<QTabBar *>("reportViews");
    EXPECT_FALSE(views->isTabEnabled(1));
    choose_report(dialog, static_cast<int>(application::ReportKind::aer));
    EXPECT_EQ(target->id(), "sensor");
    run();
    EXPECT_EQ(published, 2);
    EXPECT_TRUE(views->isTabEnabled(1));
    EXPECT_TRUE(available_curves(dialog).contains("aer.azimuth"));
    EXPECT_EQ(
        static_cast<ReportPlot *>(dialog.findChild<QWidget *>("reportPlot"))->fields().front().id,
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
        [&](const application::ReportQuery &, ReportDialog::Ready ready) {
            ++preparations;
            ready(ReportInput{project, runtime::DataSnapshot::from_providers(time)}, {});
        },
        [](auto) {}, [](auto) {});
    dialog.result_published = [&](auto result, const runtime::Analysis &analysis) {
        EXPECT_EQ(result->input_revision, analysis.snapshot().revision);
        EXPECT_EQ(analysis.data().time(), time);
        published = std::move(result);
    };
    choose_report(dialog, static_cast<int>(application::report_kinds.size()));
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

TEST(ReportDialog, NumericSortingPrecisionCopyAndTimeSeekKeepSourceRowIdentity) {
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
    application::Project project("表格交互", core::Epoch{0}, core::Epoch{120'000'000'001});
    project.ground_stations.push_back(
        {"low", "地面站", {std::acos(2000.123456789 / 6378137.0), 0, 0}});
    project.ground_stations.push_back({"high", "高点", {std::acos(10000.0 / 6378137.0), 0, 0}});
    application::ReportDefinition definition;
    definition.id = "table-view";
    definition.name = "表格交互";
    definition.query.frame = core::FrameId::itrf;
    definition.query.object_ids = {"low", "high"};
    definition.format.columns = {{"state.x", "", "X"},
                                 {"time.epoch", "", ""},
                                 {"object.name", "", ""},
                                 {"object.id", "", ""}};
    project.reports.push_back(definition);
    int preparations = 0;
    std::optional<core::Epoch> sought;
    std::shared_ptr<const runtime::ReportResult> result;
    ReportDialog dialog(
        project, time,
        [&](const application::ReportQuery &, ReportDialog::Ready ready) {
            ++preparations;
            ready(ReportInput{project, runtime::DataSnapshot::from_providers(time)}, {});
        },
        [](auto) {}, [&](core::Epoch epoch) { sought = epoch; });
    dialog.result_published = [&](auto value, const runtime::Analysis &) {
        result = std::move(value);
    };
    choose_report(dialog, static_cast<int>(application::report_kinds.size()));
    dialog.findChild<QPushButton *>("generateReport")->click();
    wait_for_report(dialog);
    ASSERT_TRUE(result) << dialog.findChild<QLabel *>("reportStatus")->text().toStdString();
    auto *table = dialog.findChild<QTableView *>("reportTable");
    auto *proxy = qobject_cast<QSortFilterProxyModel *>(table->model());
    ASSERT_TRUE(proxy);
    ASSERT_EQ(proxy->rowCount(), 8);
    table->sortByColumn(0, Qt::AscendingOrder);
    EXPECT_EQ(proxy->index(0, 3).data().toString(), "low"); // 2 precedes 10 numerically.
    EXPECT_EQ(proxy->index(7, 3).data().toString(), "high");
    table->sortByColumn(0, Qt::DescendingOrder);
    EXPECT_EQ(proxy->index(0, 3).data().toString(), "high");
    table->sortByColumn(1, Qt::DescendingOrder);
    const auto first = proxy->index(0, 1);
    EXPECT_EQ(first.data(Qt::UserRole).toLongLong(), project.last.tai_nanoseconds());
    table->doubleClicked(first);
    ASSERT_TRUE(sought);
    EXPECT_EQ(sought->tai_nanoseconds(), project.last.tai_nanoseconds());
    table->selectRow(0);
    dialog.findChild<QAction *>("reportTableCopy")->trigger();
    const auto source_row = proxy->mapToSource(first).row();
    EXPECT_TRUE(QApplication::clipboard()->text().contains(
        QString::fromStdString(result->cell_text(source_row, 1))));
    dialog.findChild<QAction *>("reportOriginalOrder")->trigger();
    for (int i = 0; i < proxy->rowCount(); ++i)
        EXPECT_EQ(proxy->mapToSource(proxy->index(i, 0)).row(), i);
    auto *precision = dialog.findChild<QComboBox *>("reportPrecision");
    precision->setCurrentIndex(precision->findData(6));
    EXPECT_EQ(proxy->index(0, 0).data(Qt::ToolTipRole).toString(),
              QString::fromStdString(result->cell_text(0, 0)));
    precision->setCurrentIndex(precision->findData(17));
    EXPECT_DOUBLE_EQ(proxy->index(0, 0).data().toString().toDouble(), *result->numeric_value(0, 0));
    EXPECT_EQ(preparations, 1);
    EXPECT_FALSE(dialog.findChild<QAction *>("reportOriginalOrder")->isEnabled());
    table->sortByColumn(0, Qt::DescendingOrder);
    table->selectRow(0);
    EXPECT_TRUE(dialog.findChild<QAction *>("reportTableCopy")->isEnabled());
    dialog.findChild<QPushButton *>("generateReport")->click();
    wait_for_report(dialog);
    EXPECT_EQ(preparations, 2);
    EXPECT_EQ(table->horizontalHeader()->sortIndicatorSection(), -1);
    EXPECT_FALSE(dialog.findChild<QAction *>("reportTableCopy")->isEnabled());
}

TEST(ReportDialog, LightingIntervalsAndCurveShareResultsAndPreserveNarrowLayout) {
    (void)test_app();
    if (!runtime::two_body_available())
        GTEST_SKIP();
    class Sun final : public core::IThirdBodyEphemeris {
      public:
        core::ThirdBodyDataInfo info{
            "UI solar fixture",   std::string(64, 'a'),          "fixed-sun-v1",
            core::Epoch{0},       core::Epoch{20000000000000LL}, 1.3271244004127942e20,
            4.9028001184575496e12};
        std::array<double, 3> position(core::ThirdBody, core::Epoch) const override {
            return {149597870700., 0, 0};
        }
        void validate_interval(core::Epoch, core::Epoch,
                               std::span<const core::ThirdBody>) const override {}
        const core::ThirdBodyDataInfo &data_info() const noexcept override { return info; }
    };
    auto sun = std::make_shared<Sun>();
    const auto data = runtime::DataSnapshot::builtin_time().with_ephemeris("de440s", sun);
    application::Project project("Lighting", core::Epoch{0}, core::Epoch{12000000000000LL});
    application::ProjectSatellite sat;
    sat.display_name = "LEO · 观测一号";
    sat.orbit.first = project.first;
    sat.orbit.last = project.last;
    sat.orbit.initial = {project.first,
                         core::OriginId::earth,
                         core::FrameId::gcrf,
                         {7e6, 0, 0},
                         {0, 7546.053290107542, 0}};
    project.satellites.push_back(sat);
    project.force_data.push_back(runtime::ephemeris_reference("de440s", sun->info));
    int preparations = 0;
    std::shared_ptr<const runtime::ReportResult> published;
    core::Epoch sought{-1};
    ReportDialog dialog(
        project, data.time(),
        [&](const application::ReportQuery &query, auto ready) {
            EXPECT_TRUE(application::report_is_lighting(query.kind));
            EXPECT_EQ(query.lighting_ephemeris, "de440s");
            ++preparations;
            ready(ReportInput{project, data}, {});
        },
        [](auto) {}, [&](auto at) { sought = at; });
    dialog.result_published = [&](auto value, const auto &) { published = value; };
    dialog.select_object(sat.orbit.object_id, application::ReportKind::lighting_intervals);
    dialog.resize(1140, 780);
    dialog.show();
    dialog.findChild<QPushButton *>("generateReport")->click();
    wait_for_report(dialog);
    ASSERT_TRUE(published);
    ASSERT_TRUE(published->lighting_statistics);
    auto *table = dialog.findChild<QTableView *>("reportTable");
    auto *views = dialog.findChild<QTabBar *>("reportViews");
    EXPECT_GT(table->model()->rowCount(), 8);
    EXPECT_FALSE(views->isTabEnabled(1));
    EXPECT_TRUE(dialog.findChild<QLabel *>("accessMetric0")->text().contains("%"));
    EXPECT_TRUE(table->isColumnHidden(6));
    EXPECT_TRUE(table->isColumnHidden(7));
    const auto end = std::get<core::Epoch>(published->table.rows()[1][3]);
    QMetaObject::invokeMethod(table, "doubleClicked", Qt::DirectConnection,
                              Q_ARG(QModelIndex, table->model()->index(1, 3)));
    EXPECT_EQ(sought, end);
    const auto capture = qEnvironmentVariable("ORBITFORGE_REPORT_UI_CAPTURE");
    if (!capture.isEmpty())
        QDir().mkpath(capture);
    const auto settle = [] {
        for (int i = 0; i < 5; ++i) {
            QCoreApplication::processEvents();
            QThread::msleep(10);
        }
    };
    for (const auto width : {1140, 420}) {
        dialog.resize(width, 780);
        settle();
        EXPECT_EQ(table->model()->rowCount(), static_cast<int>(published->table.rows().size()));
        if (!capture.isEmpty())
            dialog.grab().save(capture + QString("/lighting-intervals-%1.png").arg(width));
    }
    dialog.resize(1140, 780);
    settle();
    choose_report(dialog, static_cast<int>(application::ReportKind::lighting_state));
    dialog.findChild<QPushButton *>("generateReport")->click();
    wait_for_report(dialog);
    ASSERT_EQ(preparations, 2);
    ASSERT_FALSE(published->lighting_statistics);
    EXPECT_TRUE(views->isTabEnabled(1));
    views->setCurrentIndex(1);
    auto *plot = static_cast<ReportPlot *>(dialog.findChild<QWidget *>("reportPlot"));
    EXPECT_FALSE(plot->image(1).isNull());
    for (const auto width : {1140, 420}) {
        dialog.resize(width, 780);
        settle();
        if (!capture.isEmpty())
            dialog.grab().save(capture + QString("/lighting-curve-%1.png").arg(width));
    }
    const auto rows = published->table.rows();
    project.satellites.clear();
    dialog.update_project(project, data.time());
    dialog.findChild<QPushButton *>("generateReport")->click();
    wait_for_report(dialog);
    EXPECT_EQ(published->table.rows(), rows);
}
TEST(ReportDialog, CovarianceUsesSavedSettingsAndPublishesSharedResult) {
    (void)test_app();
    const auto data = runtime::DataSnapshot::builtin_time();
    application::Project project("Covariance report", core::Epoch{0}, core::Epoch{60'000'000'000});
    application::ProjectSatellite satellite;
    satellite.orbit.propagator = application::OrbitPropagator::hpop;
    satellite.orbit.first = project.first;
    satellite.orbit.last = project.last;
    satellite.orbit.hpop.gravity_degree = satellite.orbit.hpop.gravity_order = 0;
    satellite.orbit.covariance.enabled = true;
    for (std::size_t i = 0; i < 6; ++i)
        satellite.orbit.covariance.state[i][i] = i < 3 ? 10000 : 1;
    project.satellites.push_back(satellite);
    int preparations = 0;
    ReportDialog dialog(
        project, data.time(),
        [&](const application::ReportQuery &, auto ready) {
            ++preparations;
            ready(ReportInput{project, data}, {});
        },
        [](auto) {}, [](auto) {});
    std::shared_ptr<const runtime::ReportResult> published;
    dialog.result_published = [&](auto result, const runtime::Analysis &analysis) {
        published = result;
        EXPECT_EQ(result->algorithm, "hpop-joint-covariance-direct-coast-v1");
        EXPECT_EQ(result->table.rows().size(), 2);
        EXPECT_EQ(analysis.snapshot().project->satellites.front().orbit.covariance,
                  satellite.orbit.covariance);
        const auto covariance = core::propagate_hpop_covariance(
            satellite.orbit.initial, project.last, satellite.orbit.covariance.active_input(),
            satellite.orbit.hpop, data.force_environment(satellite.orbit));
        EXPECT_EQ(std::get<double>(result->table.rows().back()[4]),
                  covariance.covariance.state[0][0]);
    };
    dialog.select_object(satellite.orbit.object_id, application::ReportKind::state_covariance);
    dialog.show();
    EXPECT_FALSE(dialog.findChild<QComboBox *>("reportFrame")->isVisible());
    dialog.findChild<QPushButton *>("generateReport")->click();
    wait_for_report(dialog);
    ASSERT_TRUE(published);
    EXPECT_EQ(preparations, 1);
    auto *table = dialog.findChild<QTableView *>("reportTable");
    ASSERT_EQ(table->model()->columnCount(), 25);
    ASSERT_EQ(table->model()->rowCount(), 2);
    EXPECT_DOUBLE_EQ(table->model()->data(table->model()->index(0, 4)).toString().toDouble(), .01);
    EXPECT_TRUE(table->model()->headerData(4, Qt::Horizontal).toString().contains("km^2"));
    dialog.findChild<QComboBox *>("reportUnits")->setCurrentIndex(1);
    EXPECT_EQ(preparations, 1);
    EXPECT_DOUBLE_EQ(table->model()->data(table->model()->index(0, 4)).toString().toDouble(),
                     10000);
    EXPECT_TRUE(table->model()->headerData(4, Qt::Horizontal).toString().contains("m^2"));
    auto *plot = static_cast<ReportPlot *>(dialog.findChild<QWidget *>("reportPlot"));
    EXPECT_FALSE(plot->image(1).isNull());
    project.satellites[0].orbit.covariance.enabled = false;
    dialog.update_project(project, data.time());
    EXPECT_FALSE(
        report_available(dialog, static_cast<int>(application::ReportKind::state_covariance)));
}
} // namespace
} // namespace orbitforge::desktop

namespace orbitforge::desktop {
TEST(ReportDialog, OrbitalEventsSummaryTimeSeekAndResponsiveLayout) {
    (void)test_app();
    if (!runtime::two_body_available())
        GTEST_SKIP();
    const auto data = runtime::DataSnapshot::builtin_time();
    application::Project project("Orbital events", core::Epoch{0}, core::Epoch{18000000000000LL});
    application::ProjectSatellite sat;
    sat.display_name = "LEO · 观测一号";
    sat.orbit.first = project.first;
    sat.orbit.last = project.last;
    sat.orbit.initial = {
        project.first, core::OriginId::earth, core::FrameId::gcrf, {7e6, 0, 0}, {0, 8000, 2000}};
    project.satellites.push_back(sat);
    std::shared_ptr<const runtime::ReportResult> published;
    core::Epoch sought{-1};
    ReportDialog dialog(
        project, data.time(),
        [&](const application::ReportQuery &, auto ready) {
            ready(ReportInput{project, data}, {});
        },
        [](auto) {}, [&](auto at) { sought = at; });
    dialog.result_published = [&](auto value, const auto &) { published = value; };
    dialog.select_object(sat.orbit.object_id, application::ReportKind::orbital_events);
    dialog.resize(1140, 780);
    dialog.show();
    dialog.findChild<QPushButton *>("generateReport")->click();
    wait_for_report(dialog);
    ASSERT_TRUE(published);
    ASSERT_TRUE(published->orbital_event_statistics);
    auto *table = dialog.findChild<QTableView *>("reportTable");
    EXPECT_GT(table->model()->rowCount(), 8);
    EXPECT_FALSE(dialog.findChild<QTabBar *>("reportViews")->isTabEnabled(1));
    EXPECT_EQ(dialog.findChild<QLabel *>("accessMetric0")->text().toULongLong(),
              published->orbital_event_statistics->counts[0]);
    EXPECT_TRUE(dialog.findChild<QLabel *>("orbitalEventNotice")->isHidden());
    const auto epoch = std::get<core::Epoch>(published->table.rows()[2][2]);
    QMetaObject::invokeMethod(table, "doubleClicked", Qt::DirectConnection,
                              Q_ARG(QModelIndex, table->model()->index(2, 3)));
    EXPECT_EQ(sought, epoch);
    const auto capture = qEnvironmentVariable("ORBITFORGE_REPORT_UI_CAPTURE");
    if (!capture.isEmpty())
        QDir().mkpath(capture);
    for (const auto width : {1140, 420}) {
        dialog.resize(width, 780);
        for (int i = 0; i < 5; ++i) {
            QCoreApplication::processEvents();
            QThread::msleep(10);
        }
        EXPECT_EQ(table->model()->rowCount(), static_cast<int>(published->table.rows().size()));
        EXPECT_LE(dialog.minimumSizeHint().width(), 420);
        if (!capture.isEmpty())
            dialog.grab().save(capture + QString("/orbital-events-%1.png").arg(width));
    }
    project.satellites[0].orbit.initial.velocity_mps = {0, std::sqrt(3.986004418e14 / 7e6), 0};
    dialog.update_project(project, data.time());
    dialog.findChild<QPushButton *>("generateReport")->click();
    wait_for_report(dialog);
    ASSERT_TRUE(published);
    EXPECT_TRUE(published->table.rows().empty());
    EXPECT_FALSE(dialog.findChild<QLabel *>("orbitalEventNotice")->isHidden());
    EXPECT_TRUE(
        dialog.findChild<QLabel *>("orbitalEventNotice")->text().contains(QStringLiteral("近圆")));
    const auto rows = published->table.rows();
    project.satellites.clear();
    dialog.update_project(project, data.time());
    dialog.findChild<QPushButton *>("generateReport")->click();
    wait_for_report(dialog);
    EXPECT_EQ(published->table.rows(), rows);
}

} // namespace orbitforge::desktop

namespace orbitforge::desktop {
TEST(ReportDialog, IntersatellitePairCurvesExtremaSeekAndResponsiveLayout) {
    (void)test_app();
    if (!runtime::two_body_available())
        GTEST_SKIP();
    std::shared_ptr<const core::ITimeProvider> time = runtime::make_builtin_time_provider();
    std::shared_ptr<const core::IEarthOrientationProvider> eop =
        runtime::load_eop_snapshot(ORBITFORGE_ACCESS_EOP);
    std::shared_ptr<const core::IFrameProvider> frames = runtime::make_frame_provider();
    const auto data = runtime::DataSnapshot::from_providers(time, frames, eop);
    application::Project project{"Intersatellite", core::Epoch{0}, core::Epoch{600'000'000'000}};
    project.earth_orientation = eop->data_info();
    application::ProjectSatellite a, b;
    a.orbit.object_id = "primary";
    b.orbit.object_id = "target";
    a.display_name = "LEO · 基准卫星";
    b.display_name = "LEO · 目标卫星";
    a.orbit.first = b.orbit.first = project.first;
    a.orbit.last = b.orbit.last = project.last;
    a.orbit.initial = {
        project.first, core::OriginId::earth, core::FrameId::gcrf, {7e6, 0, 0}, {0, 7500, 100}};
    b.orbit.initial = {
        project.first, core::OriginId::earth, core::FrameId::gcrf, {7.5e6, 0, 0}, {0, -7000, 1000}};
    project.satellites = {a, b};
    std::shared_ptr<const runtime::ReportResult> published;
    core::Epoch sought{-1};
    int preparations = 0;
    ReportDialog dialog(
        project, time,
        [&](const application::ReportQuery &, auto ready) {
            ++preparations;
            ready(ReportInput{project, data}, {});
        },
        [&](auto reports) { project.reports = std::move(reports); },
        [&](auto epoch) { sought = epoch; });
    dialog.result_published = [&](auto r, const auto &) { published = r; };
    dialog.select_object("primary", application::ReportKind::relative_motion);
    dialog.resize(1140, 780);
    dialog.show();
    auto *primary = dialog.findChild<ObjectSelectionField *>("accessStation");
    auto *target = dialog.findChild<ObjectSelectionField *>("accessTarget");
    EXPECT_EQ(primary->id(), "primary");
    EXPECT_EQ(target->id(), "target");
    EXPECT_EQ(primary->accessibleName(), QStringLiteral("基准卫星"));
    dialog.findChild<QPushButton *>("generateReport")->click();
    wait_for_report(dialog);
    ASSERT_TRUE(published);
    ASSERT_TRUE(published->intersatellite_statistics);
    EXPECT_EQ(published->definition.query.object_ids,
              (std::vector<std::string>{"primary", "target"}));
    auto *table = dialog.findChild<QTableView *>("reportTable");
    auto *views = dialog.findChild<QTabBar *>("reportViews");
    EXPECT_EQ(table->model()->rowCount(), 11);
    EXPECT_TRUE(views->isTabEnabled(1));
    auto *notice = dialog.findChild<QLabel *>("orbitalEventNotice");
    QMetaObject::invokeMethod(notice, "linkActivated", Qt::DirectConnection,
                              Q_ARG(QString, QString("closest-approach")));
    EXPECT_EQ(sought, published->intersatellite_statistics->minimum_range.epoch);
    const auto capture = qEnvironmentVariable("ORBITFORGE_REPORT_UI_CAPTURE");
    if (!capture.isEmpty())
        QDir().mkpath(capture);
    const auto screenshots = [&](const QString &name) {
        for (int width : {1140, 420}) {
            dialog.resize(width, 780);
            for (int i = 0; i < 5; ++i) {
                QCoreApplication::processEvents();
                QThread::msleep(10);
            }
            EXPECT_LE(dialog.minimumSizeHint().width(), 420);
            EXPECT_EQ(primary->id(), "primary");
            EXPECT_EQ(target->id(), "target");
            if (!capture.isEmpty())
                dialog.grab().save(capture + QString("/%1-%2.png").arg(name).arg(width));
        }
    };
    screenshots("relative-table");
    auto *toggle = dialog.findChild<QPushButton *>("reportToggleSettings");
    toggle->click();
    QCoreApplication::processEvents();
    EXPECT_TRUE(primary->isVisible());
    EXPECT_TRUE(target->isVisible());
    if (!capture.isEmpty())
        dialog.grab().save(capture + "/relative-settings-420.png");
    toggle->click();
    QCoreApplication::processEvents();
    views->setCurrentIndex(1);
    screenshots("relative-curve");
    EXPECT_FALSE(
        static_cast<ReportPlot *>(dialog.findChild<QWidget *>("reportPlot"))->image(1).isNull());
    dialog.findChild<QComboBox *>("reportUnits")->setCurrentIndex(1);
    EXPECT_EQ(preparations, 1);
    choose_report(dialog, static_cast<int>(application::ReportKind::intersatellite_access));
    dialog.findChild<QPushButton *>("generateReport")->click();
    wait_for_report(dialog);
    ASSERT_TRUE(published);
    EXPECT_EQ(preparations, 2);
    EXPECT_FALSE(views->isTabEnabled(1));
    ASSERT_GT(table->model()->rowCount(), 0);
    const auto nearest = std::get<core::Epoch>(published->table.rows()[0][8]);
    QMetaObject::invokeMethod(table, "doubleClicked", Qt::DirectConnection,
                              Q_ARG(QModelIndex, table->model()->index(0, 7)));
    EXPECT_EQ(sought, nearest);
    screenshots("intersatellite-access");
    const auto rows = published->table.rows();
    target->set_id("primary");
    dialog.findChild<QPushButton *>("generateReport")->click();
    wait_for_report(dialog);
    EXPECT_EQ(preparations, 2);
    EXPECT_EQ(published->table.rows(), rows);
    target->set_id("target");
    project.satellites.pop_back();
    dialog.update_project(project, time);
    EXPECT_EQ(target->id(), "target");
    dialog.findChild<QPushButton *>("generateReport")->click();
    wait_for_report(dialog);
    EXPECT_EQ(published->table.rows(), rows);
}
TEST(ReportDialog, CoverageHeatmapPointWindowsTemplatesAndResponsiveLayout) {
    (void)test_app();
    if (!runtime::two_body_available())
        GTEST_SKIP();
    std::shared_ptr<const core::ITimeProvider> time = runtime::make_builtin_time_provider();
    std::shared_ptr<const core::IEarthOrientationProvider> eop =
        runtime::load_eop_snapshot(ORBITFORGE_ACCESS_EOP);
    const auto data =
        runtime::DataSnapshot::from_providers(time, runtime::make_frame_provider(), eop);
    application::Project project{"Coverage · 区域与重访", core::Epoch{0},
                                 core::Epoch{12000'000'000'000}};
    project.earth_orientation = eop->data_info();
    application::ProjectSatellite sat;
    sat.orbit.object_id = "asset";
    sat.display_name = "LEO · 覆盖卫星";
    sat.orbit.first = project.first;
    sat.orbit.last = project.last;
    sat.orbit.initial = {
        project.first, core::OriginId::earth, core::FrameId::gcrf, {7e6, 0, 0}, {0, 6200, 5300}};
    project.satellites.push_back(sat);
    int preparations = 0;
    core::Epoch sought{-1};
    std::shared_ptr<const runtime::ReportResult> result;
    ReportDialog dialog(
        project, time,
        [&](const auto &, auto ready) {
            ++preparations;
            ready(ReportInput{project, data}, {});
        },
        [&](auto reports) { project.reports = std::move(reports); },
        [&](auto epoch) { sought = epoch; });
    dialog.result_published = [&](auto r, const auto &) { result = r; };
    dialog.select_object("asset", application::ReportKind::coverage_grid);
    dialog.show();
    dialog.resize(1140, 820);
    QCoreApplication::processEvents();
    dialog.findChild<QPushButton *>("coverageGlobal")->click();
    dialog.findChild<QDoubleSpinBox *>("coverageResolution")->setValue(20);
    dialog.findChild<QPushButton *>("generateReport")->click();
    wait_for_report(dialog, 20000);
    ASSERT_TRUE(result);
    ASSERT_TRUE(result->coverage_statistics);
    auto *views = dialog.findChild<QTabBar *>("reportViews");
    EXPECT_EQ(views->currentIndex(), 2);
    EXPECT_FALSE(views->isTabEnabled(1));
    auto *map = static_cast<CoverageMap *>(dialog.findChild<QWidget *>("coverageMap"));
    ASSERT_TRUE(map);
    EXPECT_FALSE(map->image().isNull());
    const auto &points = result->coverage_statistics->points;
    auto found = std::ranges::find_if(points, [](const auto &p) { return !p.windows.empty(); });
    ASSERT_NE(found, points.end());
    const auto index = static_cast<std::size_t>(found - points.begin());
    map->select_point(index);
    EXPECT_EQ(dialog.findChild<QComboBox *>("coverageWindows")->count(),
              static_cast<int>(found->windows.size()));
    dialog.findChild<QPushButton *>("coverageJump")->click();
    EXPECT_EQ(sought, found->windows.front().first);
    QKeyEvent right(QEvent::KeyPress, Qt::Key_Right, Qt::NoModifier);
    QApplication::sendEvent(map, &right);
    EXPECT_TRUE(dialog.findChild<QLabel *>("coveragePointDetail")
                    ->text()
                    .contains(QString::number((index + 1) % points.size() + 1)));
    const auto capture = qEnvironmentVariable("ORBITFORGE_REPORT_UI_CAPTURE");
    if (!capture.isEmpty())
        QDir().mkpath(capture);
    for (int width : {1140, 420}) {
        dialog.resize(width, 820);
        for (int i = 0; i < 5; ++i) {
            QCoreApplication::processEvents();
            QThread::msleep(10);
        }
        EXPECT_LE(dialog.minimumSizeHint().width(), 420);
        EXPECT_LE(map->width(), dialog.width());
        if (!capture.isEmpty())
            dialog.grab().save(capture + QString("/coverage-map-%1.png").arg(width));
    }
    auto *toggle = dialog.findChild<QPushButton *>("reportToggleSettings");
    toggle->click();
    QCoreApplication::processEvents();
    EXPECT_TRUE(dialog.findChild<QWidget *>("coverageSettings")->isVisible());
    if (!capture.isEmpty())
        dialog.grab().save(capture + "/coverage-settings-420.png");
    toggle->click();
    dialog.findChild<QComboBox *>("coverageMetric")->setCurrentIndex(1);
    if (!capture.isEmpty())
        dialog.grab().save(capture + "/coverage-revisit-420.png");
    const auto selected_detail = dialog.findChild<QLabel *>("coveragePointDetail")->text();
    dialog.findChild<QComboBox *>("reportUnits")->setCurrentIndex(1);
    EXPECT_EQ(dialog.findChild<QLabel *>("coveragePointDetail")->text(), selected_detail);
    EXPECT_EQ(preparations, 1);
    save_template(dialog, QStringLiteral("全球覆盖"));
    ASSERT_EQ(project.reports.size(), 1U);
    EXPECT_EQ(project.reports[0].query.object_ids, (std::vector<std::string>{"asset"}));
    EXPECT_DOUBLE_EQ(project.reports[0].query.coverage.west_rad, -std::numbers::pi);
    choose_report(dialog, static_cast<int>(application::ReportKind::coverage_time));
    dialog.findChild<QDoubleSpinBox *>("coverageResolution")->setValue(30);
    dialog.findChild<QPushButton *>("coverageGlobal")->click();
    dialog.findChild<QPushButton *>("generateReport")->click();
    wait_for_report(dialog, 20000);
    ASSERT_EQ(result->definition.query.kind, application::ReportKind::coverage_time);
    EXPECT_EQ(views->currentIndex(), 1);
    EXPECT_TRUE(views->isTabEnabled(2));
    dialog.resize(1140, 820);
    QCoreApplication::processEvents();
    if (!capture.isEmpty())
        dialog.grab().save(capture + "/coverage-curve-1140.png");
    auto previous = result;
    dialog.findChild<QDoubleSpinBox *>("coverageNorth")->setValue(-90);
    dialog.findChild<QPushButton *>("generateReport")->click();
    wait_for_report(dialog);
    EXPECT_EQ(result, previous);
    EXPECT_EQ(preparations, 2);
    choose_report(dialog, static_cast<int>(application::report_kinds.size()));
    project.satellites.clear();
    dialog.update_project(project, time);
    dialog.findChild<QPushButton *>("generateReport")->click();
    wait_for_report(dialog);
    EXPECT_EQ(result, previous);
    EXPECT_EQ(preparations, 2);
}
} // namespace orbitforge::desktop
