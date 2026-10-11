#include "../src/viewer/epoch_editor.hpp"
#include "../src/viewer/report_dialog.hpp"
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
#include <QLabel>
#include <QLineEdit>
#include <QListWidget>
#include <QMimeData>
#include <QPlainTextEdit>
#include <QPushButton>
#include <QScrollArea>
#include <QScrollBar>
#include <QStandardItemModel>
#include <QTableView>
#include <QThread>
#include <QTimer>
#include <QToolButton>
#include <gtest/gtest.h>
#include <orbitforge/runtime/time_frames.hpp>

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
    dialog.findChild<QLineEdit *>("reportName")->setText(QStringLiteral("沿海站位置"));
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
    dialog.findChild<QPushButton *>("saveReport")->click();
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
                        .contains("builtin-reports-v1"));
        const auto directory = qEnvironmentVariable("ORBITFORGE_REPORT_UI_CAPTURE");
        if (!directory.isEmpty())
            EXPECT_TRUE(source->grab().save(directory + "/provenance.png"));
        source->accept();
    });
    dialog.findChild<QToolButton *>("reportShowSource")->click();
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
    library->setCurrentIndex(4);
    EXPECT_EQ(dialog.findChild<QLineEdit *>("reportName")->text(), QStringLiteral("沿海站位置"));
    dialog.findChild<QAction *>("deleteReport")->trigger();
    EXPECT_TRUE(project.reports.empty());
    dialog.select_object("station", application::ReportKind::ground_stations);
    dialog.findChild<QAction *>("duplicateReport")->trigger();
    ASSERT_EQ(project.reports.size(), 1U);
    EXPECT_EQ(project.reports[0].object_ids, std::vector<std::string>{"station"});
    const auto original_id = project.reports[0].id;
    dialog.findChild<QAction *>("duplicateReport")->trigger();
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
    dialog.findChild<QPushButton *>("saveReport")->click();
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
    library->setCurrentIndex(4);
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
} // namespace
} // namespace orbitforge::desktop
