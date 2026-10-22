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
#include <QMouseEvent>
#include <QPushButton>
#include <QSplitter>
#include <QTabBar>
#include <QTableView>
#include <QTemporaryDir>
#include <QThread>
#include <QTimer>
#include <QToolButton>
#include <QTreeWidget>
#include <QWheelEvent>
#include <algorithm>
#include <cmath>
#include <gtest/gtest.h>
#include <numbers>
#include <orbelle/runtime/ephemeris.hpp>
#include <orbelle/runtime/time_frames.hpp>
#include <orbelle/runtime/two_body.hpp>
#include <stdexcept>

#include "support/report_test_support.hpp"

namespace orbelle::desktop {
namespace {
using namespace report_test;
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
        const auto directory = qEnvironmentVariable("ORBELLE_REPORT_UI_CAPTURE");
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
} // namespace
} // namespace orbelle::desktop
