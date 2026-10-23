#include "../src/viewer/eop_catalog.hpp"
#include "../src/viewer/epoch_editor.hpp"
#include "../src/viewer/form_kit.hpp"
#include "../src/viewer/ground_station_editor.hpp"
#include "../src/viewer/hpop_covariance_editor.hpp"
#include "../src/viewer/hpop_settings.hpp"
#include "../src/viewer/mission_editor.hpp"
#include "../src/viewer/object_selection.hpp"
#include "../src/viewer/orbit_editor.hpp"
#include "../src/viewer/project_update.hpp"
#include "../src/viewer/quantity_editor.hpp"
#include "../src/viewer/report_names.hpp"
#include "../src/viewer/sensor_editor.hpp"
#include "../src/viewer/workbench_style.hpp"
#include "window_show_observer.hpp"
#include <QApplication>
#include <QComboBox>
#include <QDir>
#include <QHBoxLayout>
#include <QLineEdit>
#include <QPushButton>
#include <QScrollArea>
#include <QScrollBar>
#include <QTabBar>
#include <QTreeWidget>
#include <cmath>
#include <fstream>
#include <gtest/gtest.h>
#include <iostream>
#include <numbers>
#include <orbelle/runtime/ephemeris.hpp>
#include <orbelle/runtime/j2.hpp>
#include <orbelle/runtime/j4.hpp>
#include <orbelle/runtime/time_frames.hpp>
#include <orbelle/runtime/two_body.hpp>

#include "support/property_editor_fixture.hpp"

