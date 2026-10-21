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
#include <QClipboard>
#include <QComboBox>
#include <QElapsedTimer>
#include <QFile>
#include <QFileDialog>
#include <QHeaderView>
#include <QLabel>
#include <QMimeData>
#include <QPushButton>
#include <QSortFilterProxyModel>
#include <QTabBar>
#include <QTableView>
#include <QTemporaryDir>
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
TEST(ReportDialog, ColumnsRetainSortingSelectionWidthsCurvesAndExportWithoutCalculation) {
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
    application::Project project("报告列", core::Epoch{0}, core::Epoch{120'000'000'001});
    project.ground_stations.push_back({"low", "低点", {0, 0, 0}});
    project.ground_stations.push_back({"high", "高点", {0, 0, 1000}});
    application::ReportDefinition definition;
    definition.id = "custom";
    definition.name = "自定义位置";
    definition.query.frame = core::FrameId::itrf;
    definition.query.object_ids = {"low", "high"};
    definition.format.time_scale = application::ReportTimeScale::tai;
    definition.format.columns = {{"state.x", "m", "横向位置"},
                                 {"time.epoch", "", ""},
                                 {"object.name", "", ""},
                                 {"object.id", "", ""}};
    project.reports.push_back(definition);
    int preparations = 0, publications = 0;
    std::optional<core::Epoch> sought;
    std::shared_ptr<const runtime::ReportResult> source_result;
    ReportDialog dialog(
        project, time,
        [&](const application::ReportQuery &, ReportDialog::Ready ready) {
            ++preparations;
            ready(ReportInput{project, runtime::DataSnapshot::from_providers(time)}, {});
        },
        [&](auto reports) { project.reports = std::move(reports); },
        [&](core::Epoch epoch) { sought = epoch; });
    dialog.result_published = [&](auto result, const runtime::Analysis &) {
        ++publications;
        source_result = std::move(result);
    };
    dialog.show();
    dialog.resize(1120, 740);
    choose_report(dialog, static_cast<int>(application::report_kinds.size()));
    dialog.findChild<QPushButton *>("generateReport")->click();
    wait_for_report(dialog);
    ASSERT_TRUE(source_result);
    auto *table = dialog.findChild<QTableView *>("reportTable");
    auto *proxy = qobject_cast<QSortFilterProxyModel *>(table->model());
    auto *views = dialog.findChild<QTabBar *>("reportViews");
    auto *plot = static_cast<ReportPlot *>(dialog.findChild<QWidget *>("reportPlot"));
    auto *columns = dialog.findChild<QAction *>("reportChooseColumns");
    ASSERT_TRUE(proxy && columns->isEnabled());
    EXPECT_EQ(available_curves(dialog).size(), 6); // Complete source, not just displayed X.
    select_curves(dialog, {"state.x", "state.y"});
    EXPECT_EQ(plot->series_count(), 4U);
    EXPECT_EQ(plot->panel_count(), 2U); // Explicit X metres and engineering Y kilometres.
    views->setCurrentIndex(1);
    QCoreApplication::processEvents();
    plot->zoom(.65);
    const auto interval = plot->visible_interval();
    const auto original_image = plot->image(1);
    table->sortByColumn(table_column(table, "state.x"), Qt::DescendingOrder);
    table->setColumnWidth(table_column(table, "state.x"), 237);
    table->setCurrentIndex(proxy->index(1, table_column(table, "state.x")));
    table->selectRow(1);
    const auto selected_source_row =
        proxy->mapToSource(table->selectionModel()->selectedRows().front()).row();
    select_columns(dialog, {"state.y", "state.x"}, false);
    EXPECT_EQ(proxy->columnCount(), 4);
    select_columns(dialog, {"state.y", "state.x"});
    ASSERT_EQ(proxy->columnCount(), 2);
    EXPECT_EQ(table_column(table, "state.y"), 0);
    EXPECT_EQ(table_column(table, "state.x"), 1);
    EXPECT_EQ(table->horizontalHeader()->sortIndicatorSection(), 1);
    EXPECT_EQ(table->horizontalHeader()->sortIndicatorOrder(), Qt::DescendingOrder);
    EXPECT_EQ(table->columnWidth(1), 237);
    ASSERT_EQ(table->selectionModel()->selectedRows().size(), 1);
    EXPECT_EQ(proxy->mapToSource(table->selectionModel()->selectedRows().front()).row(),
              selected_source_row);
    EXPECT_EQ(proxy->mapToSource(table->currentIndex()).row(), selected_source_row);
    EXPECT_EQ(table->currentIndex().column(), 1);
    EXPECT_EQ(plot->visible_interval(), interval);
    EXPECT_EQ(plot->image(1), original_image); // Columns alone do not rebuild or alter the plot.
    EXPECT_EQ(plot->fields().front().unit, "m");
    EXPECT_TRUE(plot->fields().front().label.contains(QStringLiteral("横向位置")));
    EXPECT_TRUE(views->isTabEnabled(1));
    table->doubleClicked(table->selectionModel()->selectedRows().front());
    ASSERT_TRUE(sought);
    EXPECT_EQ(*sought, std::get<core::Epoch>(
                           source_cell(*source_result, selected_source_row, "time.epoch")));
    select_columns(dialog, {"state.y"}); // Even hiding the plotted X keeps its unit and title.
    auto *units = dialog.findChild<QComboBox *>("reportUnits");
    units->setCurrentIndex(1);
    EXPECT_EQ(plot->fields().front().unit, "m");
    EXPECT_TRUE(plot->fields().front().label.contains(QStringLiteral("横向位置")));
    EXPECT_EQ(plot->series_count(), 4U);
    units->setCurrentIndex(0);
    select_columns(dialog, {"state.y", "state.x"});
    EXPECT_EQ(table->columnWidth(1), 237);
    EXPECT_TRUE(
        proxy->headerData(1, Qt::Horizontal).toString().contains(QStringLiteral("横向位置")));
    EXPECT_DOUBLE_EQ(proxy->index(0, 1).data().toString().toDouble(), 6378137);
    views->setCurrentIndex(0);
    table->selectRow(0);
    dialog.findChild<QAction *>("reportTableCopy")->trigger();
    const auto copied = QApplication::clipboard()->text().split('\n');
    ASSERT_GE(copied.size(), 2);
    EXPECT_EQ(copied[0].count('\t'), 1);
    EXPECT_EQ(copied[1].count('\t'), 1);
    EXPECT_TRUE(copied[0].trimmed().endsWith(QStringLiteral("横向位置 (m)")));
    save_template(dialog, QStringLiteral("两列位置"));
    ASSERT_EQ(project.reports.size(), 2U);
    EXPECT_EQ(project.reports.back().format.columns.size(), 2U);
    EXPECT_EQ(project.reports.back().format.columns[0].field, "state.y");
    EXPECT_EQ(project.reports.back().format.columns[1].unit, "m");
    EXPECT_TRUE(columns->isEnabled()); // Saving a matching template keeps the result usable.
    EXPECT_FALSE(dialog.findChild<QLabel *>("reportResultLabel")
                     ->text()
                     .contains(QStringLiteral("设置已更改")));
    const auto round_trip = application::decode_project(application::encode_project(project));
    EXPECT_EQ(round_trip.reports.back().format.columns, project.reports.back().format.columns);
    QTemporaryDir output;
    ASSERT_TRUE(output.isValid());
    const auto file = output.filePath("chosen-columns.csv");
    const bool native = QApplication::testAttribute(Qt::AA_DontUseNativeDialogs);
    QApplication::setAttribute(Qt::AA_DontUseNativeDialogs, true);
    QTimer::singleShot(0, [file] {
        auto *chooser = qobject_cast<QFileDialog *>(QApplication::activeModalWidget());
        ASSERT_TRUE(chooser);
        chooser->selectFile(file);
        QMetaObject::invokeMethod(chooser, "accept", Qt::QueuedConnection);
    });
    dialog.findChild<QPushButton *>("exportReportCsv")->click();
    QApplication::setAttribute(Qt::AA_DontUseNativeDialogs, native);
    wait_for_report(dialog);
    QFile csv(file);
    ASSERT_TRUE(csv.open(QIODevice::ReadOnly));
    const auto rows = QString::fromUtf8(csv.readAll()).trimmed().split('\n');
    ASSERT_EQ(rows.size(), proxy->rowCount() + 1);
    EXPECT_EQ(rows[0].count(','), 1);
    EXPECT_TRUE(rows[0].contains(QStringLiteral("横向位置")));
    for (int i = 1; i < rows.size(); ++i)
        EXPECT_EQ(rows[i].count(','), 1);
    // A presentation changed while the chooser is open must not be overwritten.
    QTimer::singleShot(0, [&] {
        auto *chooser =
            dynamic_cast<ReportColumnSelectionDialog *>(QApplication::activeModalWidget());
        ASSERT_TRUE(chooser);
        chooser->findChild<QPushButton *>("reportColumnAll")->click();
        units->setCurrentIndex(1);
        chooser->accept();
    });
    columns->trigger();
    EXPECT_EQ(proxy->columnCount(), 2);
    EXPECT_TRUE(
        dialog.findChild<QLabel *>("reportStatus")->text().contains(QStringLiteral("请重新选择")));
    EXPECT_EQ(preparations, 1);
    EXPECT_EQ(publications, 1);
    EXPECT_EQ(source_result->table.columns().size(), 4U);
    EXPECT_EQ(source_result->source_table->rows().size(), 8U);
}

TEST(ReportDialog, ColumnOnlyResultsExplainMissingUtcAndEnableCurvesWithTaiWithoutRegeneration) {
    (void)test_app();
    application::Project project("无 UTC 数据", core::Epoch{0}, core::Epoch{120'000'000'000});
    project.ground_stations.push_back({"station", "地面站", {0, 0, 0}});
    application::ReportDefinition definition;
    definition.id = "column-only";
    definition.name = "位置";
    definition.query.frame = core::FrameId::itrf;
    definition.query.object_ids = {"station"};
    definition.format.columns = {{"state.x", "", ""}};
    project.reports.push_back(definition);
    int preparations = 0;
    ReportDialog dialog(
        project, nullptr,
        [&](const application::ReportQuery &, ReportDialog::Ready ready) {
            ++preparations;
            ready(ReportInput{project, runtime::DataSnapshot::from_providers(nullptr)}, {});
        },
        [](auto) {}, [](auto) {});
    choose_report(dialog, static_cast<int>(application::report_kinds.size()));
    dialog.findChild<QPushButton *>("generateReport")->click();
    wait_for_report(dialog);
    auto *table = dialog.findChild<QTableView *>("reportTable");
    auto *views = dialog.findChild<QTabBar *>("reportViews");
    ASSERT_EQ(table->model()->columnCount(), 1)
        << dialog.findChild<QLabel *>("reportStatus")->text().toStdString();
    ASSERT_GT(table->model()->rowCount(), 0);
    EXPECT_TRUE(views->isTabVisible(1));
    EXPECT_FALSE(views->isTabEnabled(1));
    EXPECT_TRUE(views->tabToolTip(1).contains("TAI"));
    dialog.findChild<QComboBox *>("reportTimeScale")->setCurrentIndex(1);
    EXPECT_TRUE(views->isTabEnabled(1));
    EXPECT_TRUE(views->tabToolTip(1).isEmpty());
    EXPECT_EQ(available_curves(dialog).size(), 6);
    EXPECT_EQ(preparations, 1);
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
} // namespace
} // namespace orbitforge::desktop
