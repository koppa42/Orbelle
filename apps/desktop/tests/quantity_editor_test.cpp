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
#include <QDir>
#include <QKeyEvent>
#include <QLineEdit>
#include <QToolButton>
#include <array>
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

TEST_F(QuantityTest, ExpandedUnitsMatchReferenceRelationships) {
    // Independent relationships from NIST SP 811 and IAU 2012 Resolution B2;
    // EXPECT_DOUBLE_EQ allows four ULPs for binary factors and the two arithmetic operations.
    struct Reference {
        double input;
        Unit from;
        double expected;
        Unit to;
    };
    const std::array references{
        Reference{12, Unit::inch, 1, Unit::foot},
        Reference{1, Unit::inch, 25.4, Unit::millimetre},
        Reference{100, Unit::centimetre, 1, Unit::metre},
        Reference{1, Unit::foot, 30.48, Unit::centimetre},
        Reference{1, Unit::mile, 5280, Unit::foot},
        Reference{1, Unit::nautical_mile, 1852, Unit::metre},
        Reference{1, Unit::astronomical_unit, 149597870.7, Unit::kilometre},
        Reference{1, Unit::centimetres_per_second, 10, Unit::millimetres_per_second},
        Reference{1, Unit::metres_per_second, 100, Unit::centimetres_per_second},
        Reference{36, Unit::kilometres_per_hour, 10, Unit::metres_per_second},
        Reference{60, Unit::miles_per_hour, 88, Unit::feet_per_second},
        Reference{1, Unit::feet_per_second, 0.3048, Unit::metres_per_second},
        Reference{1, Unit::knot, 1.852, Unit::kilometres_per_hour},
        Reference{1, Unit::revolution, 360, Unit::degree},
        Reference{1, Unit::degree, 60, Unit::arcminute},
        Reference{1, Unit::arcminute, 60, Unit::arcsecond},
        Reference{1, Unit::arcsecond, 1000, Unit::milliarcsecond},
        Reference{1, Unit::millisecond, 1000, Unit::microsecond},
        Reference{1, Unit::microsecond, 1000, Unit::nanosecond},
        Reference{1, Unit::week, 7, Unit::day},
        Reference{10000, Unit::square_centimetres_per_kilogram, 1,
                  Unit::square_metres_per_kilogram},
        Reference{1, Unit::square_metres_per_gram, 10000, Unit::square_centimetres_per_gram},
        Reference{4.56, Unit::micropascal, 4560, Unit::nanopascal},
        Reference{1, Unit::pascal, 1000, Unit::millipascal},
        Reference{1, Unit::millipascal, 1000, Unit::micropascal},
        Reference{1, Unit::bar, 1000, Unit::hectopascal},
        Reference{1, Unit::hectopascal, 100, Unit::pascal},
        Reference{1, Unit::bar, 100, Unit::kilopascal},
        Reference{1, Unit::megapascal, 10, Unit::bar},
        Reference{1, Unit::pound, 453.59237, Unit::gram},
        Reference{1, Unit::gram, 1000, Unit::milligram}};
    for (const auto &reference : references) {
        SCOPED_TRACE(unit_symbol(reference.from).toStdString() + " -> " +
                     unit_symbol(reference.to).toStdString());
        for (const double sign : {-1.0, 1.0}) {
            EXPECT_DOUBLE_EQ(convert_quantity(sign * reference.input, reference.from, reference.to),
                             sign * reference.expected);
            EXPECT_DOUBLE_EQ(
                convert_quantity(sign * reference.expected, reference.to, reference.from),
                sign * reference.input);
        }
    }
}

