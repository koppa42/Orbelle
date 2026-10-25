#include "../src/viewer/coverage_map.hpp"
#include "../src/viewer/epoch_editor.hpp"
#include "../src/viewer/object_selection.hpp"
#include "../src/viewer/report_dialog.hpp"
#include "../src/viewer/report_plot.hpp"
#include "../src/viewer/report_selection.hpp"
#include "../src/viewer/workbench_style.hpp"
#include "../src/viewer/workspace.hpp"
#include "support/ui_test_support.hpp"
#include "window_show_observer.hpp"
#include <QApplication>
#include <QComboBox>
#include <QDir>
#include <QDoubleSpinBox>
#include <QElapsedTimer>
#include <QHelpEvent>
#include <QKeyEvent>
#include <QLabel>
#include <QMouseEvent>
#include <QPushButton>
#include <QTabBar>
#include <QTableView>
#include <QThread>
#include <QTimer>
#include <QToolTip>
#include <QTreeWidget>
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
TEST(ReportDialog, AccessPairSensorScopeCurveAndDeletedBinding) {
    (void)test_app();
    if (!runtime::two_body_available())
        GTEST_SKIP();
    std::shared_ptr<const core::ITimeProvider> time = runtime::make_builtin_time_provider();
    std::shared_ptr<const core::IEarthOrientationProvider> eop =
        runtime::load_eop_snapshot(ORBELLE_ACCESS_EOP);
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
    const auto expected_epoch =
        std::get<core::Epoch>(source_cell(*pass_result, 0, "access.max_elevation_time"));
    EXPECT_TRUE(QMetaObject::invokeMethod(
        table, "doubleClicked", Qt::DirectConnection,
        Q_ARG(QModelIndex, table->model()->index(0, table_column(table, "access.max_elevation")))));
    ASSERT_TRUE(sought);
    EXPECT_EQ(*sought, expected_epoch);
    auto *search = dialog.findChild<QComboBox *>("accessSearchMode");
    auto *tolerance = dialog.findChild<QDoubleSpinBox *>("accessTimeTolerance");
    EXPECT_EQ(search->currentIndex(), 1);
    const auto settle_layout = [] {
        for (int i = 0; i < 8; ++i)
            QCoreApplication::processEvents();
    };
    const auto access_capture = qEnvironmentVariable("ORBELLE_REPORT_UI_CAPTURE");
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
    const auto capture = qEnvironmentVariable("ORBELLE_REPORT_UI_CAPTURE");
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
    EXPECT_THROW(table_column(table, "lighting.start_clipped"), std::runtime_error);
    EXPECT_THROW(table_column(table, "lighting.stop_clipped"), std::runtime_error);
    const auto end = std::get<core::Epoch>(source_cell(*published, 1, "lighting.stop"));
    QMetaObject::invokeMethod(
        table, "doubleClicked", Qt::DirectConnection,
        Q_ARG(QModelIndex, table->model()->index(1, table_column(table, "lighting.stop"))));
    EXPECT_EQ(sought, end);
    const auto capture = qEnvironmentVariable("ORBELLE_REPORT_UI_CAPTURE");
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
        EXPECT_EQ(result->algorithm, "hpop-joint-covariance-sequential-samples-v2");
        EXPECT_EQ(result->table.rows().size(), 2);
        EXPECT_EQ(analysis.snapshot().project->satellites.front().orbit.covariance,
                  satellite.orbit.covariance);
        const auto covariance = core::propagate_hpop_covariance(
            satellite.orbit.initial, project.last, satellite.orbit.covariance.active_input(),
            satellite.orbit.hpop, data.force_environment(satellite.orbit));
        EXPECT_EQ(std::get<double>(
                      source_cell(*result, result->table.rows().size() - 1, "covariance.x.x")),
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
    ASSERT_EQ(table->model()->columnCount(), 24);
    ASSERT_EQ(table->model()->rowCount(), 2);
    const auto xx = table_column(table, "covariance.x.x");
    EXPECT_DOUBLE_EQ(table->model()->data(table->model()->index(0, xx)).toString().toDouble(), .01);
    EXPECT_TRUE(table->model()->headerData(xx, Qt::Horizontal).toString().contains("km^2"));
    dialog.findChild<QComboBox *>("reportUnits")->setCurrentIndex(1);
    EXPECT_EQ(preparations, 1);
    EXPECT_DOUBLE_EQ(table->model()->data(table->model()->index(0, xx)).toString().toDouble(),
                     10000);
    EXPECT_TRUE(table->model()->headerData(xx, Qt::Horizontal).toString().contains("m^2"));
    auto *plot = static_cast<ReportPlot *>(dialog.findChild<QWidget *>("reportPlot"));
    EXPECT_FALSE(plot->image(1).isNull());
    project.satellites[0].orbit.covariance.enabled = false;
    dialog.update_project(project, data.time());
    EXPECT_FALSE(
        report_available(dialog, static_cast<int>(application::ReportKind::state_covariance)));
}

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
    const auto epoch = std::get<core::Epoch>(source_cell(*published, 2, "time.epoch"));
    QMetaObject::invokeMethod(
        table, "doubleClicked", Qt::DirectConnection,
        Q_ARG(QModelIndex, table->model()->index(2, table_column(table, "event.kind"))));
    EXPECT_EQ(sought, epoch);
    const auto capture = qEnvironmentVariable("ORBELLE_REPORT_UI_CAPTURE");
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

TEST(ReportDialog, IntersatellitePairCurvesExtremaSeekAndResponsiveLayout) {
    (void)test_app();
    if (!runtime::two_body_available())
        GTEST_SKIP();
    std::shared_ptr<const core::ITimeProvider> time = runtime::make_builtin_time_provider();
    std::shared_ptr<const core::IEarthOrientationProvider> eop =
        runtime::load_eop_snapshot(ORBELLE_ACCESS_EOP);
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
    const auto capture = qEnvironmentVariable("ORBELLE_REPORT_UI_CAPTURE");
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
    const auto nearest = std::get<core::Epoch>(source_cell(*published, 0, "access.min_range_time"));
    QMetaObject::invokeMethod(
        table, "doubleClicked", Qt::DirectConnection,
        Q_ARG(QModelIndex, table->model()->index(0, table_column(table, "access.min_range"))));
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

TEST(CoverageMap, DataHintsWaitForHelpEventAndRefreshAfterMetricChange) {
    (void)test_app();
    application::ReportDefinition definition;
    auto &grid = definition.query.coverage;
    grid.south_rad = grid.west_rad = -.1;
    grid.north_rad = grid.east_rad = .1;
    auto result = std::make_shared<runtime::ReportResult>(
        runtime::ReportResult{definition, results::Table{{}, {}}});
    result->last = core::Epoch{3600'000'000'000};
    result->coverage_statistics = results::CoverageStatistics{};
    result->coverage_statistics->points.push_back(
        {{{0, 0, 0}, -.1, .1, -.1, .1, 1e6}, {}, {1800, 900, 900, 1}});
    CoverageMap map;
    map.resize(600, 400);
    map.set_result(result);
    map.show();
    QCoreApplication::processEvents();
    const QPoint point(300, 180);
    QMouseEvent move(QEvent::MouseMove, point, map.mapToGlobal(point), Qt::NoButton, Qt::NoButton,
                     Qt::NoModifier);
    QCoreApplication::sendEvent(&map, &move);
    EXPECT_FALSE(QToolTip::isVisible());
    QHelpEvent help(QEvent::ToolTip, point, map.mapToGlobal(point));
    QCoreApplication::sendEvent(&map, &help);
    EXPECT_TRUE(help.isAccepted());
    EXPECT_TRUE(QToolTip::isVisible());
    EXPECT_TRUE(QToolTip::text().contains("50.000 %"));
    map.set_metric(1);
    ASSERT_TRUE(wait_for_ui([] { return !QToolTip::isVisible(); }, 600));
    QCoreApplication::sendEvent(&map, &help);
    EXPECT_TRUE(QToolTip::text().contains("0.250 h"));
    map.set_result({});
    ASSERT_TRUE(wait_for_ui([] { return !QToolTip::isVisible(); }, 600));
    QCoreApplication::sendEvent(&map, &help);
    EXPECT_FALSE(help.isAccepted());
    EXPECT_FALSE(QToolTip::isVisible());
}

TEST(CoverageMap, CachedPaintMatchesFreshMapAfterSelectionMetricResizeAndResultChange) {
    (void)test_app();
    application::ReportDefinition definition;
    auto &grid = definition.query.coverage;
    grid.south_rad = grid.west_rad = -.1;
    grid.north_rad = grid.east_rad = .1;
    auto result = std::make_shared<runtime::ReportResult>(
        runtime::ReportResult{definition, results::Table{{}, {}}});
    result->last = core::Epoch{3600'000'000'000};
    result->coverage_statistics = results::CoverageStatistics{};
    auto &stats = *result->coverage_statistics;
    stats.maximum_gap_seconds = 1800;
    stats.points.push_back({{{-.05, 0, 0}, -.1, .1, -.1, 0, 1e6}, {}, {1800, 900, 900, 1}});
    stats.points.push_back({{{.05, 0, 0}, -.1, .1, 0, .1, 1e6}, {}, {900, 1800, 1800, 1}});
    CoverageMap map;
    map.resize(600, 400);
    map.set_result(result);
    const auto initial = map.grab().toImage();
    map.select_point(1);
    EXPECT_NE(initial, map.grab().toImage());
    const auto compare = [&](int metric, std::shared_ptr<const runtime::ReportResult> expected) {
        CoverageMap fresh;
        fresh.resize(map.size());
        fresh.set_result(std::move(expected));
        fresh.set_metric(metric);
        fresh.select_point(map.point_index());
        EXPECT_EQ(map.grab().toImage(), fresh.grab().toImage());
    };
    compare(0, result);
    map.set_metric(1);
    compare(1, result);
    map.resize(420, 300);
    compare(1, result);
    map.set_result({});
    compare(1, {});
}

TEST(ReportDialog, DenseCoverageEstimateUsesMemorySettingAndDiscardsOlderEdits) {
    (void)test_app();
    if (!runtime::time_frames_available())
        GTEST_SKIP();
    std::shared_ptr<const core::ITimeProvider> time = runtime::make_builtin_time_provider();
    application::Project project{"Dense estimate", core::Epoch{0}, core::Epoch{600'000'000'000}};
    application::ProjectSatellite sat;
    sat.orbit.object_id = "asset";
    sat.orbit.first = project.first;
    sat.orbit.last = project.last;
    sat.orbit.initial = {
        project.first, core::OriginId::earth, core::FrameId::gcrf, {7e6, 0, 0}, {0, 6200, 5300}};
    project.satellites.push_back(sat);
    application::ProjectRegion region;
    region.object_id = "world";
    region.boundary = {
        -std::numbers::pi / 2, std::numbers::pi / 2, -std::numbers::pi, std::numbers::pi, {}};
    region.grid_resolution_rad = .25 * std::numbers::pi / 180;
    project.regions.push_back(region);
    ReportDialog dialog(project, time, [](const auto &, auto) {}, [](auto) {}, [](auto) {});
    application::ReportDefinition definition;
    definition.id = definition.name = "Dense coverage";
    definition.query.kind = application::ReportKind::coverage_grid;
    definition.query.object_ids = {"asset"};
    definition.query.region_id = "world";
    ASSERT_TRUE(dialog.configure_report(definition));
    dialog.show();
    auto *memory = dialog.findChild<QDoubleSpinBox *>("coverageMemoryMiB");
    auto *estimate = dialog.findChild<QLabel *>("reportEstimate");
    ASSERT_TRUE(memory && estimate);
    EXPECT_FALSE(dialog.findChild<QDoubleSpinBox *>("coverageResolution"));
    ASSERT_TRUE(wait_for_ui([&] { return estimate->text().contains("budget"); }, 3000));
    memory->setValue(1536);
    ASSERT_TRUE(
        wait_for_ui([&] { return estimate->text().contains(QStringLiteral("个网格点")); }, 3000));
    EXPECT_TRUE(estimate->text().contains("MiB"));
    project.regions.front().grid_resolution_rad = .01 * std::numbers::pi / 180;
    dialog.update_project(project, time);
    project.regions.front().grid_resolution_rad = std::numbers::pi / 180;
    dialog.update_project(project, time);
    ASSERT_TRUE(wait_for_ui([&] { return estimate->text().startsWith("41344"); }, 3000));
}

TEST(ReportDialog, CoverageHeatmapPointWindowsTemplatesAndResponsiveLayout) {
    (void)test_app();
    if (!runtime::two_body_available())
        GTEST_SKIP();
    std::shared_ptr<const core::ITimeProvider> time = runtime::make_builtin_time_provider();
    std::shared_ptr<const core::IEarthOrientationProvider> eop =
        runtime::load_eop_snapshot(ORBELLE_ACCESS_EOP);
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
    application::ProjectRegion region;
    region.object_id = "world";
    region.display_name = "全球区域";
    region.boundary = {
        -std::numbers::pi / 2, std::numbers::pi / 2, -std::numbers::pi, std::numbers::pi, {}};
    region.grid_resolution_rad = 20 * std::numbers::pi / 180;
    project.regions.push_back(region);
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
    application::ReportDefinition definition;
    definition.id = definition.name = "Global coverage";
    definition.query.kind = application::ReportKind::coverage_grid;
    definition.query.object_ids = {"asset"};
    definition.query.region_id = "world";
    ASSERT_TRUE(dialog.configure_report(definition));
    std::string edited_region;
    dialog.open_region_properties = [&](const auto &id) { edited_region = id; };
    dialog.findChild<QPushButton *>("coverageRegionProperties")->click();
    EXPECT_EQ(edited_region, "world");
    dialog.show();
    dialog.resize(1140, 820);
    QCoreApplication::processEvents();
    EXPECT_FALSE(dialog.findChild<QPushButton *>("coverageGlobal"));
    EXPECT_FALSE(dialog.findChild<QDoubleSpinBox *>("coverageNorth"));
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
    const auto capture = qEnvironmentVariable("ORBELLE_REPORT_UI_CAPTURE");
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
    project.regions.front().grid_resolution_rad = 30 * std::numbers::pi / 180;
    dialog.update_project(project, time);
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
    project.regions.clear();
    dialog.update_project(project, time);
    dialog.findChild<QPushButton *>("generateReport")->click();
    wait_for_report(dialog);
    EXPECT_EQ(result, previous);
    EXPECT_EQ(preparations, 2);
    choose_report(dialog, static_cast<int>(application::report_kinds.size()));
    project.regions.push_back(region);
    project.satellites.clear();
    dialog.update_project(project, time);
    dialog.findChild<QPushButton *>("generateReport")->click();
    wait_for_report(dialog);
    EXPECT_EQ(result, previous);
    EXPECT_EQ(preparations, 2);
}
} // namespace
} // namespace orbelle::desktop
