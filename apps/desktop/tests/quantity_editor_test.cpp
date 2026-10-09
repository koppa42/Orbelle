#include "../src/viewer/orbit_editor.hpp"
#include "../src/viewer/quantity_editor.hpp"
#include <QApplication>
#include <QComboBox>
#include <QKeyEvent>
#include <QLineEdit>
#include <QMenu>
#include <QPushButton>
#include <QToolButton>
#include <cmath>
#include <gtest/gtest.h>
#include <numbers>
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
    auto &mu = field(editor, "gravitationalParameter");
    mu.set_unit(Unit::metres_cubed_per_second_squared);
    auto &step = field(editor, "displayStep");
    step.set_unit(Unit::minute);
    text(step)->setText("0.75");
    std::optional<OrbitInput> submitted;
    editor.apply = [&](OrbitInput result) { submitted = std::move(result); };
    editor.submit();
    ASSERT_TRUE(submitted);
    EXPECT_DOUBLE_EQ(submitted->initial.position_m[0], 7.2e6);
    EXPECT_DOUBLE_EQ(submitted->initial.velocity_mps[1], 6300);
    EXPECT_DOUBLE_EQ(submitted->mu, input.mu);
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
                              core::GravitationalParameter{input.mu});
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
} // namespace orbitforge::desktop
