#include "../src/viewer/epoch_editor.hpp"
#include "../src/viewer/ground_station_editor.hpp"
#include "../src/viewer/hpop_settings.hpp"
#include "../src/viewer/mission_editor.hpp"
#include "../src/viewer/object_selection.hpp"
#include "../src/viewer/orbit_editor.hpp"
#include "../src/viewer/quantity_editor.hpp"
#include "../src/viewer/sensor_editor.hpp"
#include "../src/viewer/workbench_style.hpp"
#include "window_show_observer.hpp"
#include <QApplication>
#include <QCheckBox>
#include <QColorDialog>
#include <QComboBox>
#include <QDialog>
#include <QDialogButtonBox>
#include <QDir>
#include <QDoubleSpinBox>
#include <QEventLoop>
#include <QKeyEvent>
#include <QLabel>
#include <QLineEdit>
#include <QListWidget>
#include <QMenu>
#include <QPlainTextEdit>
#include <QPushButton>
#include <QScrollArea>
#include <QScrollBar>
#include <QSpinBox>
#include <QTabBar>
#include <QTabWidget>
#include <QTimer>
#include <QToolButton>
#include <QTreeWidget>
#include <cmath>
#include <gtest/gtest.h>
#include <numbers>
#include <orbitforge/runtime/j2.hpp>
#include <orbitforge/runtime/j4.hpp>
#include <orbitforge/runtime/time_frames.hpp>
#include <orbitforge/runtime/two_body.hpp>