namespace orbelle::desktop {
TEST_F(QuantityTest, ShortPropertyPagesKeepActionsVisibleAndPreserveDrafts) {
    apply_workbench_style(*qobject_cast<QApplication *>(QApplication::instance()));
    QWidget host;
    auto *layout = new QHBoxLayout(&host);
    layout->setContentsMargins(0, 0, 0, 0);
    layout->setSizeConstraint(QLayout::SetNoConstraint);
    auto *editor = new OrbitEditor(OrbitInput{});
    editor->setSizePolicy(QSizePolicy::Ignored, QSizePolicy::Ignored);
    layout->addWidget(editor);
    auto *selector = editor->findChild<QComboBox *>("propertyPageSelector");
    auto &axis = field(*editor, "semiMajorAxis");
    text(axis)->setText("7123.456");
    field(*editor, "displayStep").set_value(45, Unit::second);
    editor->show_result("Applied orbit\nMore details", false);
    editor->show_appearance_result("Model failed\nCorrect the file path and retry", true);
    int submissions = 0;
    editor->apply = [&](auto input) {
        ++submissions;
        EXPECT_EQ(input.step_seconds, 45);
    };
    host.show();
    const auto settle = [] {
        for (int i = 0; i < 12; ++i)
            QCoreApplication::processEvents();
    };
    const auto capture = qEnvironmentVariable("ORBELLE_RESPONSIVE_UI_CAPTURE");
    if (!capture.isEmpty())
        ASSERT_TRUE(QDir().mkpath(capture));
    for (const auto size : {QSize{640, 140}, QSize{320, 180}, QSize{1120, 620}, QSize{640, 140}}) {
        host.resize(size);
        for (int page = 0; page < 4; ++page) {
            selector->setCurrentIndex(page);
            settle();
            SCOPED_TRACE(std::to_string(size.width()) + "x" + std::to_string(size.height()) +
                         " page=" + std::to_string(page));
            EXPECT_EQ(host.size(), size);
            EXPECT_EQ(editor->size(), size);
            bool applied_button = false;
            for (auto *button : editor->findChildren<QPushButton *>()) {
                if (!button->isVisible() || !button->objectName().startsWith("apply"))
                    continue;
                applied_button = true;
                for (auto *parent = button->parentWidget(); parent; parent = parent->parentWidget())
                    EXPECT_TRUE(parent->rect().contains(
                        QRect(button->mapTo(parent, QPoint{}), button->size())))
                        << parent->metaObject()->className();
            }
            EXPECT_TRUE(applied_button);
            for (auto *scroll : editor->findChildren<QScrollArea *>())
                if (scroll->isVisible()) {
                    EXPECT_GE(scroll->viewport()->height(), 24);
                    EXPECT_EQ(scroll->horizontalScrollBar()->maximum(), 0);
                    scroll->verticalScrollBar()->setValue(scroll->verticalScrollBar()->maximum());
                    settle();
                    scroll->verticalScrollBar()->setValue(0);
                }
            if (!capture.isEmpty())
                EXPECT_TRUE(host.grab().save(
                    capture +
                    QString("/short-%1x%2-%3.png").arg(size.width()).arg(size.height()).arg(page)));
        }
        EXPECT_EQ(text(axis)->text(), "7123.456");
        EXPECT_EQ(field(*editor, "displayStep").value(Unit::second), 45);
        EXPECT_EQ(submissions, 0);
    }
    selector->setCurrentIndex(0);
    settle();
    editor->findChild<QPushButton *>("applyOrbit")->click();
    EXPECT_EQ(submissions, 1);
}
TEST_F(QuantityTest, PreparingGroundStationDoesNotShowTemporaryWindows) {
    WindowShowObserver windows;
    GroundStationEditor editor;
    editor.set_station({"station", "Station", {1.2, 0.6, 350}}, true);
    editor.set_busy(true);
    editor.set_busy(false);
    editor.show_result("Invalid position");
    QCoreApplication::processEvents();
    EXPECT_FALSE(editor.isVisible());
    EXPECT_TRUE(windows.shown.isEmpty()) << windows.shown.join(", ").toStdString();
}

TEST_F(QuantityTest, ResponsivePropertiesPreserveDraftsAndKeepFieldsInsideViewport) {
    apply_workbench_style(*qobject_cast<QApplication *>(QApplication::instance()));
    OrbitEditor editor(OrbitInput{});
    editor.resize(1120, 620);
    editor.show();
    auto *selector = editor.findChild<QComboBox *>("propertyPageSelector");
    auto *tabs = editor.findChild<QTabBar *>("propertyPageTabs");
    auto *navigation = editor.findChild<QTreeWidget *>("propertyNavigation");
    auto *state = editor.findChild<QComboBox *>("stateRepresentation");
    ASSERT_TRUE(selector && tabs && navigation && state);
    state->setCurrentIndex(1);
    auto &axis = field(editor, "semiMajorAxis");
    text(axis)->setText("7123.456");
    editor.findChild<QComboBox *>("trackExtent")->setCurrentIndex(2);
    text(field(editor, "pastDuration"))->setText("37.5");
    const auto settle = [] {
        for (int i = 0; i < 12; ++i)
            QCoreApplication::processEvents();
    };
    const auto capture = qEnvironmentVariable("ORBELLE_RESPONSIVE_UI_CAPTURE");
    if (!capture.isEmpty())
        ASSERT_TRUE(QDir().mkpath(capture));
    for (int width : {1120, 320, 560, 840, 320, 1120}) {
        const int page = selector->currentIndex();
        editor.resize(width, 620);
        settle();
        EXPECT_EQ(editor.width(), width);
        EXPECT_EQ(selector->currentIndex(), page);
        // Tree beside wide pages, tabs at inspector widths, a selector when narrower.
        EXPECT_EQ(selector->isVisible(), width < 400);
        EXPECT_EQ(tabs->isVisible(), width >= 400 && width < 680);
        EXPECT_EQ(navigation->isVisible(), width >= 680);
        for (int index = 0; index < 4; ++index) {
            if (tabs->isVisible())
                tabs->setCurrentIndex(index);
            else
                selector->setCurrentIndex(index);
            settle();
            EXPECT_EQ(navigation->currentItem()->data(0, Qt::UserRole).toInt(), index);
            EXPECT_EQ(selector->currentIndex(), index);
            EXPECT_EQ(tabs->currentIndex(), index);
            for (auto *scroll : editor.findChildren<QScrollArea *>())
                if (scroll->isVisible()) {
                    EXPECT_EQ(scroll->horizontalScrollBar()->maximum(), 0)
                        << "width=" << width << ", page=" << index;
                    EXPECT_LE(scroll->widget()->width(), scroll->viewport()->width())
                        << "width=" << width << ", page=" << index;
                }
            if (!capture.isEmpty() && (width == 320 || width == 1120))
                EXPECT_TRUE(editor.grab().save(
                    capture + QString("/properties-%1-%2.png").arg(width).arg(index)));
        }
        EXPECT_EQ(text(axis)->text(), "7123.456");
        EXPECT_EQ(text(field(editor, "pastDuration"))->text(), "37.5");
    }
    editor.set_create_mode(true);
    editor.resize(320, 480);
    settle();
    EXPECT_EQ(selector->currentIndex(), 0);
    EXPECT_FALSE(selector->isEnabled());
    EXPECT_FALSE(tabs->isEnabled());
    EXPECT_TRUE(editor.findChild<QPushButton *>("applyOrbit")->isVisible());
    editor.set_create_mode(false);
    EXPECT_TRUE(selector->isEnabled());

    GroundStationEditor station;
    station.set_station({"station", "Station", {1.2, 0.6, 350}}, true);
    station.findChild<QLineEdit *>("stationName")->setText(QStringLiteral("未应用的站点名称"));
    station.show();
    for (int width : {480, 320, 640, 320}) {
        station.resize(width, 520);
        settle();
        EXPECT_EQ(station.width(), width);
        auto *scroll = station.findChild<QScrollArea *>("groundStationScroll");
        EXPECT_EQ(scroll->horizontalScrollBar()->maximum(), 0);
        auto *submit = station.findChild<QPushButton *>("applyGroundStation");
        EXPECT_TRUE(station.rect().contains(submit->mapTo(&station, submit->rect().bottomRight())));
        EXPECT_EQ(station.findChild<QLineEdit *>("stationName")->text(),
                  QStringLiteral("未应用的站点名称"));
        if (!capture.isEmpty())
            EXPECT_TRUE(station.grab().save(capture + QString("/station-%1.png").arg(width)));
    }
}

TEST_F(QuantityTest, StationAndSensorUnitChangesPreserveExactAppliedValues) {
    const application::ProjectGroundStation station{
        "station", "", {1.2345678901234567, -0.4567890123456789, 123.45}, 0.12345678901234567};
    GroundStationEditor station_editor;
    station_editor.set_station(station, false);
    for (const auto *id : {"stationLongitude", "stationLatitude", "stationMinimumElevation"}) {
        auto *field = station_editor.findChild<QuantityEditor *>(id);
        field->set_unit(Unit::radian);
        field->set_unit(Unit::degree);
    }
    const auto draft = station_editor.station();
    EXPECT_EQ(draft.location, station.location);
    EXPECT_EQ(draft.minimum_elevation_rad, station.minimum_elevation_rad);
    EXPECT_EQ(draft.name(), station.name());

    application::Project project{"Scenario", core::Epoch{0}, core::Epoch{120'000'000'000}};
    application::ProjectSensor sensor;
    sensor.parent_satellite_id = "satellite";
    sensor.pattern.half_angle_rad = 0.567890123456789;
    SensorEditor sensor_editor;
    sensor_editor.set_sensor(sensor, project, false);
    auto *angle = sensor_editor.findChild<QuantityEditor *>("sensorHalfAngle");
    angle->set_unit(Unit::radian);
    angle->set_unit(Unit::degree);
    EXPECT_TRUE(same_sensor_input(sensor, sensor_editor.sensor()));
    angle->set_value(sensor.pattern.half_angle_rad + 1e-10, Unit::radian);
    EXPECT_FALSE(same_sensor_input(sensor, sensor_editor.sensor()));
}

TEST_F(QuantityTest, ObjectNamesAreEditableWithoutChangingIdentityOrOtherDrafts) {
    OrbitInput input;
    input.object_id = "fixed-satellite-id";
    OrbitEditor orbit(input);
    orbit.set_name("Original");
    orbit.set_create_mode(false);
    auto *name = orbit.findChild<QLineEdit *>("objectName");
    ASSERT_NE(name, nullptr);
    EXPECT_FALSE(name->isReadOnly());
    EXPECT_EQ(name->accessibleName(), QStringLiteral("对象名称"));
    name->setText(QStringLiteral("  观测卫星 A  "));
    EXPECT_TRUE(orbit.has_unapplied_changes());
    std::optional<OrbitInput> submitted;
    orbit.apply = [&](OrbitInput value) { submitted = value; };
    orbit.submit();
    ASSERT_TRUE(submitted);
    EXPECT_TRUE(same_orbit_input(*submitted, input));
    EXPECT_EQ(orbit.object_name(), "观测卫星 A");
    orbit.sync_name("External rename");
    EXPECT_EQ(orbit.object_name(), "观测卫星 A"); // Keep an independent name draft.
    orbit.findChild<QPushButton *>("resetOrbit")->click();
    EXPECT_EQ(orbit.object_name(), "External rename");
    EXPECT_FALSE(orbit.has_unapplied_changes());
    name->setText("   ");
    submitted.reset();
    orbit.submit();
    EXPECT_FALSE(submitted);
    EXPECT_TRUE(orbit.has_unapplied_changes());

    GroundStationEditor station;
    station.set_station({"fixed-station", "Station", {}}, false);
    station.findChild<QLineEdit *>("stationName")->setText(QStringLiteral("测控站"));
    EXPECT_EQ(station.station().object_id, "fixed-station");
    EXPECT_EQ(station.station().name(), "测控站");
    application::Project project{"Scene", core::Epoch{0}, core::Epoch{120'000'000'000}};
    application::ProjectSensor sensor;
    sensor.object_id = "fixed-sensor";
    sensor.parent_satellite_id = input.object_id;
    SensorEditor editor;
    editor.set_sensor(sensor, project, false);
    editor.findChild<QLineEdit *>("sensorName")->setText(QStringLiteral("相机 A"));
    EXPECT_EQ(editor.sensor().object_id, sensor.object_id);
    EXPECT_EQ(editor.sensor().parent_satellite_id, input.object_id);
    EXPECT_EQ(editor.sensor().name(), "相机 A");
}

TEST_F(QuantityTest, ReportRenameRefreshesHiddenIdentityColumnsWithoutChangingSamples) {
    application::Project project{"Scene", core::Epoch{0}, core::Epoch{60'000'000'000}};
    application::ProjectSatellite satellite;
    satellite.orbit.object_id = "sat";
    satellite.display_name = "New satellite";
    project.satellites.push_back(satellite);
    project.ground_stations.push_back({"station", "New station", {}});
    runtime::ReportResult report{
        .definition = {},
        .table = results::Table{
            {{"object.id", "ID", ""}, {"object.name", "Name", ""}, {"state.x", "X", "m"}},
            {{std::string("sat"), std::string("Old"), 123.25}}}};
    report.input_sha256 = "unchanged";
    report.definition.query.kind = application::ReportKind::cartesian_state;
    report.definition.format.time_scale = application::ReportTimeScale::tai;
    report.definition.format.columns = {{"object.name", {}, {}}, {"state.x", {}, {}}};
    report = report.with_template(report.definition.format);
    const auto renamed = renamed_report(report, project);
    ASSERT_TRUE(renamed);
    EXPECT_EQ(renamed->cell_text(0, 0), "New satellite");
    EXPECT_EQ(renamed->numeric_value(0, 1), report.numeric_value(0, 1));
    EXPECT_EQ(renamed->input_sha256, report.input_sha256);
    EXPECT_EQ(report.cell_text(0, 0), "Old");
    EXPECT_FALSE(renamed_report(*renamed, project));
    report.definition.query.kind = application::ReportKind::aer;
    report.definition.query.object_ids = {"station", "sat"};
    report.definition.format.columns = {{"object.name", {}, {}}};
    const auto pair = renamed_report(report, project);
    ASSERT_TRUE(pair);
    EXPECT_EQ(pair->cell_text(0, 0), "New station → New satellite");
}

TEST_F(QuantityTest, PropertyDraftsRemainDirtyUntilSuccessfullyPublished) {
    OrbitInput input;
    OrbitEditor orbit(input);
    EXPECT_FALSE(orbit.has_unapplied_changes());
    orbit.set_state_mode(0);
    auto *position = orbit.findChild<QuantityEditor *>("positionX");
    position->set_unit(Unit::metre);
    EXPECT_FALSE(orbit.has_unapplied_changes());
    position->set_value(input.initial.position_m[0] + 100, Unit::metre);
    EXPECT_TRUE(orbit.has_unapplied_changes());
    std::optional<OrbitInput> requested;
    orbit.apply = [&](OrbitInput value) { requested = value; };
    orbit.submit();
    ASSERT_TRUE(requested);
    EXPECT_TRUE(orbit.has_unapplied_changes());
    orbit.accept_input(*requested);
    EXPECT_FALSE(orbit.has_unapplied_changes());
    position->findChild<QLineEdit *>("quantityValue")->setText("unfinished");
    EXPECT_TRUE(orbit.has_unapplied_changes());
    orbit.findChild<QPushButton *>("resetOrbit")->click();
    EXPECT_FALSE(orbit.has_unapplied_changes());

    int requests = 0;
    orbit.appearance = [&](int, QString, double) {
        ++requests;
        throw std::runtime_error("Missing model");
    };
    orbit.configure_appearance(2, "missing.glb", 10);
    EXPECT_TRUE(orbit.has_unapplied_changes());
    orbit.findChild<QPushButton *>("applyAppearance")->click();
    EXPECT_EQ(requests, 2);
    orbit.accept_appearance(2, "missing.glb", 10);
    EXPECT_FALSE(orbit.has_unapplied_changes());
    orbit.findChild<QuantityEditor *>("modelDiameter")->set_value(20, Unit::kilometre);
    orbit.findChild<QPushButton *>("resetAppearance")->click();
    EXPECT_FALSE(orbit.has_unapplied_changes());

    GroundStationEditor station;
    station.set_station({"station", "", {1.2, 0.6, 100}}, true);
    EXPECT_FALSE(station.has_unapplied_changes());
    station.findChild<QuantityEditor *>("stationHeight")->set_value(200, Unit::metre);
    EXPECT_TRUE(station.has_unapplied_changes());
    station.sync_name("Renamed");
    EXPECT_EQ(station.station().name(), "Renamed");
    EXPECT_TRUE(station.has_unapplied_changes());
    station.accept_station(station.station());
    EXPECT_FALSE(station.has_unapplied_changes());
    EXPECT_EQ(station.findChild<QPushButton *>("applyGroundStation")->text(),
              QStringLiteral("应用"));

    application::Project project{"Scenario", core::Epoch{0}, core::Epoch{120'000'000'000}};
    application::ProjectSensor sensor;
    sensor.parent_satellite_id = "satellite";
    SensorEditor sensor_editor;
    sensor_editor.set_sensor(sensor, project, true);
    EXPECT_FALSE(sensor_editor.has_unapplied_changes());
    sensor_editor.findChild<QuantityEditor *>("sensorHalfAngle")->set_value(20, Unit::degree);
    EXPECT_TRUE(sensor_editor.has_unapplied_changes());
    sensor_editor.sync_name("New sensor name");
    EXPECT_EQ(sensor_editor.sensor().name(), "New sensor name");
    sensor_editor.accept_sensor(sensor_editor.sensor());
    EXPECT_FALSE(sensor_editor.has_unapplied_changes());
    EXPECT_EQ(sensor_editor.findChild<QPushButton *>("applySensor")->text(),
              QStringLiteral("应用"));
    sensor_editor.findChild<QuantityEditor *>("sensorHalfAngle")
        ->findChild<QLineEdit *>("quantityValue")
        ->setText("unfinished");
    EXPECT_TRUE(sensor_editor.has_unapplied_changes());
    sensor_editor.findChild<QPushButton *>("resetSensor")->click();
    EXPECT_FALSE(sensor_editor.has_unapplied_changes());
}

TEST_F(QuantityTest, GroundStationResetUsesCreationStateInsteadOfButtonCopy) {
    GroundStationEditor editor;
    application::ProjectGroundStation station;
    station.object_id = "draft-station";
    editor.set_station(station, true);
    auto *apply = editor.findChild<QPushButton *>("applyGroundStation");
    auto *reset = editor.findChild<QPushButton *>("resetGroundStation");
    ASSERT_NE(apply, nullptr);
    ASSERT_NE(reset, nullptr);
    apply->setText("Translated create action");
    reset->click();
    EXPECT_EQ(apply->text(), QStringLiteral("插入地面站"));
    editor.accept_station(station);
    apply->setText("Translated apply action");
    reset->click();
    EXPECT_EQ(apply->text(), QStringLiteral("应用"));
}
} // namespace orbelle::desktop