TEST_F(QuantityTest, ExpandedMenusPreserveExactValuesAndFitNarrowFields) {
    apply_workbench_style(*qApp);
    const double original = 1.2345678901234567;
    for (const auto base : {Unit::metre, Unit::metres_per_second, Unit::radian, Unit::second,
                            Unit::square_metres_per_kilogram, Unit::pascal, Unit::kilogram}) {
        QuantityEditor editor(base);
        editor.set_value(original, base);
        editor.show();
        auto *button = editor.findChild<QToolButton *>("quantityUnit");
        int changes = 0;
        QObject::connect(&editor, &QuantityEditor::valueChanged, [&changes] { ++changes; });
        for (const auto width : {130, 276}) {
            editor.resize(width, editor.sizeHint().height());
            for (const auto unit : compatible_units(base)) {
                SCOPED_TRACE(unit_symbol(unit).toStdString());
                choose(editor, unit);
                QCoreApplication::processEvents();
                EXPECT_EQ(editor.unit(), unit);
                EXPECT_EQ(button->text(), unit_symbol(unit));
                EXPECT_EQ(editor.value(base), original);
                EXPECT_EQ(changes, 0);
                EXPECT_GE(button->width(),
                          button->fontMetrics().horizontalAdvance(button->text()) + 7 + 17 + 1 + 8);
                EXPECT_GE(text(editor)->width(), 60);
                EXPECT_TRUE(editor.rect().contains(button->geometry()))
                    << "field=" << editor.width() << 'x' << editor.height()
                    << " button=" << button->x() << ',' << button->y() << ' ' << button->width()
                    << 'x' << button->height();
                EXPECT_TRUE(editor.rect().contains(text(editor)->geometry()));
                int checked = 0;
                for (auto *action : button->menu()->actions())
                    if (action->isChecked()) {
                        ++checked;
                        EXPECT_EQ(action->data().toInt(), static_cast<int>(unit));
                    }
                EXPECT_EQ(checked, 1);
            }
            const auto capture = qEnvironmentVariable("ORBELLE_QUANTITY_UI_CAPTURE");
            if (!capture.isEmpty()) {
                ASSERT_TRUE(QDir().mkpath(capture));
                EXPECT_TRUE(editor.grab().save(
                    capture +
                    QString("/quantity-%1-%2.png").arg(static_cast<int>(base)).arg(width)));
            }
        }
        EXPECT_THROW(editor.set_unit(base == Unit::metre ? Unit::knot : Unit::nautical_mile),
                     std::invalid_argument);
        EXPECT_EQ(editor.value(base), original);
    }
}

TEST_F(QuantityTest, ExpandedUnitsKeepPhysicalConstraintsAndInvalidDrafts) {
    QuantityEditor duration(Unit::second);
    duration.set_range(1, 60, Unit::second);
    duration.set_quantum(1, Unit::second);
    duration.set_single_step(1, Unit::second);
    duration.set_value(45, Unit::second);
    choose(duration, Unit::microsecond);
    text(duration)->setText("45000000");
    EXPECT_DOUBLE_EQ(duration.value(Unit::second), 45);
    QKeyEvent up(QEvent::KeyPress, Qt::Key_Up, Qt::NoModifier);
    QCoreApplication::sendEvent(text(duration), &up);
    EXPECT_DOUBLE_EQ(duration.value(Unit::second), 46);
    EXPECT_EQ(text(duration)->text(), "46000000");
    text(duration)->setText("45000001");
    EXPECT_THROW((void)duration.value(Unit::second), std::out_of_range);
    EXPECT_THROW(duration.set_unit(Unit::week), std::out_of_range);
    EXPECT_EQ(text(duration)->text(), "45000001");
    duration.set_value(60, Unit::second);
    choose(duration, Unit::nanosecond);
    text(duration)->setText("61000000000");
    EXPECT_THROW((void)duration.value(Unit::second), std::out_of_range);
    EXPECT_TRUE(duration.property("invalid").toBool());

    QuantityEditor angle(Unit::arcsecond);
    text(angle)->setText("1e");
    choose(angle, Unit::milliarcsecond);
    EXPECT_EQ(angle.unit(), Unit::arcsecond);
    EXPECT_EQ(text(angle)->text(), "1e");
    EXPECT_TRUE(angle.property("invalid").toBool());
    auto *menu = angle.findChild<QToolButton *>("quantityUnit")->menu();
    for (auto *action : menu->actions())
        EXPECT_EQ(action->isChecked(), action->data().toInt() == static_cast<int>(Unit::arcsecond));

    QuantityEditor distance(Unit::metre);
    distance.set_value(1e308, Unit::metre);
    const auto draft = text(distance)->text();
    EXPECT_THROW(distance.set_unit(Unit::millimetre), std::out_of_range);
    EXPECT_EQ(distance.unit(), Unit::metre);
    EXPECT_EQ(text(distance)->text(), draft);
    EXPECT_EQ(distance.value(Unit::metre), 1e308);
    EXPECT_THROW(convert_quantity(1e308, Unit::astronomical_unit, Unit::metre), std::out_of_range);
    EXPECT_THROW(convert_quantity(std::numeric_limits<double>::denorm_min(), Unit::metre,
                                  Unit::astronomical_unit),
                 std::out_of_range);
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
} // namespace orbelle::desktop