namespace orbitforge::desktop {
class QuantityTest : public ::testing::Test {
  protected:
    void SetUp() override {
        static int argc = 1;
        static char executable[] = "quantity_test";
        static char *argv[] = {executable, nullptr};
        static QApplication app(argc, argv);
    }
    static QLineEdit *text(QuantityEditor &editor) {
        return editor.findChild<QLineEdit *>("quantityValue");
    }
    static void choose(QuantityEditor &editor, Unit unit) {
        auto *menu = editor.findChild<QToolButton *>("quantityUnit")->menu();
        for (auto *action : menu->actions())
            if (action->data().toInt() == static_cast<int>(unit)) {
                action->trigger();
                return;
            }
        FAIL() << "Requested unit was not offered";
    }
    static QuantityEditor &field(OrbitEditor &editor, const char *name) {
        auto *result = editor.findChild<QuantityEditor *>(name);
        if (!result)
            throw std::runtime_error("Missing quantity field");
        return *result;
    }
};
TEST_F(QuantityTest, ConstructingFieldsDoesNotShowTemporaryWindows) {
    WindowShowObserver windows;
    for (const auto unit : {Unit::one, Unit::kilometre, Unit::kilometres_per_second, Unit::degree,
                            Unit::second, Unit::kilometres_cubed_per_second_squared}) {
        QuantityEditor editor(unit);
        EXPECT_FALSE(editor.isVisible());
        QCoreApplication::processEvents();
    }
    EXPECT_TRUE(windows.shown.isEmpty()) << windows.shown.join(", ").toStdString();
}
TEST_F(QuantityTest, PreparingOrbitPropertiesDoesNotShowTemporaryWindows) {
    WindowShowObserver windows;
    OrbitEditor editor(OrbitInput{});
    QCoreApplication::processEvents();
    EXPECT_FALSE(editor.isVisible());
    EXPECT_TRUE(windows.shown.isEmpty()) << windows.shown.join(", ").toStdString();
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
    auto *navigation = editor.findChild<QTreeWidget *>("propertyNavigation");
    auto *state = editor.findChild<QComboBox *>("stateRepresentation");
    ASSERT_TRUE(selector && navigation && state);
    state->setCurrentIndex(1);
    auto &axis = field(editor, "semiMajorAxis");
    text(axis)->setText("7123.456");
    editor.findChild<QComboBox *>("trackExtent")->setCurrentIndex(2);
    text(field(editor, "pastDuration"))->setText("37.5");
    const auto settle = [] {
        for (int i = 0; i < 12; ++i)
            QCoreApplication::processEvents();
    };
    const auto capture = qEnvironmentVariable("ORBITFORGE_RESPONSIVE_UI_CAPTURE");
    if (!capture.isEmpty())
        ASSERT_TRUE(QDir().mkpath(capture));
    for (int width : {1120, 320, 560, 840, 320, 1120}) {
        const int page = selector->currentIndex();
        editor.resize(width, 620);
        settle();
        EXPECT_EQ(editor.width(), width);
        EXPECT_EQ(selector->currentIndex(), page);
        EXPECT_EQ(selector->isVisible(), width < 680);
        EXPECT_EQ(navigation->isVisible(), width >= 680);
        for (int index = 0; index < 4; ++index) {
            selector->setCurrentIndex(index);
            settle();
            EXPECT_EQ(navigation->currentItem()->data(0, Qt::UserRole).toInt(), index);
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
TEST_F(QuantityTest, PreparingSensorDoesNotShowTemporaryWindows) {
    WindowShowObserver windows;
    application::Project project("Scenario", core::Epoch{0}, core::Epoch{120'000'000'000});
    project.ground_stations.push_back({"station", "Station", {1.2, 0.6, 350}});
    application::ProjectSensor sensor;
    sensor.object_id = "sensor";
    sensor.parent_satellite_id = "satellite";
    sensor.half_angle_rad = 0.2;
    SensorEditor editor;
    editor.set_sensor(sensor, project, true);
    sensor.pointing = application::SensorPointing::targeted;
    sensor.target_station_id = "station";
    editor.set_sensor(sensor, project, false);
    QCoreApplication::processEvents();
    EXPECT_FALSE(editor.isVisible());
    EXPECT_TRUE(windows.shown.isEmpty()) << windows.shown.join(", ").toStdString();
}
TEST_F(QuantityTest, SensorPointingChoicesKeepTargetDraftAndSubmitStableIds) {
    application::Project project("Scenario", core::Epoch{0}, core::Epoch{120'000'000'000});
    project.ground_stations.push_back({"station-a", "Same name", {1.2, 0.6, 350}});
    project.ground_stations.push_back({"station-b", "Same name", {1.3, 0.7, 100}});
    application::ProjectSensor sensor;
    sensor.object_id = "sensor";
    sensor.parent_satellite_id = "satellite";
    SensorEditor editor;
    editor.set_sensor(sensor, project, true);
    auto *name = editor.findChild<QLineEdit *>("sensorName");
    auto *angle = editor.findChild<QuantityEditor *>("sensorHalfAngle");
    auto *nadir = editor.findChild<QPushButton *>("sensorNadir");
    auto *targeted = editor.findChild<QPushButton *>("sensorTargeted");
    auto *target = editor.findChild<ObjectSelectionField *>("sensorTarget");
    auto *row = editor.findChild<QWidget *>("sensorTargetRow");
    auto *submit = editor.findChild<QPushButton *>("applySensor");
    ASSERT_TRUE(name && angle && nadir && targeted && target && row && submit);
    EXPECT_TRUE(nadir->isChecked());
    EXPECT_TRUE(row->isHidden());
    name->setText(QStringLiteral("  对地观测仪  "));
    text(*angle)->setText("30");
    choose(*angle, Unit::radian);
    // The two stations have identical display names; binding must use the stable ID.
    targeted->click();
    QTimer::singleShot(0, [] {
        auto *chooser = qobject_cast<ObjectSelectionDialog *>(QApplication::activeModalWidget());
        ASSERT_TRUE(chooser);
        chooser->findChild<QLineEdit *>("objectSelectionSearch")->setText("station-b");
        auto *objects = chooser->findChild<QTreeWidget *>("objectSelectionTree");
        ASSERT_EQ(objects->topLevelItemCount(), 2);
        objects->setCurrentItem(objects->topLevelItem(1));
        chooser->findChild<QDialogButtonBox *>()->button(QDialogButtonBox::Ok)->click();
    });
    target->click();
    EXPECT_FALSE(nadir->isChecked());
    EXPECT_FALSE(row->isHidden());
    EXPECT_EQ(editor.sensor().target_station_id, "station-b");
    nadir->click();
    EXPECT_TRUE(row->isHidden());
    EXPECT_TRUE(editor.sensor().target_station_id.empty());
    targeted->click();
    EXPECT_EQ(target->id(), "station-b");
    std::optional<application::ProjectSensor> applied;
    editor.apply = [&](application::ProjectSensor value) { applied = std::move(value); };
    submit->click();
    ASSERT_TRUE(applied);
    EXPECT_EQ(applied->object_id, sensor.object_id);
    EXPECT_EQ(applied->parent_satellite_id, sensor.parent_satellite_id);
    EXPECT_EQ(applied->display_name, "对地观测仪");
    EXPECT_EQ(applied->pointing, application::SensorPointing::targeted);
    EXPECT_EQ(applied->target_station_id, "station-b");
    EXPECT_NEAR(applied->half_angle_rad, std::numbers::pi / 6, 1e-14);
    editor.set_sensor(*applied, project, false);
    EXPECT_EQ(angle->unit(), Unit::radian);
    EXPECT_EQ(target->id(), "station-b");
    EXPECT_EQ(submit->text(), QStringLiteral("应用更改"));
    applied.reset();
    name->setText("Unsubmitted draft");
    editor.findChild<QPushButton *>("closeSensor")->click();
    EXPECT_FALSE(applied);
    editor.set_sensor(sensor, project, true);
    EXPECT_EQ(name->text(), "sensor");
    EXPECT_TRUE(nadir->isChecked());
    EXPECT_EQ(submit->text(), QStringLiteral("插入传感器"));
}
TEST_F(QuantityTest, SensorRejectsInvalidDraftAndNeverRebindsMissingTarget) {
    application::Project project("Scenario", core::Epoch{0}, core::Epoch{120'000'000'000});
    application::ProjectSensor sensor;
    sensor.object_id = "sensor";
    sensor.parent_satellite_id = "satellite";
    sensor.pointing = application::SensorPointing::targeted;
    sensor.target_station_id = "missing-station";
    SensorEditor editor;
    editor.set_sensor(sensor, project, false);
    auto *target = editor.findChild<ObjectSelectionField *>("sensorTarget");
    auto *submit = editor.findChild<QPushButton *>("applySensor");
    auto *error = editor.findChild<QLabel *>("sensorError");
    ASSERT_TRUE(target && submit && error);
    int applications = 0;
    editor.apply = [&](application::ProjectSensor) { ++applications; };
    EXPECT_TRUE(target->objects().empty());
    submit->click();
    EXPECT_EQ(applications, 0);
    EXPECT_FALSE(error->isHidden());
    EXPECT_TRUE(error->text().contains(QStringLiteral("没有地面站")));
    project.ground_stations.push_back({"other-station", "Station", {1.2, 0.6, 350}});
    editor.set_sensor(sensor, project, false);
    EXPECT_TRUE(error->isHidden());
    EXPECT_TRUE(target->isEnabled());
    EXPECT_EQ(target->id(), "missing-station");
    EXPECT_FALSE(target->contains(target->id()));
    submit->click();
    EXPECT_EQ(applications, 0);
    target->set_id("other-station");
    auto *angle = editor.findChild<QuantityEditor *>("sensorHalfAngle");
    ASSERT_TRUE(angle);
    text(*angle)->setText("91");
    submit->click();
    EXPECT_EQ(applications, 0);
    text(*angle)->setText("30");
    auto *name = editor.findChild<QLineEdit *>("sensorName");
    ASSERT_TRUE(name);
    name->setText(" ");
    submit->click();
    EXPECT_EQ(applications, 0);
    EXPECT_EQ(error->text(), QStringLiteral("请输入传感器名称。"));
    name->setText("Sensor");
    submit->click();
    EXPECT_EQ(applications, 1);
    EXPECT_TRUE(error->isHidden());
}
TEST_F(QuantityTest, SensorLayoutFitsNarrowPanelsAndKeepsActionsVisible) {
    apply_workbench_style(*qobject_cast<QApplication *>(QApplication::instance()));
    application::Project project("Scenario", core::Epoch{0}, core::Epoch{120'000'000'000});
    application::ProjectSatellite satellite;
    satellite.orbit.object_id = "satellite";
    satellite.display_name = "Aurora-01";
    project.satellites.push_back(satellite);
    project.ground_stations.push_back({"station", "Ground Station", {1.2, 0.6, 350}});
    application::ProjectSensor sensor;
    sensor.object_id = "sensor";
    sensor.display_name = "对地观测仪";
    sensor.parent_satellite_id = "satellite";
    sensor.half_angle_rad = std::numbers::pi / 6;
    SensorEditor editor;
    editor.set_sensor(sensor, project, true);
    auto *scroll = editor.findChild<QScrollArea *>("sensorScroll");
    auto *targeted = editor.findChild<QPushButton *>("sensorTargeted");
    auto *submit = editor.findChild<QPushButton *>("applySensor");
    auto *angle = editor.findChild<QuantityEditor *>("sensorHalfAngle");
    auto *full_angle = editor.findChild<QLabel *>("sensorFullAngle");
    ASSERT_TRUE(scroll && targeted && submit && angle && full_angle);
    const auto capture = qEnvironmentVariable("ORBITFORGE_SENSOR_UI_CAPTURE");
    if (!capture.isEmpty())
        ASSERT_TRUE(QDir{}.mkpath(capture));
    const auto screenshot = [&](const QString &name) {
        QApplication::processEvents();
        if (!capture.isEmpty())
            EXPECT_TRUE(editor.grab().save(capture + "/" + name + ".png"));
    };
    editor.resize(480, 640);
    editor.show();
    screenshot("sensor-insert");
    EXPECT_EQ(full_angle->text(), QStringLiteral("全视场 60°"));
    targeted->setFocus();
    QKeyEvent press(QEvent::KeyPress, Qt::Key_Space, Qt::NoModifier);
    QKeyEvent release(QEvent::KeyRelease, Qt::Key_Space, Qt::NoModifier);
    QApplication::sendEvent(targeted, &press);
    QApplication::sendEvent(targeted, &release);
    EXPECT_TRUE(targeted->isChecked());
    EXPECT_EQ(editor.sensor().target_station_id, "station");
    screenshot("sensor-targeted");
    // Long user names must not force the workspace pane wider.
    project.satellites[0].display_name = std::string(200, 'A');
    project.ground_stations[0].display_name = std::string(200, 'S');
    sensor.pointing = application::SensorPointing::targeted;
    sensor.target_station_id = "station";
    editor.set_sensor(sensor, project, false);
    for (const int width : {320, 480, 640}) {
        editor.resize(width, 400);
        QApplication::processEvents();
        EXPECT_EQ(editor.width(), width);
        EXPECT_EQ(scroll->horizontalScrollBar()->maximum(), 0);
        EXPECT_FALSE(scroll->horizontalScrollBar()->isVisible());
        EXPECT_LE(scroll->widget()->width(), scroll->viewport()->width());
        scroll->ensureWidgetVisible(editor.findChild<ObjectSelectionField *>("sensorTarget"));
        QApplication::processEvents();
        const QRect action_rect(submit->mapTo(&editor, QPoint{}), submit->size());
        EXPECT_TRUE(editor.rect().contains(action_rect));
        EXPECT_FALSE(scroll->isAncestorOf(submit));
        screenshot(QStringLiteral("sensor-narrow-%1").arg(width));
    }
    editor.resize(360, 640);
    text(*angle)->setText("invalid");
    QApplication::processEvents();
    EXPECT_EQ(full_angle->text(), QStringLiteral("半角范围 0.001–90°"));
    submit->click();
    screenshot("sensor-invalid");
    project.ground_stations.clear();
    editor.set_sensor(sensor, project, false);
    submit->click();
    screenshot("sensor-no-stations");
    EXPECT_EQ(scroll->horizontalScrollBar()->maximum(), 0);
}
TEST_F(QuantityTest, PreparingEpochAndValidationDoesNotShowTemporaryWindows) {
    WindowShowObserver windows;
    EpochEditor editor;
    editor.set_read_only(true);
    editor.set_read_only(false);
    editor.findChild<QLineEdit *>("epochValue")->setText("invalid epoch");
    editor.set_scale(CalendarScale::tt);
    QCoreApplication::processEvents();
    EXPECT_FALSE(editor.isVisible());
    EXPECT_TRUE(windows.shown.isEmpty()) << windows.shown.join(", ").toStdString();
}
TEST_F(QuantityTest, ConvertsCompatibleDimensionsAndRejectsInvalidConversions) {
    EXPECT_DOUBLE_EQ(convert_quantity(7000, Unit::kilometre, Unit::metre), 7e6);
    EXPECT_DOUBLE_EQ(convert_quantity(7.5, Unit::kilometres_per_second, Unit::metres_per_second),
                     7500);
    EXPECT_DOUBLE_EQ(convert_quantity(180, Unit::degree, Unit::radian), std::numbers::pi);
    EXPECT_DOUBLE_EQ(convert_quantity(2, Unit::day, Unit::hour), 48);
    EXPECT_DOUBLE_EQ(convert_quantity(0.75, Unit::minute, Unit::millisecond), 45000);
    EXPECT_NEAR(convert_quantity(398600.4418, Unit::kilometres_cubed_per_second_squared,
                                 Unit::metres_cubed_per_second_squared),
                3.986004418e14, 0.1);
    EXPECT_THROW(convert_quantity(1, Unit::metre, Unit::second), std::invalid_argument);
    EXPECT_THROW(convert_quantity(1, static_cast<Unit>(999), Unit::second), std::invalid_argument);
    EXPECT_THROW(
        convert_quantity(std::numeric_limits<double>::infinity(), Unit::degree, Unit::radian),
        std::invalid_argument);
    EXPECT_THROW(convert_quantity(1e308, Unit::kilometre, Unit::metre), std::out_of_range);
    EXPECT_THROW(
        convert_quantity(std::numeric_limits<double>::denorm_min(), Unit::metre, Unit::kilometre),
        std::out_of_range);
}
TEST_F(QuantityTest, MenuConvertsDraftAndGetterDoesNotDependOnDisplayUnit) {
    QuantityEditor editor(Unit::degree);
    text(editor)->setText("180");
    choose(editor, Unit::radian);
    EXPECT_EQ(editor.unit(), Unit::radian);
    EXPECT_TRUE(text(editor)->text().startsWith("3.14159265358979"));
    EXPECT_DOUBLE_EQ(editor.value(Unit::radian), std::numbers::pi);
    EXPECT_DOUBLE_EQ(editor.value(Unit::degree), 180);
    text(editor)->setText("0.5");
    choose(editor, Unit::degree);
    EXPECT_DOUBLE_EQ(editor.value(Unit::radian), 0.5);
    EXPECT_NEAR(editor.value(Unit::degree), 28.64788975654116, 1e-12);
}
TEST_F(QuantityTest, RepeatedUnitChangesKeepFullPrecisionAndDoNotChangePhysicalValue) {
    QuantityEditor editor(Unit::radian);
    const double original = 1.2345678901234567;
    editor.set_value(original, Unit::radian);
    int values = 0, units = 0;
    QObject::connect(&editor, &QuantityEditor::valueChanged, [&values] { ++values; });
    QObject::connect(&editor, &QuantityEditor::unitChanged, [&units] { ++units; });
    for (int i = 0; i < 100; ++i) {
        editor.set_unit(Unit::degree);
        EXPECT_DOUBLE_EQ(editor.value(Unit::radian), original);
        editor.set_unit(Unit::radian);
    }
    EXPECT_EQ(values, 0);
    EXPECT_EQ(units, 200);
    editor.set_value(2, Unit::radian);
    EXPECT_EQ(values, 1);
}
TEST_F(QuantityTest, InvalidDraftIsNotDiscardedAndCannotChangeUnitOrBeRead) {
    QuantityEditor editor(Unit::degree);
    editor.set_value(45, Unit::degree);
    for (const QString input : {QString{}, QString("-"), QString("1e"), QString("nan"),
                                QString("inf"), QString("1,000"), QString("1e400")}) {
        text(editor)->setText(input);
        EXPECT_THROW(editor.set_unit(Unit::radian), std::exception);
        EXPECT_THROW((void)editor.value(Unit::radian), std::exception);
        EXPECT_EQ(editor.unit(), Unit::degree);
        EXPECT_EQ(text(editor)->text(), input);
        EXPECT_TRUE(editor.property("invalid").toBool());
    }
    text(editor)->setText("9e1");
    EXPECT_FALSE(editor.property("invalid").toBool());
    choose(editor, Unit::radian);
    EXPECT_DOUBLE_EQ(editor.value(Unit::degree), 90);
}
TEST_F(QuantityTest, ScientificInputAndMixedUnitSetterPreserveSignedValues) {
    QuantityEditor editor(Unit::kilometre);
    text(editor)->setText("-2.5e-3");
    EXPECT_DOUBLE_EQ(editor.value(Unit::metre), -2.5);
    editor.set_unit(Unit::metre);
    EXPECT_EQ(text(editor)->text(), "-2.5");
    editor.set_value(2.25, Unit::kilometre);
    EXPECT_EQ(text(editor)->text(), "2250");
    EXPECT_THROW(editor.set_unit(Unit::radian), std::invalid_argument);
    EXPECT_EQ(editor.unit(), Unit::metre);
    EXPECT_DOUBLE_EQ(editor.value(Unit::metre), 2250);
    EXPECT_THROW((void)editor.value(Unit::second), std::invalid_argument);
}
TEST_F(QuantityTest, PhysicalRangeAndWholeSecondResolutionSurviveUnitChanges) {
    QuantityEditor editor(Unit::second);
    editor.set_range(1, 60, Unit::second);
    editor.set_quantum(1, Unit::second);
    editor.set_value(60, Unit::second);
    choose(editor, Unit::minute);
    EXPECT_EQ(text(editor)->text(), "1");
    text(editor)->setText("0.75");
    EXPECT_DOUBLE_EQ(editor.value(Unit::second), 45);
    text(editor)->setText("0.751");
    EXPECT_THROW((void)editor.value(Unit::second), std::out_of_range);
    EXPECT_THROW(editor.set_unit(Unit::second), std::out_of_range);
    EXPECT_EQ(text(editor)->text(), "0.751");
    editor.set_value(1, Unit::second);
    editor.set_unit(Unit::day);
    const auto rendered = text(editor)->text();
    text(editor)->setText({});
    text(editor)->setText(rendered);
    EXPECT_DOUBLE_EQ(editor.value(Unit::second), 1);
    EXPECT_THROW(editor.set_value(61, Unit::second), std::out_of_range);
    EXPECT_DOUBLE_EQ(editor.value(Unit::second), 1);
}
TEST_F(QuantityTest, KeyboardStepUsesPhysicalIncrementAndDimensionlessHasNoUnitMenu) {
    QuantityEditor editor(Unit::degree);
    editor.set_range(0, 180, Unit::degree);
    editor.set_single_step(1, Unit::degree);
    editor.set_value(90, Unit::degree);
    editor.set_unit(Unit::radian);
    QKeyEvent up(QEvent::KeyPress, Qt::Key_Up, Qt::NoModifier);
    QCoreApplication::sendEvent(text(editor), &up);
    EXPECT_NEAR(editor.value(Unit::degree), 91, 1e-12);
    QuantityEditor eccentricity(Unit::one);
    EXPECT_TRUE(eccentricity.findChild<QToolButton *>("quantityUnit")->isHidden());
    eccentricity.set_value(0.1, Unit::one);
    EXPECT_THROW(eccentricity.set_unit(Unit::degree), std::invalid_argument);
}
TEST_F(QuantityTest, OrbitSubmissionReadsMixedUnitsAsSIAndKeepsUnitPreferences) {
    OrbitInput input;
    OrbitEditor editor(input);
    editor.set_state_mode(0);
    auto &position = field(editor, "positionX"), &velocity = field(editor, "velocityY");
    position.set_unit(Unit::metre);
    velocity.set_unit(Unit::metres_per_second);
    text(position)->setText("7.2e6");
    text(velocity)->setText("6300");
    EXPECT_EQ(editor.findChild<QuantityEditor *>("gravitationalParameter"), nullptr);
    auto &step = field(editor, "displayStep");
    step.set_unit(Unit::minute);
    text(step)->setText("0.75");
    std::optional<OrbitInput> submitted;
    editor.apply = [&](OrbitInput result) { submitted = std::move(result); };
    editor.submit();
    ASSERT_TRUE(submitted);
    EXPECT_DOUBLE_EQ(submitted->initial.position_m[0], 7.2e6);
    EXPECT_DOUBLE_EQ(submitted->initial.velocity_mps[1], 6300);
    EXPECT_DOUBLE_EQ(submitted->mu(), input.mu());
    EXPECT_EQ(submitted->step_seconds, 45);
    editor.set_input(*submitted);
    EXPECT_EQ(position.unit(), Unit::metre);
    EXPECT_EQ(velocity.unit(), Unit::metres_per_second);
    EXPECT_EQ(step.unit(), Unit::minute);
    submitted.reset();
    text(step)->setText("0.751");
    editor.submit();
    EXPECT_FALSE(submitted);
}
TEST_F(QuantityTest, KeplerianSubmissionUsesRadiansWithoutDoubleConversion) {
    OrbitInput input;
    OrbitEditor editor(input);
    const std::array<const char *, 6> names{"semiMajorAxis", "eccentricity",        "inclination",
                                            "raan",          "argumentOfPeriapsis", "trueAnomaly"};
    const std::array values{7.8e6, 0.1, 0.6, 1.3, 2.1, -0.2};
    for (std::size_t i = 0; i < names.size(); ++i) {
        const auto unit = i == 0 ? Unit::metre : i == 1 ? Unit::one : Unit::radian;
        auto &quantity = field(editor, names[i]);
        quantity.set_unit(unit);
        quantity.set_value(values[i], unit);
    }
    const auto expected =
        runtime::to_cartesian({input.initial.epoch, 7.8e6, 0.1, 0.6, 1.3, 2.1, -0.2},
                              core::GravitationalParameter{input.mu()});
    std::optional<OrbitInput> submitted;
    editor.apply = [&](OrbitInput result) { submitted = std::move(result); };
    editor.submit();
    ASSERT_TRUE(submitted);
    for (int i = 0; i < 3; ++i) {
        EXPECT_NEAR(submitted->initial.position_m[i], expected.position_m[i], 1e-7);
        EXPECT_NEAR(submitted->initial.velocity_mps[i], expected.velocity_mps[i], 1e-10);
    }
}
TEST_F(QuantityTest, DisplayDurationAndModelDiameterKeepExistingStorageUnits) {
    OrbitEditor editor(OrbitInput{});
    editor.findChild<QComboBox *>("trackExtent")->setCurrentIndex(2);
    auto &past = field(editor, "pastDuration");
    past.set_unit(Unit::hour);
    text(past)->setText("0.5");
    auto &future = field(editor, "futureDuration");
    future.set_unit(Unit::second);
    text(future)->setText("900");
    std::optional<DisplaySettings> settings;
    editor.display = [&](DisplaySettings result) { settings = result; };
    editor.findChild<QPushButton *>("applyDisplay")->click();
    ASSERT_TRUE(settings);
    EXPECT_DOUBLE_EQ(settings->past_minutes, 30);
    EXPECT_DOUBLE_EQ(settings->future_minutes, 15);
    editor.set_appearance(1, {}, 12.5);
    auto &diameter = field(editor, "modelDiameter");
    diameter.set_unit(Unit::metre);
    EXPECT_DOUBLE_EQ(diameter.value(Unit::metre), 12500);
    text(diameter)->setText("17000");
    double applied = 0;
    editor.appearance = [&](int, QString, double km) { applied = km; };
    for (auto *button : editor.findChildren<QPushButton *>())
        if (button->text() == QStringLiteral("应用外观"))
            button->click();
    EXPECT_DOUBLE_EQ(applied, 17);
}
TEST_F(QuantityTest, TrackStylePreviewApplyCustomColorAndObjectSwitchStayIndependent) {
    apply_workbench_style(*qobject_cast<QApplication *>(QApplication::instance()));
    OrbitEditor editor(OrbitInput{});
    editor.resize(980, 660);
    auto *navigation = editor.findChild<QTreeWidget *>("propertyNavigation");
    navigation->setCurrentItem(navigation->topLevelItem(0)->child(1));
    DisplaySettings existing;
    existing.line_color = 0x123456;
    existing.line_width = 2.5;
    existing.line_style = application::LineStyle::dash_dot;
    existing.extent = 2;
    existing.past_minutes = 12;
    existing.track_3d = false;
    editor.set_display(existing);
    editor.show();
    QApplication::processEvents();
    auto *style = editor.findChild<QComboBox *>("trackLineStyle");
    auto *width = editor.findChild<QDoubleSpinBox *>("trackLineWidth");
    auto *custom = editor.findChild<QPushButton *>("trackCustomColor");
    ASSERT_TRUE(style && width && custom);
    EXPECT_EQ(style->currentIndex(), 3);
    EXPECT_DOUBLE_EQ(width->value(), 2.5);
    EXPECT_EQ(width->buttonSymbols(), QAbstractSpinBox::UpDownArrows);
    EXPECT_TRUE(custom->text().contains("#123456"));
    std::vector<DisplaySettings> applied;
    editor.display = [&](DisplaySettings result) { applied.push_back(result); };
    const auto before = editor.findChild<QWidget *>("trackStylePreview")->grab().toImage();
    editor.findChild<QToolButton *>("trackColor2")->click();
    style->setCurrentIndex(1);
    width->setValue(4);
    const auto after = editor.findChild<QWidget *>("trackStylePreview")->grab().toImage();
    EXPECT_NE(before, after);
    EXPECT_TRUE(applied.empty());
    editor.findChild<QPushButton *>("applyDisplay")->click();
    ASSERT_EQ(applied.size(), 1);
    EXPECT_EQ(applied[0].line_color, 0xe9a2ba);
    EXPECT_EQ(applied[0].line_style, application::LineStyle::dashed);
    EXPECT_EQ(applied[0].line_width, 4);
    EXPECT_EQ(applied[0].past_minutes, 12);
    EXPECT_FALSE(applied[0].track_3d);
    const auto capture = qEnvironmentVariable("ORBITFORGE_STYLE_UI_CAPTURE");
    if (!capture.isEmpty()) {
        ASSERT_TRUE(QDir{}.mkpath(capture));
        ASSERT_TRUE(editor.grab().save(capture + "/track-style.png"));
        editor.resize(910, 400);
        QApplication::processEvents();
        ASSERT_TRUE(editor.grab().save(capture + "/track-style-compact.png"));
    }
    QTimer::singleShot(0, [] {
        auto *dialog = qobject_cast<QColorDialog *>(QApplication::activeModalWidget());
        ASSERT_TRUE(dialog);
        dialog->setCurrentColor(QColor("#abcdef"));
        dialog->reject();
    });
    custom->click();
    EXPECT_TRUE(custom->text().contains("#E9A2BA"));
    QTimer::singleShot(0, [] {
        auto *dialog = qobject_cast<QColorDialog *>(QApplication::activeModalWidget());
        ASSERT_TRUE(dialog);
        dialog->setCurrentColor(QColor("#abcdef"));
        dialog->accept();
    });
    custom->click();
    editor.findChild<QPushButton *>("applyDisplay")->click();
    EXPECT_EQ(applied.back().line_color, 0xabcdef);
    editor.set_display(existing);
    editor.findChild<QPushButton *>("applyDisplay")->click();
    EXPECT_EQ(applied.back(), existing);
}
TEST_F(QuantityTest, FrameSwitchAndItrfInputUseFullPositionVelocityTransform) {
    const auto frames = runtime::make_frame_provider();
    const auto eop = runtime::load_eop_snapshot(ORBITFORGE_TEST_EOP);
    OrbitInput input;
    input.initial.epoch =
        runtime::make_builtin_time_provider()->from_utc({2026, 10, 9, 0, 0, 0, 0});
    OrbitEditor editor(input);
    editor.transform_state = [&](core::CartesianState state, core::FrameId frame) {
        const auto transform = frames->gcrf_to_itrf_state(state.epoch, eop->at(state.epoch));
        return frame == core::FrameId::itrf ? transform.apply(state) : transform.inverse(state);
    };
    editor.findChild<QComboBox *>("initialReferenceFrame")->setCurrentIndex(1);
    EXPECT_EQ(editor.findChild<QComboBox *>("stateRepresentation")->currentIndex(), 0);
    const auto transform =
        frames->gcrf_to_itrf_state(input.initial.epoch, eop->at(input.initial.epoch));
    auto expected_fixed = transform.apply(input.initial);
    EXPECT_NEAR(field(editor, "positionX").value(Unit::metre), expected_fixed.position_m[0], 1e-6);
    EXPECT_NEAR(field(editor, "velocityY").value(Unit::metres_per_second),
                expected_fixed.velocity_mps[1], 1e-8);
    // Editing fixed-frame velocity must include the Earth rotation term on submission.
    field(editor, "velocityY").set_value(1000, Unit::metres_per_second);
    expected_fixed.velocity_mps[1] = 1000;
    std::optional<OrbitInput> submitted;
    editor.apply = [&](OrbitInput result) { submitted = result; };
    editor.submit();
    ASSERT_TRUE(submitted);
    const auto expected = transform.inverse(expected_fixed);
    EXPECT_EQ(submitted->input_frame, core::FrameId::itrf);
    for (std::size_t k = 0; k < 3; ++k) {
        EXPECT_NEAR(submitted->initial.position_m[k], expected.position_m[k], 1e-6);
        EXPECT_NEAR(submitted->initial.velocity_mps[k], expected.velocity_mps[k], 1e-8);
    }
    editor.set_input(*submitted);
    EXPECT_EQ(editor.findChild<QComboBox *>("initialReferenceFrame")->currentIndex(), 1);
    editor.set_state_mode(1);
    EXPECT_EQ(editor.findChild<QComboBox *>("initialReferenceFrame")->currentIndex(), 0);
    submitted.reset();
    editor.submit();
    ASSERT_TRUE(submitted);
    for (std::size_t k = 0; k < 3; ++k)
        EXPECT_NEAR(submitted->initial.position_m[k], expected.position_m[k], .001);
}
TEST_F(QuantityTest, FailedFrameSwitchRetainsTheOriginalDraft) {
    OrbitEditor editor(OrbitInput{});
    const double original = field(editor, "semiMajorAxis").value(Unit::metre);
    editor.transform_state = [](core::CartesianState, core::FrameId) -> core::CartesianState {
        throw std::out_of_range("No EOP coverage");
    };
    editor.findChild<QComboBox *>("initialReferenceFrame")->setCurrentIndex(1);
    EXPECT_EQ(editor.findChild<QComboBox *>("initialReferenceFrame")->currentIndex(), 0);
    EXPECT_EQ(editor.findChild<QComboBox *>("stateRepresentation")->currentIndex(), 1);
    EXPECT_DOUBLE_EQ(field(editor, "semiMajorAxis").value(Unit::metre), original);
}
TEST_F(QuantityTest, J2MeanAnomalyAndReadOnlyModelDetailsPreserveDraft) {
    OrbitEditor editor(OrbitInput{});
    editor.findChild<QComboBox *>("orbitPropagator")->setCurrentIndex(1);
    field(editor, "meanAnomaly").set_value(90, Unit::degree);
    auto &step = field(editor, "displayStep");
    step.set_unit(Unit::minute);
    step.set_value(30, Unit::second);
    int submissions = 0;
    std::optional<OrbitInput> submitted;
    editor.apply = [&](OrbitInput result) {
        ++submissions;
        submitted = result;
    };
    QTimer::singleShot(0, &editor, [&] {
        auto *dialog = editor.findChild<QDialog *>("propagatorDetailsDialog");
        ASSERT_NE(dialog, nullptr);
        EXPECT_TRUE(dialog->findChildren<QCheckBox *>().empty());
        EXPECT_TRUE(dialog->findChildren<QuantityEditor *>().empty());
        EXPECT_TRUE(dialog->findChildren<QLineEdit *>().empty());
        dialog->findChild<QDialogButtonBox *>()->button(QDialogButtonBox::Close)->click();
    });
    editor.findChild<QPushButton *>("propagatorDetails")->click();
    EXPECT_EQ(submissions, 0);
    EXPECT_EQ(step.unit(), Unit::minute);
    EXPECT_DOUBLE_EQ(step.value(Unit::minute), 0.5);
    editor.submit();
    ASSERT_TRUE(submitted);
    EXPECT_EQ(submitted->propagator, OrbitPropagator::j2_secular);
    EXPECT_EQ(submitted->step_seconds, 30);
    const auto mean = runtime::j2_mean_ellipse_elements(submitted->initial);
    EXPECT_NEAR(mean.mean_anomaly_rad, std::numbers::pi / 2, 1e-13);
    editor.set_input(*submitted);
    EXPECT_NEAR(field(editor, "meanAnomaly").value(Unit::degree), 90, 1e-10);
    // Inspecting a model must not validate or overwrite a partially edited field.
    text(step)->setText("unfinished");
    QTimer::singleShot(0, &editor, [&] {
        auto *dialog = editor.findChild<QDialog *>("propagatorDetailsDialog");
        ASSERT_NE(dialog, nullptr);
        QKeyEvent escape(QEvent::KeyPress, Qt::Key_Escape, Qt::NoModifier);
        QCoreApplication::sendEvent(dialog, &escape);
    });
    editor.open_propagator_details();
    EXPECT_EQ(submissions, 1);
    EXPECT_EQ(text(step)->text(), "unfinished");
    EXPECT_EQ(step.unit(), Unit::minute);
    step.set_value(30, Unit::second);
    editor.submit();
    EXPECT_EQ(submitted->step_seconds, 30);
    editor.findChild<QComboBox *>("orbitPropagator")->setCurrentIndex(0);
    EXPECT_NE(editor.findChild<QuantityEditor *>("trueAnomaly"), nullptr);
    QTimer::singleShot(0, &editor, [&] {
        auto *dialog = editor.findChild<QDialog *>("propagatorDetailsDialog");
        ASSERT_NE(dialog, nullptr);
        EXPECT_TRUE(dialog->findChildren<QuantityEditor *>().empty());
        EXPECT_TRUE(dialog->findChildren<QCheckBox *>().empty());
        dialog->reject();
    });
    editor.open_propagator_details();
    editor.submit();
    EXPECT_EQ(submitted->propagator, OrbitPropagator::two_body);
    EXPECT_EQ(submitted->step_seconds, 30);
}

TEST_F(QuantityTest, J4AndTleEditingKeepSeparateDraftsAndRejectInvalidInput) {
    apply_workbench_style(*qobject_cast<QApplication *>(QApplication::instance()));
    OrbitEditor editor(OrbitInput{});
    editor.set_time_provider(runtime::make_builtin_time_provider());
    editor.set_propagator(OrbitPropagator::j4_secular);
    field(editor, "meanAnomaly").set_value(45, Unit::degree);
    std::optional<OrbitInput> submitted;
    editor.apply = [&](OrbitInput value) { submitted = std::move(value); };
    editor.submit();
    ASSERT_TRUE(submitted);
    EXPECT_EQ(submitted->propagator, OrbitPropagator::j4_secular);
    EXPECT_NEAR(runtime::j4_mean_ellipse_elements(submitted->initial).mean_anomaly_rad,
                std::numbers::pi / 4, 1e-10);
    const auto manual = submitted->initial;
    editor.set_propagator(OrbitPropagator::sgp4);
    auto *input = editor.findChild<QPlainTextEdit *>("tleInput");
    ASSERT_NE(input, nullptr);
    EXPECT_TRUE(editor.findChild<QComboBox *>("stateRepresentation")->isHidden());
    EXPECT_FALSE(editor.findChild<QComboBox *>("initialReferenceFrame")->isEnabled());
    submitted.reset();
    input->setPlainText("incomplete");
    editor.submit();
    EXPECT_FALSE(submitted);
    input->setPlainText("VANGUARD\n"
                        "1 00005U 58002B   00179.78495062  .00000023  00000-0  28098-4 0  4753\n"
                        "2 00005  34.2682 348.7242 1859667 331.7664  19.3264 10.82419157413667");
    editor.submit();
    ASSERT_TRUE(submitted);
    EXPECT_EQ(submitted->propagator, OrbitPropagator::sgp4);
    EXPECT_EQ(submitted->tle.line1.size(), 69U);
    EXPECT_EQ(submitted->initial.position_m, manual.position_m);
    const auto saved = *submitted;
    auto fixed_draft = saved;
    fixed_draft.input_frame = core::FrameId::itrf;
    editor.transform_state = [](core::CartesianState, core::FrameId) -> core::CartesianState {
        throw std::runtime_error("Inactive draft epoch is outside current EOP coverage");
    };
    EXPECT_NO_THROW(editor.set_input(fixed_draft));
    editor.submit();
    EXPECT_EQ(submitted->tle, saved.tle);
    EXPECT_EQ(submitted->initial.position_m, saved.initial.position_m);
    editor.set_propagator(OrbitPropagator::j4_secular);
    EXPECT_FALSE(editor.findChild<QComboBox *>("stateRepresentation")->isHidden());
    editor.submit();
    EXPECT_TRUE(submitted->tle.line1.empty());
    EXPECT_NEAR(runtime::j4_mean_ellipse_elements(submitted->initial).mean_anomaly_rad,
                std::numbers::pi / 4, 1e-10);
    editor.set_propagator(OrbitPropagator::sgp4);
    EXPECT_TRUE(input->toPlainText().contains("00005"));
    const auto capture = qEnvironmentVariable("ORBITFORGE_PROPAGATOR_UI_CAPTURE");
    if (!capture.isEmpty()) {
        ASSERT_TRUE(QDir{}.mkpath(capture));
        editor.resize(1120, 720);
        editor.show();
        QApplication::processEvents();
        ASSERT_TRUE(editor.grab().save(capture + "/sgp4-wide.png"));
        editor.resize(840, 560);
        QApplication::processEvents();
        ASSERT_TRUE(editor.grab().save(capture + "/sgp4-compact.png"));
        editor.resize(1120, 720);
        editor.set_propagator(OrbitPropagator::j4_secular);
        QApplication::processEvents();
        ASSERT_TRUE(editor.grab().save(capture + "/j4-wide.png"));
    }
}
TEST_F(QuantityTest, MissionEditorValidatesSequencePreservesDraftAndCancelsWithoutApplying) {
    apply_workbench_style(*qobject_cast<QApplication *>(QApplication::instance()));
    OrbitEditor editor(OrbitInput{});
    editor.resize(1100, 760);
    editor.show();
    editor.set_propagator(OrbitPropagator::mission);
    std::optional<OrbitInput> submitted;
    editor.apply = [&](OrbitInput input) { submitted = std::move(input); };
    const auto capture = qEnvironmentVariable("ORBITFORGE_MISSION_UI_CAPTURE");
    QTimer::singleShot(0, &editor, [&] {
        auto *dialog = editor.findChild<QDialog *>("missionEditorDialog");
        ASSERT_NE(dialog, nullptr);
        auto *list = dialog->findChild<QListWidget *>("missionSegments");
        ASSERT_EQ(list->count(), 1);
        auto *duration = dialog->findChild<QuantityEditor *>("missionCoastDuration");
        duration->set_value(600, Unit::second);
        dialog->findChild<QPushButton *>("missionAddImpulse")->click();
        dialog->findChild<QLineEdit *>("missionSegmentName")->setText(QStringLiteral("升轨点火"));
        dialog->findChild<QuantityEditor *>("missionDeltaV0")
            ->set_value(120, Unit::metres_per_second);
        dialog->findChild<QComboBox *>("missionManeuverFrame")->setCurrentIndex(1);
        dialog->findChild<QPushButton *>("missionMoveUp")->click();
        EXPECT_EQ(list->currentRow(), 0);
        dialog->findChild<QPushButton *>("missionMoveDown")->click();
        EXPECT_EQ(list->currentRow(), 1);
        dialog->findChild<QPushButton *>("missionAddCoast")->click();
        auto *ok = dialog->findChild<QDialogButtonBox *>()->button(QDialogButtonBox::Ok);
        ok->click();
        EXPECT_TRUE(dialog->isVisible());
        EXPECT_TRUE(dialog->findChild<QLabel *>("errorText")->isVisible());
        text(*duration)->setText("unfinished");
        list->setCurrentRow(1);
        EXPECT_EQ(list->currentRow(), 2);
        duration->set_value(6600, Unit::second);
        list->setCurrentRow(1);
        EXPECT_EQ(list->currentRow(), 1);
        if (!capture.isEmpty()) {
            ASSERT_TRUE(QDir{}.mkpath(capture));
            const auto settled = [] {
                QEventLoop loop;
                QTimer::singleShot(80, &loop, &QEventLoop::quit);
                loop.exec();
            };
            settled();
            EXPECT_TRUE(dialog->grab().save(capture + "/mission-wide.png"));
            dialog->resize(440, 760);
            settled();
            EXPECT_TRUE(dialog->grab().save(capture + "/mission-narrow.png"));
            for (auto *scroll : dialog->findChildren<QScrollArea *>())
                EXPECT_EQ(scroll->horizontalScrollBar()->maximum(), 0);
            EXPECT_TRUE(ok->isVisible());
            dialog->resize(900, 690);
            settled();
            EXPECT_DOUBLE_EQ(dialog->findChild<QuantityEditor *>("missionDeltaV0")
                                 ->value(Unit::metres_per_second),
                             120);
        }
        ok->click();
    });
    editor.open_propagator_details();
    EXPECT_FALSE(submitted);
    editor.submit();
    ASSERT_TRUE(submitted);
    ASSERT_EQ(submitted->mission.segments.size(), 3U);
    EXPECT_EQ(submitted->mission.segments[1].delta_v_mps[0], 120);
    const auto saved = *submitted;
    editor.set_input(saved);
    QTimer::singleShot(0, &editor, [&] {
        auto *dialog = editor.findChild<QDialog *>("missionEditorDialog");
        ASSERT_NE(dialog, nullptr);
        EXPECT_EQ(dialog->findChild<QListWidget *>("missionSegments")->count(), 3);
        dialog->findChild<QPushButton *>("missionRemove")->click();
        dialog->reject();
    });
    editor.open_propagator_details();
    editor.submit();
    EXPECT_EQ(submitted->mission, saved.mission);
}
TEST_F(QuantityTest, MissionTimelineUpdatesExactBurnTimesAndRecoversFromIncompleteInput) {
    using namespace std::chrono_literals;
    apply_workbench_style(*qobject_cast<QApplication *>(QApplication::instance()));
    core::MissionPlan plan{
        core::MissionCoastModel::two_body,
        {core::MissionSegment::coast(600s + 1ns), core::MissionSegment::impulse({3, 4, 0}),
         core::MissionSegment::impulse({0, 0, 12}), core::MissionSegment::coast(6600s)}};
    core::HpopOptions hpop;
    QWidget parent;
    QTimer::singleShot(0, &parent, [&] {
        auto *dialog = parent.findChild<QDialog *>("missionEditorDialog");
        ASSERT_NE(dialog, nullptr);
        auto *list = dialog->findChild<QListWidget *>("missionSegments");
        auto *coverage = dialog->findChild<QLabel *>("missionCoverage");
        auto *timing = dialog->findChild<QLabel *>("missionSegmentTiming");
        auto *duration = dialog->findChild<QuantityEditor *>("missionCoastDuration");
        auto *error = dialog->findChild<QLabel *>("errorText");
        EXPECT_FALSE(coverage->property("warning").toBool());
        EXPECT_TRUE(list->item(1)->toolTip().contains("T+00:10:00.000000001"));
        EXPECT_TRUE(list->item(2)->toolTip().contains("T+00:10:00.000000001"));
        duration->set_value(1200, Unit::second);
        EXPECT_TRUE(list->item(1)->toolTip().contains("T+00:20:00"));
        list->setCurrentRow(1);
        EXPECT_TRUE(timing->text().contains("T+00:20:00"));
        EXPECT_TRUE(
            dialog->findChild<QLabel *>("missionImpulseMagnitude")->text().contains("5 m/s"));
        EXPECT_TRUE(dialog->findChild<QLabel *>("missionSummary")->text().contains("17 m/s"));
        dialog->findChild<QComboBox *>("missionManeuverFrame")->setCurrentIndex(2);
        EXPECT_TRUE(list->item(1)->toolTip().contains("LVLH"));
        auto *dv = dialog->findChild<QuantityEditor *>("missionDeltaV0");
        text(*dv)->setText("-");
        EXPECT_FALSE(error->isHidden());
        EXPECT_TRUE(coverage->text().contains(QStringLiteral("输入尚未完成")));
        list->setCurrentRow(2);
        EXPECT_EQ(list->currentRow(), 1);
        // Returning to the previous number emits no valueChanged signal.
        text(*dv)->setText("3");
        EXPECT_TRUE(error->isHidden());
        EXPECT_FALSE(coverage->property("warning").toBool());
        list->setCurrentRow(0);
        duration->set_value(1e-9, Unit::second);
        EXPECT_TRUE(coverage->property("warning").toBool());
        EXPECT_TRUE(coverage->text().contains(QStringLiteral("延长滑行段")));
        duration->set_value(600, Unit::second);
        EXPECT_FALSE(coverage->property("warning").toBool());
        auto *ok = dialog->findChild<QDialogButtonBox *>()->button(QDialogButtonBox::Ok);
        ok->click();
    });
    ASSERT_TRUE(edit_mission(plan, hpop, core::Epoch{0}, core::Epoch{0},
                             core::Epoch{7'200'000'000'000}, &parent));
    EXPECT_EQ(plan.segments[0].duration_nanoseconds, 600'000'000'000);
    EXPECT_EQ(plan.segments[1].frame, core::ManeuverFrame::lvlh);
}
TEST_F(QuantityTest, MissionCompactPagesPreserveUnitsAndDraftsAndHandleEmptySequence) {
    using namespace std::chrono_literals;
    apply_workbench_style(*qobject_cast<QApplication *>(QApplication::instance()));
    core::MissionPlan plan{core::MissionCoastModel::two_body,
                           {core::MissionSegment::coast(600s),
                            core::MissionSegment::impulse({120, 0, 0}, core::ManeuverFrame::vnc,
                                                          std::string(300, 'A')),
                            core::MissionSegment::coast(6600s)}};
    const auto original = plan;
    core::HpopOptions hpop;
    QWidget parent;
    const auto capture = qEnvironmentVariable("ORBITFORGE_MISSION_UI_CAPTURE");
    QTimer::singleShot(0, &parent, [&] {
        auto *dialog = parent.findChild<QDialog *>("missionEditorDialog");
        ASSERT_NE(dialog, nullptr);
        const auto settled = [] {
            QEventLoop loop;
            QTimer::singleShot(80, &loop, &QEventLoop::quit);
            loop.exec();
        };
        const auto screenshot = [&](const char *name) {
            if (!capture.isEmpty()) {
                EXPECT_TRUE(QDir{}.mkpath(capture));
                EXPECT_TRUE(dialog->grab().save(capture + '/' + name + ".png"));
            }
        };
        auto *list = dialog->findChild<QListWidget *>("missionSegments");
        auto *navigation = dialog->findChild<QTabBar *>("missionNavigation");
        auto *scroll = dialog->findChild<QScrollArea *>("missionParameters");
        auto *dv = dialog->findChild<QuantityEditor *>("missionDeltaV0");
        auto *ok = dialog->findChild<QDialogButtonBox *>()->button(QDialogButtonBox::Ok);
        list->setCurrentRow(1);
        dv->set_unit(Unit::kilometres_per_second);
        text(*dv)->setText("1e");
        for (const auto size : {QSize{980, 740}, QSize{400, 520}, QSize{440, 760}}) {
            dialog->resize(size);
            navigation->setCurrentIndex(1);
            settled();
            EXPECT_EQ(list->currentRow(), 1);
            EXPECT_EQ(dv->unit(), Unit::kilometres_per_second);
            EXPECT_EQ(text(*dv)->text(), "1e");
            EXPECT_EQ(scroll->horizontalScrollBar()->maximum(), 0);
            EXPECT_GT(scroll->viewport()->height(), 130);
            EXPECT_TRUE(dialog->rect().contains(ok->mapTo(dialog, ok->rect().bottomRight())));
            screenshot(size.width() == 400   ? "mission-short-invalid"
                       : size.width() == 440 ? "mission-narrow-invalid"
                                             : "mission-wide-long-name");
        }
        EXPECT_TRUE(navigation->isVisible());
        navigation->setCurrentIndex(0);
        settled();
        EXPECT_TRUE(list->isVisible());
        EXPECT_FALSE(scroll->isVisible());
        screenshot("mission-narrow-sequence");
        // Save returns to the invalid field rather than losing the hidden draft.
        ok->click();
        EXPECT_EQ(navigation->currentIndex(), 1);
        EXPECT_TRUE(text(*dv)->hasFocus());
        settled();
        EXPECT_TRUE(scroll->viewport()->rect().contains(
            text(*dv)->mapTo(scroll->viewport(), text(*dv)->rect().center())));
        text(*dv)->setText("0.12");
        dialog->resize(980, 740);
        settled();
        EXPECT_FALSE(navigation->isVisible());
        EXPECT_TRUE(list->isVisible());
        EXPECT_TRUE(scroll->isVisible());
        EXPECT_DOUBLE_EQ(dv->value(Unit::metres_per_second), 120);
        screenshot("mission-wide-restored");
        // List shortcuts reorder only the selected segment; Enter opens parameters,
        // without accidentally accepting the whole plan through QDialog's default button.
        const auto key = [&](int value, Qt::KeyboardModifiers modifiers = Qt::NoModifier) {
            QKeyEvent press(QEvent::KeyPress, value, modifiers);
            QKeyEvent release(QEvent::KeyRelease, value, modifiers);
            QApplication::sendEvent(list, &press);
            QApplication::sendEvent(list, &release);
        };
        list->setFocus();
        key(Qt::Key_Up, Qt::AltModifier);
        EXPECT_EQ(list->currentRow(), 0);
        EXPECT_EQ(list->item(0)->text(), QString(300, 'A'));
        key(Qt::Key_Down, Qt::AltModifier);
        EXPECT_EQ(list->currentRow(), 1);
        navigation->setCurrentIndex(0);
        key(Qt::Key_Return);
        EXPECT_TRUE(dialog->isVisible());
        EXPECT_EQ(navigation->currentIndex(), 1);
        key(Qt::Key_Delete);
        EXPECT_EQ(list->count(), 2);
        while (list->count() > 0)
            dialog->findChild<QPushButton *>("missionRemove")->click();
        EXPECT_TRUE(dialog->findChild<QLabel *>("missionEmpty")->isVisible());
        EXPECT_FALSE(dialog->findChild<QLineEdit *>("missionSegmentName")->isVisible());
        EXPECT_FALSE(navigation->isTabEnabled(1));
        EXPECT_FALSE(dialog->findChild<QPushButton *>("missionMoveUp")->isEnabled());
        EXPECT_FALSE(dialog->findChild<QPushButton *>("missionRemove")->isEnabled());
        settled();
        screenshot("mission-empty");
        dialog->findChild<QPushButton *>("missionAddCoast")->click();
        EXPECT_EQ(list->count(), 1);
        EXPECT_TRUE(navigation->isTabEnabled(1));
        EXPECT_TRUE(dialog->findChild<QLabel *>("errorText")->isHidden());
        dialog->reject();
    });
    EXPECT_FALSE(edit_mission(plan, hpop, core::Epoch{0}, core::Epoch{0},
                              core::Epoch{7'200'000'000'000}, &parent));
    EXPECT_EQ(plan, original);
}
TEST_F(QuantityTest, HpopSettingsValidateCancelAndRoundTripWithoutApplyingTheOrbit) {
    OrbitInput input;
    input.propagator = OrbitPropagator::hpop;
    OrbitEditor editor(input);
    apply_workbench_style(*qobject_cast<QApplication *>(QApplication::instance()));
    editor.resize(1100, 740);
    editor.show();
    std::optional<OrbitInput> submitted;
    editor.apply = [&](OrbitInput value) { submitted = std::move(value); };
    const auto capture = qEnvironmentVariable("ORBITFORGE_HPOP_UI_CAPTURE");
    QTimer::singleShot(0, &editor, [&] {
        auto *dialog = editor.findChild<QDialog *>("hpopSettingsDialog");
        ASSERT_NE(dialog, nullptr);
        auto *degree = dialog->findChild<QSpinBox *>("hpopGravityDegree");
        auto *order = dialog->findChild<QSpinBox *>("hpopGravityOrder");
        auto *tabs = dialog->findChild<QTabWidget *>();
        ASSERT_NE(degree, nullptr);
        ASSERT_NE(order, nullptr);
        degree->setValue(4);
        EXPECT_EQ(order->value(), 4);
        if (!capture.isEmpty()) {
            ASSERT_TRUE(QDir{}.mkpath(capture));
            QApplication::processEvents();
            EXPECT_TRUE(dialog->grab().save(capture + "/hpop-forces.png"));
            tabs->setCurrentIndex(2);
            QApplication::processEvents();
            EXPECT_TRUE(dialog->grab().save(capture + "/hpop-integration.png"));
            dialog->resize(400, 460);
            QApplication::processEvents();
            EXPECT_TRUE(dialog->grab().save(capture + "/hpop-compact.png"));
        }
        auto *minimum = dialog->findChild<QuantityEditor *>("hpopMinStep");
        auto *maximum = dialog->findChild<QuantityEditor *>("hpopMaxStep");
        minimum->set_value(50, Unit::second);
        maximum->set_value(10, Unit::second);
        dialog->findChild<QDialogButtonBox *>()->button(QDialogButtonBox::Ok)->click();
        EXPECT_TRUE(dialog->isVisible());
        EXPECT_TRUE(dialog->findChild<QLabel *>("errorText")->isVisible());
        minimum->set_value(.01, Unit::second);
        maximum->set_value(30, Unit::second);
        dialog->findChild<QDialogButtonBox *>()->button(QDialogButtonBox::Ok)->click();
    });
    editor.open_propagator_details();
    EXPECT_FALSE(submitted);
    editor.submit();
    ASSERT_TRUE(submitted);
    EXPECT_EQ(submitted->hpop.gravity_degree, 4);
    EXPECT_EQ(submitted->hpop.gravity_order, 4);
    EXPECT_EQ(submitted->hpop.max_step_seconds, 30);
    const auto saved = *submitted;
    editor.set_input(saved);
    QTimer::singleShot(0, &editor, [&] {
        auto *dialog = editor.findChild<QDialog *>("hpopSettingsDialog");
        ASSERT_NE(dialog, nullptr);
        EXPECT_EQ(dialog->findChild<QSpinBox *>("hpopGravityDegree")->value(), 4);
        dialog->findChild<QComboBox *>("hpopIntegrator")->setCurrentIndex(1);
        dialog->reject();
    });
    editor.open_propagator_details();
    editor.submit();
    EXPECT_EQ(submitted->hpop, saved.hpop);
    EXPECT_NEAR(submitted->initial.position_m[0], saved.initial.position_m[0], 1e-7);
    if (!capture.isEmpty()) {
        QApplication::processEvents();
        EXPECT_TRUE(editor.grab().save(capture + "/hpop-editor-wide.png"));
        editor.resize(760, 540);
        QEventLoop settled;
        QTimer::singleShot(50, &settled, &QEventLoop::quit);
        settled.exec(); // Allow queued responsive layouts to settle before the capture.
        EXPECT_TRUE(editor.grab().save(capture + "/hpop-editor-compact.png"));
    }
}
TEST_F(QuantityTest, HpopUsesInstalledGravityLimitsAndPersistsSelectedModel) {
    core::HpopOptions options;
    runtime::CatalogGravity entry;
    entry.selection = "egm2008";
    entry.info = {"EGM2008", {}, {}, "tide_free", core::egm96_mu, core::egm96_radius_m, 2190, 2190};
    QTimer::singleShot(0, [&] {
        auto *dialog = qobject_cast<QDialog *>(QApplication::activeModalWidget());
        ASSERT_TRUE(dialog);
        auto *model = dialog->findChild<QComboBox *>("hpopGravityModel");
        model->setCurrentIndex(model->findData(QStringLiteral("egm2008")));
        auto *degree = dialog->findChild<QSpinBox *>("hpopGravityDegree");
        EXPECT_EQ(degree->maximum(), 360);
        degree->setValue(21);
        dialog->findChild<QSpinBox *>("hpopGravityOrder")->setValue(21);
        dialog->findChild<QDialogButtonBox *>()->button(QDialogButtonBox::Ok)->click();
    });
    EXPECT_TRUE(edit_hpop_settings(options, nullptr, {entry}));
    EXPECT_EQ(options.gravity_model, "egm2008");
    EXPECT_EQ(options.gravity_degree, 21);
    EXPECT_EQ(options.gravity_order, 21);
    QTimer::singleShot(0, [&] {
        auto *dialog = qobject_cast<QDialog *>(QApplication::activeModalWidget());
        ASSERT_TRUE(dialog);
        dialog->findChild<QDialogButtonBox *>()->button(QDialogButtonBox::Ok)->click();
        EXPECT_TRUE(dialog->isVisible());
        EXPECT_TRUE(dialog->findChild<QLabel *>("errorText")->isVisible());
        dialog->reject();
    });
    EXPECT_FALSE(edit_hpop_settings(options, nullptr));
    EXPECT_EQ(options.gravity_model, "egm2008");
}
TEST_F(QuantityTest, HpopThirdBodiesPreserveDraftsAndValidateManualConstants) {
    apply_workbench_style(*qobject_cast<QApplication *>(QApplication::instance()));
    core::HpopOptions options;
    bool managed = false;
    QTimer::singleShot(0, [&] {
        auto *dialog = qobject_cast<QDialog *>(QApplication::activeModalWidget());
        ASSERT_TRUE(dialog);
        auto *tabs = dialog->findChild<QTabWidget *>("hpopSettingsTabs");
        ASSERT_TRUE(tabs);
        tabs->setCurrentIndex(1);
        auto *sun = dialog->findChild<QCheckBox *>("hpopSunGravity");
        auto *moon = dialog->findChild<QCheckBox *>("hpopMoonGravity");
        auto *source = dialog->findChild<QComboBox *>("hpopSunSource");
        auto *quantity = dialog->findChild<QuantityEditor *>("hpopSunMu");
        auto *mu = quantity->findChild<QLineEdit *>("quantityValue");
        ASSERT_TRUE(sun && moon && source && mu);
        EXPECT_EQ(source->currentIndex(), 0);
        EXPECT_EQ(dialog->findChild<QComboBox *>("hpopMoonSource")->currentIndex(), 0);
        EXPECT_FALSE(options.sun_mu || options.moon_mu);
        EXPECT_FALSE(source->isEnabled());
        sun->setChecked(true);
        moon->setChecked(true);
        EXPECT_TRUE(mu->isReadOnly());
        const auto constant = quantity->value(Unit::metres_cubed_per_second_squared);
        QKeyEvent up(QEvent::KeyPress, Qt::Key_Up, Qt::NoModifier);
        QApplication::sendEvent(mu, &up);
        EXPECT_EQ(quantity->value(Unit::metres_cubed_per_second_squared), constant);
        quantity->set_unit(Unit::metres_cubed_per_second_squared);
        EXPECT_EQ(quantity->value(Unit::metres_cubed_per_second_squared), constant);
        quantity->set_unit(Unit::kilometres_cubed_per_second_squared);
        source->setCurrentIndex(1);
        mu->setText("1.3271244e11");
        source->setCurrentIndex(0);
        EXPECT_TRUE(mu->isReadOnly());
        source->setCurrentIndex(1);
        EXPECT_EQ(mu->text(), QStringLiteral("1.3271244e11"));
        dialog->findChild<QToolButton *>("hpopManageEphemerides")->click();
        EXPECT_TRUE(managed);
        EXPECT_EQ(mu->text(), QStringLiteral("1.3271244e11"));
        EXPECT_TRUE(dialog->findChild<QLabel *>("hpopEphemerisStatus")->text().contains("2000"));
        const auto output = qEnvironmentVariable("ORBITFORGE_HPOP_UI_CAPTURE");
        if (!output.isEmpty()) {
            source->setCurrentIndex(0); // Preview the normal, automatic-GM configuration.
            QDir{}.mkpath(output);
            QApplication::processEvents();
            EXPECT_TRUE(dialog->grab().save(output + "/third-body-wide.png"));
            dialog->resize(460, 680);
            QEventLoop settled;
            QTimer::singleShot(60, &settled, &QEventLoop::quit);
            settled.exec();
            EXPECT_TRUE(dialog->grab().save(output + "/third-body-compact.png"));
            source->setCurrentIndex(1);
        }
        mu->setText("0");
        dialog->findChild<QDialogButtonBox *>()->button(QDialogButtonBox::Ok)->click();
        EXPECT_TRUE(dialog->isVisible());
        mu->setText("1.3271244e11");
        dialog->findChild<QDialogButtonBox *>()->button(QDialogButtonBox::Ok)->click();
    });
    EXPECT_TRUE(edit_hpop_settings(options, nullptr, {}, {}, [&] {
        managed = true;
        runtime::CatalogEphemeris entry;
        entry.selection = "de440s";
        entry.info.first = core::Epoch{0};
        entry.info.last = core::Epoch{86400000000000};
        return std::vector{entry};
    }));
    EXPECT_TRUE(options.sun_gravity && options.moon_gravity);
    ASSERT_TRUE(options.sun_mu);
    EXPECT_EQ(*options.sun_mu, 1.3271244e20);
    EXPECT_FALSE(options.moon_mu);
    const auto saved = options;
    QTimer::singleShot(0, [&] {
        auto *dialog = qobject_cast<QDialog *>(QApplication::activeModalWidget());
        ASSERT_TRUE(dialog);
        EXPECT_TRUE(dialog->findChild<QCheckBox *>("hpopSunGravity")->isChecked());
        dialog->findChild<QCheckBox *>("hpopMoonGravity")->setChecked(false);
        dialog->reject();
    });
    EXPECT_FALSE(edit_hpop_settings(options, nullptr));
    EXPECT_EQ(options, saved);
}
} // namespace orbitforge::desktop
