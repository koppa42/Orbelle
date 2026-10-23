#include "../src/viewer/ground_station_editor.hpp"
#include "../src/viewer/project_update.hpp"
#include "../src/viewer/quantity_editor.hpp"
#include "../src/viewer/workbench_style.hpp"
#include "support/property_editor_fixture.hpp"
#include <QApplication>
#include <QComboBox>
#include <QDir>
#include <QLabel>
#include <QLineEdit>
#include <QPlainTextEdit>
#include <QPushButton>
#include <QScrollArea>
#include <QScrollBar>
#include <QStackedWidget>
#include <QTabBar>
#include <QTreeWidget>
#include <gtest/gtest.h>

namespace orbelle::desktop {
namespace {
QuantityEditor &facility_field(GroundStationEditor &editor, const char *name) {
    auto *field = editor.findChild<QuantityEditor *>(name);
    if (!field)
        throw std::logic_error("Missing facility quantity field");
    return *field;
}
} // namespace
TEST_F(QuantityTest, FacilityTypesPreserveExactLocationAndRejectSwitchingInvalidDraft) {
    application::ProjectGroundStation station{
        "facility", "Facility", {1.2345678901234567, -0.4567890123456789, 123.45, 12.5}};
    GroundStationEditor editor;
    editor.set_station(station, false);
    auto *type = editor.findChild<QComboBox *>("stationPositionType");
    ASSERT_NE(type, nullptr);
    for (int i : {1, 2, 3, 4, 0, 2, 0}) {
        type->setCurrentIndex(i);
        EXPECT_EQ(editor.station().location, station.location);
        EXPECT_EQ(static_cast<int>(editor.station().position_type), i);
    }
    EXPECT_FALSE(editor.has_unapplied_changes());
    auto &latitude = facility_field(editor, "stationLatitude");
    text(latitude)->setText("95");
    type->setCurrentIndex(2);
    EXPECT_EQ(type->currentIndex(), 0);
    EXPECT_EQ(text(latitude)->text(), "95");
    EXPECT_TRUE(editor.has_unapplied_changes());
    EXPECT_FALSE(editor.findChild<QLabel *>("stationResult")->text().isEmpty());
    EXPECT_THROW((void)editor.station(), std::exception);
    editor.findChild<QPushButton *>("resetGroundStation")->click();
    EXPECT_FALSE(editor.has_unapplied_changes());
    type->setCurrentIndex(2);
    facility_field(editor, "stationX").set_value(6379137, Unit::metre);
    facility_field(editor, "stationY").set_value(0, Unit::metre);
    facility_field(editor, "stationZ").set_value(0, Unit::metre);
    const auto value = editor.station();
    EXPECT_NEAR(value.location.height_m, 1000, 1e-7);
    EXPECT_EQ(value.location.height_above_ground_m, 12.5);
    EXPECT_EQ(value.location.longitude_rad, 0);
    EXPECT_EQ(value.location.latitude_rad, 0);
}
TEST_F(QuantityTest, FacilityDescriptionAndHeightApplyResetAndCloseKeepStableIdentity) {
    GroundStationEditor editor;
    application::ProjectGroundStation station{"facility", "Station", {0, 0, 100, 10}};
    station.description = "Original description";
    editor.set_station(station, false);
    auto *description = editor.findChild<QPlainTextEdit *>("stationDescription");
    ASSERT_NE(description, nullptr);
    description->setPlainText("Roof antenna\nSecond line");
    facility_field(editor, "stationHeightAboveGround").set_value(25, Unit::metre);
    std::optional<application::ProjectGroundStation> applied;
    editor.apply = [&](auto value) { applied = value; };
    editor.findChild<QPushButton *>("applyGroundStation")->click();
    ASSERT_TRUE(applied);
    EXPECT_EQ(applied->object_id, "facility");
    EXPECT_EQ(applied->description, "Roof antenna\nSecond line");
    EXPECT_EQ(applied->location.height_m, 100);
    EXPECT_EQ(applied->location.height_above_ground_m, 25);
    editor.accept_station(*applied);
    EXPECT_FALSE(editor.has_unapplied_changes());
    description->setPlainText(QString(17000, 'a'));
    applied.reset();
    editor.findChild<QPushButton *>("applyGroundStation")->click();
    EXPECT_FALSE(applied);
    EXPECT_TRUE(editor.has_unapplied_changes());
    editor.findChild<QPushButton *>("resetGroundStation")->click();
    EXPECT_EQ(description->toPlainText(), "Roof antenna\nSecond line");
    EXPECT_FALSE(editor.has_unapplied_changes());
    description->setPlainText("Unapplied");
    editor.close();
    EXPECT_FALSE(applied);
    editor.set_station(station, false);
    EXPECT_EQ(description->toPlainText(), "Original description");
}
TEST_F(QuantityTest, FacilityNavigationAndUnitsSurviveNarrowAndWideLayoutChanges) {
    apply_workbench_style(*qobject_cast<QApplication *>(QApplication::instance()));
    GroundStationEditor editor;
    editor.set_station({"facility", "Facility", {1.2, 0.6, 350, 12}}, false);
    editor.show();
    auto *selector = editor.findChild<QComboBox *>("propertyPageSelector");
    auto *tabs = editor.findChild<QTabBar *>("propertyPageTabs");
    auto *tree = editor.findChild<QTreeWidget *>("propertyNavigation");
    auto *description = editor.findChild<QPlainTextEdit *>("stationDescription");
    auto *type = editor.findChild<QComboBox *>("stationPositionType");
    ASSERT_TRUE(selector && tabs && tree && description && type);
    description->setPlainText("Unapplied facility notes");
    facility_field(editor, "stationHeightAboveGround").set_unit(Unit::kilometre);
    const auto settle = [] {
        for (int i = 0; i < 12; ++i)
            QCoreApplication::processEvents();
    };
    const auto capture = qEnvironmentVariable("ORBELLE_FACILITY_UI_CAPTURE");
    if (!capture.isEmpty())
        ASSERT_TRUE(QDir().mkpath(capture));
    for (int width : {920, 560, 320, 760}) {
        editor.resize(width, 640);
        settle();
        EXPECT_EQ(editor.width(), width);
        EXPECT_EQ(tree->isVisible(), width >= 680);
        EXPECT_EQ(tabs->isVisible(), width >= 400 && width < 680);
        EXPECT_EQ(selector->isVisible(), width < 400);
        for (int page = 0; page < 3; ++page) {
            selector->setCurrentIndex(page);
            settle();
            EXPECT_EQ(tree->currentItem()->data(0, Qt::UserRole).toInt(), page);
            for (auto *scroll : editor.findChildren<QScrollArea *>())
                if (scroll->isVisible()) {
                    EXPECT_EQ(scroll->horizontalScrollBar()->maximum(), 0);
                    EXPECT_LE(scroll->widget()->width(), scroll->viewport()->width());
                }
            auto *apply = editor.findChild<QPushButton *>("applyGroundStation");
            EXPECT_TRUE(editor.rect().contains(apply->mapTo(&editor, apply->rect().bottomRight())));
            if (!capture.isEmpty())
                EXPECT_TRUE(editor.grab().save(
                    capture + QString("/facility-%1-%2.png").arg(width).arg(page)));
        }
        EXPECT_EQ(description->toPlainText(), "Unapplied facility notes");
        EXPECT_EQ(facility_field(editor, "stationHeightAboveGround").unit(), Unit::kilometre);
    }
    selector->setCurrentIndex(0);
    for (int coordinate_type = 0; coordinate_type < 5; ++coordinate_type) {
        type->setCurrentIndex(coordinate_type);
        settle();
        if (!capture.isEmpty())
            EXPECT_TRUE(editor.grab().save(capture +
                                           QString("/facility-type-%1.png").arg(coordinate_type)));
    }
}
} // namespace orbelle::desktop
