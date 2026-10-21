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
#include <QKeyEvent>
#include <QLineEdit>
#include <QToolButton>
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
} // namespace orbitforge::desktop
