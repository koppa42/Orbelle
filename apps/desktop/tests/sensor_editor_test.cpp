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
#include <QCheckBox>
#include <QComboBox>
#include <QDialogButtonBox>
#include <QDir>
#include <QKeyEvent>
#include <QLabel>
#include <QLineEdit>
#include <QPushButton>
#include <QScrollArea>
#include <QScrollBar>
#include <QSpinBox>
#include <QStackedWidget>
#include <QTabBar>
#include <QTimer>
#include <QTreeWidget>
#include <cmath>
#include <fstream>
#include <gtest/gtest.h>
#include <iostream>
#include <numbers>
#include <orbitforge/runtime/ephemeris.hpp>
#include <orbitforge/runtime/j2.hpp>
#include <orbitforge/runtime/j4.hpp>
#include <orbitforge/runtime/time_frames.hpp>
#include <orbitforge/runtime/two_body.hpp>

#include "support/property_editor_fixture.hpp"

namespace orbitforge::desktop {
TEST_F(QuantityTest, SensorPatternFieldsPreserveGeometryAndRejectInvalidBoundaries) {
    application::Project project("Scenario", core::Epoch{0}, core::Epoch{120'000'000'000});
    application::ProjectSensor sensor;
    sensor.display_name = "Sensor";
    sensor.parent_satellite_id = "satellite";
    SensorEditor editor;
    editor.set_sensor(sensor, project, false);
    auto *type = editor.findChild<QComboBox *>("sensorType");
    ASSERT_TRUE(type);
    ASSERT_EQ(type->count(), 4);
    type->setCurrentIndex(1);
    editor.findChild<QuantityEditor *>("sensorInnerAngle")->set_value(10, Unit::degree);
    editor.findChild<QuantityEditor *>("sensorClockMin")->set_value(300, Unit::degree);
    editor.findChild<QuantityEditor *>("sensorClockMax")->set_value(60, Unit::degree);
    EXPECT_EQ(editor.sensor().pattern.type, core::SensorPatternType::complex_conic);
    editor.findChild<QuantityEditor *>("sensorInnerAngle")->set_value(80, Unit::degree);
    EXPECT_THROW((void)editor.sensor(), std::invalid_argument);
    type->setCurrentIndex(2);
    editor.findChild<QuantityEditor *>("sensorHorizontal")->set_value(20, Unit::degree);
    editor.findChild<QuantityEditor *>("sensorVertical")->set_value(10, Unit::degree);
    EXPECT_NEAR(editor.sensor().pattern.horizontal_half_angle_rad, std::numbers::pi / 9, 1e-14);
    type->setCurrentIndex(3);
    EXPECT_EQ(editor.sensor().pattern.custom_points.size(), 4U);
    const auto saved = editor.sensor();
    editor.set_sensor(saved, project, false);
    EXPECT_EQ(editor.sensor().pattern, saved.pattern);
    EXPECT_FALSE(editor.has_unapplied_changes());
    apply_workbench_style(*qobject_cast<QApplication *>(QApplication::instance()));
    editor.resize(340, 640);
    editor.show();
    QApplication::processEvents();
    auto *scroll = editor.findChild<QScrollArea *>("sensorScroll");
    EXPECT_EQ(scroll->horizontalScrollBar()->maximum(), 0);
    const auto capture = qEnvironmentVariable("ORBITFORGE_SENSOR_UI_CAPTURE");
    if (!capture.isEmpty())
        EXPECT_TRUE(editor.grab().save(capture + "/sensor-custom.png"));
    editor.findChild<QuantityEditor *>("sensorInnerAngle")->set_value(10, Unit::degree);
    for (int i = 0; i < 4; ++i) {
        SCOPED_TRACE(i);
        type->setCurrentIndex(i);
        for (const int width : {320, 560, 1500}) {
            editor.resize(width, 680);
            QApplication::processEvents();
            QApplication::processEvents();
            EXPECT_EQ(scroll->horizontalScrollBar()->maximum(), 0);
            EXPECT_LE(scroll->widget()->width(), 640);
            EXPECT_LE(scroll->widget()->width(), scroll->viewport()->width());
            auto *preview = editor.findChild<QWidget *>("sensorAnglePreview");
            ASSERT_TRUE(preview);
            EXPECT_TRUE(preview->isVisible());
            EXPECT_TRUE(scroll->widget()->rect().contains(
                QRect(preview->mapTo(scroll->widget(), QPoint{}), preview->size())));
            if (!capture.isEmpty())
                EXPECT_TRUE(editor.grab().save(
                    capture + QString("/sensor-type-%1-%2.png").arg(i).arg(width)));
        }
    }
    auto precise = saved;
    precise.pattern.custom_points[0].cone_rad = .345678901234567;
    precise.display.opacity = .123456789;
    editor.set_sensor(precise, project, false);
    EXPECT_EQ(editor.sensor().pattern, precise.pattern);
    EXPECT_EQ(editor.sensor().display, precise.display);
    EXPECT_FALSE(editor.has_unapplied_changes());
}

TEST_F(QuantityTest, PreparingSensorDoesNotShowTemporaryWindows) {
    WindowShowObserver windows;
    application::Project project("Scenario", core::Epoch{0}, core::Epoch{120'000'000'000});
    project.ground_stations.push_back({"station", "Station", {1.2, 0.6, 350}});
    application::ProjectSensor sensor;
    sensor.object_id = "sensor";
    sensor.parent_satellite_id = "satellite";
    sensor.pattern.half_angle_rad = 0.2;
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
    EXPECT_NEAR(applied->pattern.half_angle_rad, std::numbers::pi / 6, 1e-14);
    editor.set_sensor(*applied, project, false);
    EXPECT_EQ(angle->unit(), Unit::radian);
    EXPECT_EQ(target->id(), "station-b");
    EXPECT_EQ(submit->text(), QStringLiteral("应用"));
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
    text(*angle)->setText("181");
    submit->click();
    EXPECT_EQ(applications, 0);
    text(*angle)->setText("30");
    auto *name = editor.findChild<QLineEdit *>("sensorName");
    ASSERT_TRUE(name);
    name->setText(" ");
    submit->click();
    EXPECT_EQ(applications, 0);
    EXPECT_EQ(error->text(), QStringLiteral("请输入对象名称。"));
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
    sensor.pattern.half_angle_rad = std::numbers::pi / 6;
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
    auto *selector = editor.findChild<QComboBox *>("propertyPageSelector");
    ASSERT_TRUE(selector);
    selector->setCurrentIndex(1);
    scroll = editor.findChild<QScrollArea *>("sensorPointingScroll");
    ASSERT_TRUE(scroll);
    QApplication::processEvents();
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
    EXPECT_EQ(full_angle->text(), QStringLiteral("半角范围 0.001–179.999°"));
    submit->click();
    screenshot("sensor-invalid");
    project.ground_stations.clear();
    editor.set_sensor(sensor, project, false);
    submit->click();
    screenshot("sensor-no-stations");
    EXPECT_EQ(scroll->horizontalScrollBar()->maximum(), 0);
}

TEST_F(QuantityTest, SensorPagesPreserveDraftUnitsAndSelectionAcrossNavigationModes) {
    apply_workbench_style(*qobject_cast<QApplication *>(QApplication::instance()));
    application::Project project("Scenario", core::Epoch{0}, core::Epoch{120'000'000'000});
    project.ground_stations.push_back({"station", "Ground Station", {1.2, .6, 350}});
    application::ProjectSensor sensor;
    sensor.object_id = "sensor";
    sensor.parent_satellite_id = "satellite";
    SensorEditor editor;
    editor.set_sensor(sensor, project, false);
    auto *selector = editor.findChild<QComboBox *>("propertyPageSelector");
    auto *tabs = editor.findChild<QTabBar *>("propertyPageTabs");
    auto *tree = editor.findChild<QTreeWidget *>("propertyNavigation");
    auto *pages = editor.findChild<QStackedWidget *>("sensorPages");
    auto *angle = editor.findChild<QuantityEditor *>("sensorHalfAngle");
    auto *name = editor.findChild<QLineEdit *>("sensorName");
    ASSERT_TRUE(selector && tabs && tree && pages && angle && name);
    ASSERT_EQ(pages->count(), 4);
    ASSERT_EQ(tree->topLevelItemCount(), 3);
    std::vector<QTreeWidgetItem *> leaves;
    for (int group = 0; group < tree->topLevelItemCount(); ++group) {
        auto *category = tree->topLevelItem(group);
        EXPECT_FALSE(category->data(0, Qt::UserRole).isValid());
        for (int i = 0; i < category->childCount(); ++i)
            leaves.push_back(category->child(i));
    }
    ASSERT_EQ(leaves.size(), 4U);
    name->setText("Sensor draft");
    angle->set_value(30, Unit::degree);
    choose(*angle, Unit::radian);
    editor.findChild<QPushButton *>("sensorTargeted")->click();
    auto *show_2d = editor.findChild<QCheckBox *>("sensorShow2d");
    auto *show_3d = editor.findChild<QCheckBox *>("sensorShow3d");
    auto *fill = editor.findChild<QCheckBox *>("sensorFill");
    auto *opacity = editor.findChild<QSpinBox *>("sensorOpacity");
    ASSERT_TRUE(show_2d && show_3d && fill && opacity);
    show_2d->setChecked(false);
    show_3d->setChecked(true);
    opacity->setValue(37);
    fill->setChecked(false);
    EXPECT_FALSE(opacity->isEnabled());
    fill->setChecked(true);
    EXPECT_TRUE(opacity->isEnabled());
    editor.findChild<QuantityEditor *>("sensorSpaceRange")->set_value(1234, Unit::kilometre);
    const auto display_draft = editor.sensor().display;
    editor.show();
    for (const int width : {1500, 800, 480, 320, 1500}) {
        editor.resize(width, 600);
        QApplication::processEvents();
        EXPECT_EQ(tree->isVisible(), width >= 680);
        EXPECT_EQ(tabs->isVisible(), width >= 400 && width < 680);
        EXPECT_EQ(selector->isVisible(), width < 400);
        for (int index = 0; index < pages->count(); ++index) {
            if (tree->isVisible())
                tree->setCurrentItem(leaves[index]);
            else if (tabs->isVisible())
                tabs->setCurrentIndex(index);
            else
                selector->setCurrentIndex(index);
            QApplication::processEvents();
            EXPECT_EQ(pages->currentIndex(), index);
            EXPECT_EQ(selector->currentIndex(), index);
            EXPECT_EQ(tabs->currentIndex(), index);
            EXPECT_EQ(tree->currentItem(), leaves[index]);
            auto *scroll = qobject_cast<QScrollArea *>(pages->currentWidget());
            ASSERT_TRUE(scroll);
            EXPECT_EQ(scroll->horizontalScrollBar()->maximum(), 0);
            EXPECT_LE(scroll->widget()->width(), scroll->viewport()->width());
            EXPECT_LE(scroll->widget()->width(), 640);
            const auto output = qEnvironmentVariable("ORBITFORGE_SENSOR_UI_CAPTURE");
            if (!output.isEmpty())
                EXPECT_TRUE(editor.grab().save(
                    output + QString("/sensor-page-%1-%2.png").arg(width).arg(index)));
        }
        EXPECT_EQ(pages->currentIndex(), 3);
        EXPECT_EQ(name->text(), "Sensor draft");
        EXPECT_EQ(angle->unit(), Unit::radian);
        EXPECT_EQ(editor.sensor().target_station_id, "station");
        EXPECT_NEAR(editor.sensor().pattern.half_angle_rad, std::numbers::pi / 6, 1e-14);
        EXPECT_EQ(editor.sensor().display, display_draft);
    }
    editor.findChild<QPushButton *>("sensorEditAppearance")->click();
    EXPECT_EQ(pages->currentIndex(), 2);
    EXPECT_EQ(tree->currentItem(), leaves[2]);
    EXPECT_EQ(editor.sensor().display, display_draft);
    editor.findChild<QPushButton *>("resetSensor")->click();
    EXPECT_FALSE(editor.has_unapplied_changes());
    EXPECT_EQ(pages->currentIndex(), 2);
}
} // namespace orbitforge::desktop
