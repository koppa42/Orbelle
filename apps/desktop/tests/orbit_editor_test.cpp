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
#include <QColorDialog>
#include <QComboBox>
#include <QDialog>
#include <QDialogButtonBox>
#include <QDir>
#include <QDoubleSpinBox>
#include <QKeyEvent>
#include <QLabel>
#include <QLineEdit>
#include <QPlainTextEdit>
#include <QPushButton>
#include <QTimer>
#include <QToolButton>
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
TEST_F(QuantityTest, PreparingOrbitPropertiesDoesNotShowTemporaryWindows) {
    WindowShowObserver windows;
    OrbitEditor editor(OrbitInput{});
    QCoreApplication::processEvents();
    EXPECT_FALSE(editor.isVisible());
    EXPECT_TRUE(windows.shown.isEmpty()) << windows.shown.join(", ").toStdString();
}

TEST_F(QuantityTest, BusyHintsEndWithoutErasingOrbitOrModelResults) {
    OrbitEditor editor(OrbitInput{});
    for (const bool appearance : {false, true}) {
        SCOPED_TRACE(appearance ? "model" : "orbit");
        auto *message = editor.findChild<QLabel *>(appearance ? "modelResult" : "orbitResult");
        ASSERT_NE(message, nullptr);
        const auto busy = [&](bool value) {
            if (appearance)
                editor.set_appearance_busy(value);
            else
                editor.set_busy(value);
        };
        const auto result = [&](const QString &text, bool error) {
            if (appearance)
                editor.show_appearance_result(text, error);
            else
                editor.show_result(text, error);
        };
        busy(true);
        EXPECT_EQ(message->text(),
                  appearance ? QStringLiteral("正在准备三维模型…") : QStringLiteral("正在计算…"));
        EXPECT_FALSE(message->isHidden());
        busy(false);
        EXPECT_TRUE(message->text().isEmpty());
        EXPECT_TRUE(message->isHidden());

        // Completion may be published before or after the busy flag is reset.
        // A final success, cancellation or error must survive subsequent idle updates.
        for (const auto &text :
             {QStringLiteral("已应用"), QStringLiteral("已取消"), QStringLiteral("模型文件无效")}) {
            busy(true);
            result(text, text == QStringLiteral("模型文件无效"));
            busy(false);
            busy(false);
            EXPECT_EQ(message->text(), text);
            EXPECT_FALSE(message->isHidden());
        }
        busy(true);
        busy(false);
        EXPECT_TRUE(message->text().isEmpty());
    }
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

TEST_F(QuantityTest, UneditedOrbitPreservesExactStateAcrossRepresentationsAndUnits) {
    OrbitInput input;
    input.propagator = OrbitPropagator::hpop;
    input.initial.position_m = {7000000.123456789, 123.456789123, -987.654321987};
    OrbitEditor editor(input);
    std::optional<OrbitInput> submitted;
    editor.apply = [&](OrbitInput result) { submitted = std::move(result); };
    const auto submit_unchanged = [&] {
        submitted.reset();
        editor.submit();
        ASSERT_TRUE(submitted);
        EXPECT_TRUE(same_orbit_input(*submitted, input));
    };
    submit_unchanged();
    field(editor, "semiMajorAxis").set_unit(Unit::metre);
    submit_unchanged();
    editor.set_state_mode(0);
    submit_unchanged();
    editor.set_state_mode(1);
    submit_unchanged();
    auto &axis = field(editor, "semiMajorAxis");
    axis.set_value(axis.value(Unit::metre) + .001, Unit::metre);
    editor.submit();
    ASSERT_TRUE(submitted);
    EXPECT_FALSE(same_state(submitted->initial, input.initial));
}

TEST_F(QuantityTest, ReturningFromTleKeepsIncompleteManualDraftEditable) {
    OrbitEditor editor(OrbitInput{});
    editor.set_state_mode(0);
    auto &position = field(editor, "positionX");
    text(position)->setText("unfinished");
    editor.set_propagator(OrbitPropagator::sgp4);
    editor.set_propagator(OrbitPropagator::two_body);
    EXPECT_EQ(editor.findChild<QComboBox *>("orbitPropagator")->currentIndex(),
              static_cast<int>(OrbitPropagator::two_body));
    EXPECT_EQ(text(position)->text(), "unfinished");
    std::optional<OrbitInput> submitted;
    editor.apply = [&](OrbitInput value) { submitted = std::move(value); };
    editor.submit();
    EXPECT_FALSE(submitted);
    position.set_value(7100000, Unit::metre);
    editor.submit();
    ASSERT_TRUE(submitted);
    EXPECT_EQ(submitted->initial.position_m[0], 7100000);
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
    editor.findChild<QPushButton *>("applyAppearance")->click();
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
    const auto capture = qEnvironmentVariable("ORBELLE_STYLE_UI_CAPTURE");
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
    const auto eop = runtime::load_eop_snapshot(ORBELLE_TEST_EOP);
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

TEST_F(QuantityTest, RebindingTimeDataPreservesTypedAndIncompleteEpochDrafts) {
    class NewTimeIdentity final : public core::ITimeProvider {
      public:
        std::shared_ptr<const core::ITimeProvider> source = runtime::make_builtin_time_provider();
        core::TimeDataInfo info = source->data_info();
        NewTimeIdentity() { info.sha256 = std::string(64, 'a'); }
        core::Epoch from_utc(const core::UtcDateTime &value) const override {
            return source->from_utc(value);
        }
        core::UtcDateTime to_utc(core::Epoch value) const override { return source->to_utc(value); }
        int tai_minus_utc_seconds(core::Epoch value) const override {
            return source->tai_minus_utc_seconds(value);
        }
        core::JulianDate julian_date(core::Epoch value, core::TimeScale scale) const override {
            return source->julian_date(value, scale);
        }
        const core::TimeDataInfo &data_info() const noexcept override { return info; }
    };
    std::shared_ptr<const core::ITimeProvider> original = runtime::make_builtin_time_provider();
    auto replacement = std::make_shared<NewTimeIdentity>();
    EpochEditor editor(nullptr, original);
    auto *text = editor.findChild<QLineEdit *>("epochValue");
    text->setText("2026-10-09 12:34:56.123456789 TT");
    const auto expected = editor.epoch();
    editor.set_time_provider(replacement);
    EXPECT_EQ(editor.epoch(), expected);
    EXPECT_EQ(text->text(), "2026-10-09 12:34:56.123456789 TT");
    text->setText("2026-10-");
    editor.set_time_provider(original);
    EXPECT_EQ(text->text(), "2026-10-");
    EXPECT_THROW((void)editor.epoch(), std::invalid_argument);
    editor.set_epoch(expected);
    editor.set_time_provider(replacement);
    EXPECT_EQ(editor.epoch(), expected);
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
    const auto capture = qEnvironmentVariable("ORBELLE_PROPAGATOR_UI_CAPTURE");
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
} // namespace orbelle::desktop
