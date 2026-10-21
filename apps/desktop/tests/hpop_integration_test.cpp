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
#include <QDialog>
#include <QDialogButtonBox>
#include <QDir>
#include <QEventLoop>
#include <QKeyEvent>
#include <QLabel>
#include <QLineEdit>
#include <QListWidget>
#include <QScrollArea>
#include <QScrollBar>
#include <QSpinBox>
#include <QTabWidget>
#include <QTimer>
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
TEST_F(QuantityTest, HpopAdaptiveIntegratorsValidatePersistAndPreserveCancelledDrafts) {
    apply_workbench_style(*qobject_cast<QApplication *>(QApplication::instance()));
    for (const auto integrator : {core::HpopIntegrator::rkf78, core::HpopIntegrator::rkv89,
                                  core::HpopIntegrator::bulirsch_stoer}) {
        const int method_index = static_cast<int>(integrator);
        const auto label = method_index == 0   ? QStringLiteral("RKF 7(8)")
                           : method_index == 2 ? QStringLiteral("RKV 8(9) Efficient")
                                               : QStringLiteral("Bulirsch–Stoer");
        core::HpopOptions options;
        options.gravity_degree = options.gravity_order = 0;
        options.fixed_step_seconds = 12;
        QTimer::singleShot(0, [&] {
            auto *dialog = qobject_cast<QDialog *>(QApplication::activeModalWidget());
            ASSERT_TRUE(dialog);
            dialog->findChild<QTabWidget *>("hpopSettingsTabs")->setCurrentIndex(1);
            auto *method = dialog->findChild<QComboBox *>("hpopIntegrator");
            auto *minimum = dialog->findChild<QuantityEditor *>("hpopMinStep");
            auto *maximum = dialog->findChild<QuantityEditor *>("hpopMaxStep");
            auto *fixed = dialog->findChild<QuantityEditor *>("hpopFixedStep");
            ASSERT_EQ(method->count(), 5);
            method->setCurrentIndex(1);
            EXPECT_FALSE(maximum->isVisible());
            EXPECT_TRUE(fixed->isVisible());
            method->setFocus();
            QKeyEvent end_key(QEvent::KeyPress, Qt::Key_End, Qt::NoModifier);
            QApplication::sendEvent(method, &end_key);
            for (int index = 4; index > method_index; --index) {
                QKeyEvent up_key(QEvent::KeyPress, Qt::Key_Up, Qt::NoModifier);
                QApplication::sendEvent(method, &up_key);
            }
            EXPECT_EQ(method->currentIndex(), method_index);
            EXPECT_TRUE(method->currentText().contains(label));
            auto *control = dialog->findChild<QComboBox *>("hpopStepControl");
            ASSERT_TRUE(control);
            EXPECT_TRUE(control->isEnabled());
            control->setCurrentIndex(1);
            EXPECT_TRUE(fixed->isVisible());
            EXPECT_FALSE(maximum->isVisible());
            method->setCurrentIndex(4);
            EXPECT_FALSE(control->isEnabled());
            EXPECT_EQ(control->currentIndex(), 1);
            method->setCurrentIndex(method_index);
            EXPECT_TRUE(control->isEnabled());
            EXPECT_EQ(control->currentIndex(), 1);
            control->setCurrentIndex(0);
            method->setCurrentIndex(1);
            EXPECT_FALSE(control->isEnabled());
            EXPECT_EQ(control->currentIndex(), 1);
            method->setCurrentIndex(method_index);
            EXPECT_EQ(control->currentIndex(), 0);
            EXPECT_TRUE(maximum->isVisible());
            EXPECT_FALSE(fixed->isVisible());
            minimum->set_value(50, Unit::second);
            maximum->set_value(10, Unit::second);
            dialog->findChild<QDialogButtonBox *>()->button(QDialogButtonBox::Ok)->click();
            EXPECT_TRUE(dialog->isVisible());
            EXPECT_TRUE(dialog->findChild<QLabel *>("errorText")->isVisible());
            minimum->set_value(.01, Unit::second);
            text(*maximum)->setText("86401");
            dialog->findChild<QDialogButtonBox *>()->button(QDialogButtonBox::Ok)->click();
            EXPECT_TRUE(dialog->isVisible());
            EXPECT_TRUE(dialog->findChild<QLabel *>("errorText")->isVisible());
            maximum->set_value(86400, Unit::second);
            dialog->findChild<QDialogButtonBox *>()->button(QDialogButtonBox::Ok)->click();
        });
        ASSERT_TRUE(edit_hpop_settings(options, nullptr));
        EXPECT_EQ(options.integrator, integrator);
        EXPECT_EQ(options.min_step_seconds, .01);
        EXPECT_EQ(options.max_step_seconds, 86400);
        EXPECT_EQ(options.fixed_step_seconds, 12);
        OrbitInput input;
        input.propagator = OrbitPropagator::hpop;
        input.hpop = options;
        OrbitEditor editor(input);
        bool summary_matches = false;
        for (const auto *text_label : editor.findChildren<QLabel *>())
            summary_matches |= text_label->text().contains(label);
        EXPECT_TRUE(summary_matches);
        const auto saved = options;
        QTimer::singleShot(0, [&] {
            auto *dialog = qobject_cast<QDialog *>(QApplication::activeModalWidget());
            ASSERT_TRUE(dialog);
            EXPECT_EQ(dialog->findChild<QComboBox *>("hpopIntegrator")->currentIndex(),
                      method_index);
            dialog->findChild<QTabWidget *>("hpopSettingsTabs")->setCurrentIndex(1);
            auto *method = dialog->findChild<QComboBox *>("hpopIntegrator");
            auto *maximum = dialog->findChild<QuantityEditor *>("hpopMaxStep");
            const auto capture = qEnvironmentVariable("ORBITFORGE_HPOP_UI_CAPTURE");
            std::cout << "HPOP " << method_index
                      << " device pixel ratio: " << dialog->devicePixelRatioF() << '\n';
            for (const int width : {720, 430}) {
                dialog->resize(width, 620);
                QEventLoop settled;
                QTimer::singleShot(60, &settled, &QEventLoop::quit);
                settled.exec();
                EXPECT_EQ(method, dialog->findChild<QComboBox *>("hpopIntegrator"));
                EXPECT_EQ(maximum->value(Unit::second), 86400);
                auto *scroll = dialog->findChild<QScrollArea *>("hpopIntegratorPage");
                scroll->ensureWidgetVisible(method);
                QApplication::processEvents();
                EXPECT_EQ(scroll->horizontalScrollBar()->maximum(), 0);
                EXPECT_TRUE(
                    dialog->rect().contains(dialog->findChild<QDialogButtonBox *>()->geometry()));
                if (!capture.isEmpty()) {
                    EXPECT_TRUE(QDir{}.mkpath(capture));
                    EXPECT_TRUE(dialog->grab().save(
                        capture +
                        QString("/hpop-adaptive-%1-%2.png").arg(method_index).arg(width)));
                }
            }
            dialog->findChild<QComboBox *>("hpopIntegrator")->setCurrentIndex(1);
            dialog->findChild<QuantityEditor *>("hpopFixedStep")->set_value(7, Unit::second);
            dialog->reject();
        });
        EXPECT_FALSE(edit_hpop_settings(options, nullptr));
        EXPECT_EQ(options, saved);
    }
}

TEST_F(QuantityTest, HpopFixedStepControlAppliesAndRestoresDrafts) {
    apply_workbench_style(*qobject_cast<QApplication *>(QApplication::instance()));
    for (const auto method_kind : {core::HpopIntegrator::rkf78, core::HpopIntegrator::rkv89,
                                   core::HpopIntegrator::bulirsch_stoer}) {
        core::HpopOptions options;
        options.gravity_degree = options.gravity_order = 0;
        options.integrator = method_kind;
        const int method_index = static_cast<int>(method_kind);
        QTimer::singleShot(0, [&] {
            auto *dialog = qobject_cast<QDialog *>(QApplication::activeModalWidget());
            ASSERT_TRUE(dialog);
            dialog->findChild<QTabWidget *>("hpopSettingsTabs")->setCurrentIndex(1);
            auto *control = dialog->findChild<QComboBox *>("hpopStepControl");
            control->setCurrentIndex(1);
            auto *method = dialog->findChild<QComboBox *>("hpopIntegrator");
            method->setCurrentIndex(4);
            EXPECT_FALSE(control->isEnabled());
            method->setCurrentIndex(method_index);
            EXPECT_TRUE(control->isEnabled());
            EXPECT_EQ(control->currentIndex(), 1);
            auto *fixed = dialog->findChild<QuantityEditor *>("hpopFixedStep");
            fixed->set_value(7.1, Unit::second);
            EXPECT_TRUE(fixed->isVisible());
            auto *maximum = dialog->findChild<QuantityEditor *>("hpopMaxStep");
            EXPECT_FALSE(maximum->isVisible());
            dialog->findChild<QuantityEditor *>("hpopMinStep")->set_value(50, Unit::second);
            maximum->set_value(10, Unit::second);
            dialog->findChild<QDialogButtonBox *>()->button(QDialogButtonBox::Ok)->click();
        });
        ASSERT_TRUE(edit_hpop_settings(options, nullptr));
        EXPECT_EQ(options.integrator, method_kind);
        EXPECT_EQ(options.step_control, core::HpopStepControl::fixed);
        EXPECT_EQ(options.fixed_step_seconds, 7.1);
        EXPECT_EQ(options.max_step_seconds, 60);
        const auto saved = options;
        QTimer::singleShot(0, [&] {
            auto *dialog = qobject_cast<QDialog *>(QApplication::activeModalWidget());
            ASSERT_TRUE(dialog);
            dialog->findChild<QTabWidget *>("hpopSettingsTabs")->setCurrentIndex(1);
            auto *control = dialog->findChild<QComboBox *>("hpopStepControl");
            EXPECT_EQ(control->currentIndex(), 1);
            const auto capture = qEnvironmentVariable("ORBITFORGE_HPOP_UI_CAPTURE");
            for (int width : {430, 720}) {
                dialog->resize(width, 620);
                QEventLoop settled;
                QTimer::singleShot(60, &settled, &QEventLoop::quit);
                settled.exec();
                auto *scroll = dialog->findChild<QScrollArea *>("hpopIntegratorPage");
                scroll->ensureWidgetVisible(control);
                QApplication::processEvents();
                EXPECT_EQ(scroll->horizontalScrollBar()->maximum(), 0);
                EXPECT_TRUE(
                    dialog->rect().contains(dialog->findChild<QDialogButtonBox *>()->geometry()));
                if (!capture.isEmpty()) {
                    EXPECT_TRUE(QDir{}.mkpath(capture));
                    EXPECT_TRUE(dialog->grab().save(
                        capture + QString("/hpop-fixed-%1-%2.png").arg(method_index).arg(width)));
                }
            }
            control->setCurrentIndex(0);
            dialog->reject();
        });
        EXPECT_FALSE(edit_hpop_settings(options, nullptr));
        EXPECT_EQ(options, saved);
    }
}

TEST_F(QuantityTest, HpopGaussJacksonCorrectionControlsValidatePersistAndCancel) {
    apply_workbench_style(*qobject_cast<QApplication *>(QApplication::instance()));
    for (int scheme : {0, 1}) {
        core::HpopOptions options;
        options.gravity_degree = options.gravity_order = 0;
        QTimer::singleShot(0, [&] {
            auto *dialog = qobject_cast<QDialog *>(QApplication::activeModalWidget());
            ASSERT_TRUE(dialog);
            dialog->findChild<QTabWidget *>("hpopSettingsTabs")->setCurrentIndex(1);
            auto *method = dialog->findChild<QComboBox *>("hpopIntegrator");
            auto *correction = dialog->findChild<QComboBox *>("hpopCorrection");
            auto *fixed = dialog->findChild<QuantityEditor *>("hpopFixedStep");
            auto *maximum = dialog->findChild<QuantityEditor *>("hpopMaxStep");
            ASSERT_TRUE(correction);
            EXPECT_FALSE(correction->isVisible());
            method->setFocus();
            QKeyEvent end_key(QEvent::KeyPress, Qt::Key_End, Qt::NoModifier);
            QApplication::sendEvent(method, &end_key);
            EXPECT_EQ(method->currentIndex(), 4);
            EXPECT_FALSE(maximum->isVisible());
            EXPECT_TRUE(fixed->isVisible());
            EXPECT_TRUE(correction->isVisible());
            correction->setCurrentIndex(scheme);
            fixed->findChild<QLineEdit *>("quantityValue")->setText("0");
            dialog->findChild<QDialogButtonBox *>()->button(QDialogButtonBox::Ok)->click();
            EXPECT_TRUE(dialog->isVisible());
            EXPECT_TRUE(dialog->findChild<QLabel *>("errorText")->isVisible());
            fixed->set_value(30, Unit::second);
            method->setCurrentIndex(0);
            EXPECT_FALSE(correction->isVisible());
            method->setCurrentIndex(4);
            EXPECT_EQ(correction->currentIndex(), scheme);
            EXPECT_EQ(fixed->value(Unit::second), 30);
            dialog->findChild<QDialogButtonBox *>()->button(QDialogButtonBox::Ok)->click();
        });
        ASSERT_TRUE(edit_hpop_settings(options, nullptr));
        EXPECT_EQ(options.integrator, core::HpopIntegrator::gauss_jackson);
        EXPECT_EQ(options.correction, static_cast<core::HpopCorrection>(scheme));
        EXPECT_EQ(options.fixed_step_seconds, 30);
        const auto saved = options;
        OrbitInput input;
        input.propagator = OrbitPropagator::hpop;
        input.hpop = options;
        OrbitEditor editor(input);
        bool summary_matches = false;
        for (const auto *text_label : editor.findChildren<QLabel *>())
            summary_matches |= text_label->text().contains("Gauss–Jackson 12");
        EXPECT_TRUE(summary_matches);
        QTimer::singleShot(0, [&] {
            auto *dialog = qobject_cast<QDialog *>(QApplication::activeModalWidget());
            ASSERT_TRUE(dialog);
            dialog->findChild<QTabWidget *>("hpopSettingsTabs")->setCurrentIndex(1);
            auto *correction = dialog->findChild<QComboBox *>("hpopCorrection");
            auto *fixed = dialog->findChild<QuantityEditor *>("hpopFixedStep");
            EXPECT_EQ(correction->currentIndex(), scheme);
            EXPECT_EQ(fixed->value(Unit::second), 30);
            auto *navigation = dialog->findChild<QListWidget *>("hpopNavigation");
            ASSERT_TRUE(navigation);
            bool fixed_summary = false;
            for (int row = 0; row < navigation->count(); ++row) {
                const auto summary = navigation->item(row)->data(Qt::UserRole + 1).toString();
                if (summary.contains("Gauss–Jackson")) {
                    EXPECT_TRUE(summary.contains(QStringLiteral("步长 30 s")));
                    EXPECT_FALSE(summary.contains(QStringLiteral("相对容差")));
                    fixed_summary = true;
                }
            }
            EXPECT_TRUE(fixed_summary);
            const auto capture = qEnvironmentVariable("ORBITFORGE_HPOP_UI_CAPTURE");
            std::cout << "HPOP Gauss-Jackson " << scheme << " DPR " << dialog->devicePixelRatioF()
                      << '\n';
            for (int width : {720, 430}) {
                dialog->resize(width, 620);
                QEventLoop settled;
                QTimer::singleShot(60, &settled, &QEventLoop::quit);
                settled.exec();
                auto *scroll = dialog->findChild<QScrollArea *>("hpopIntegratorPage");
                scroll->ensureWidgetVisible(correction);
                QApplication::processEvents();
                EXPECT_EQ(scroll->horizontalScrollBar()->maximum(), 0);
                EXPECT_EQ(correction->currentIndex(), scheme);
                EXPECT_TRUE(
                    dialog->rect().contains(dialog->findChild<QDialogButtonBox *>()->geometry()));
                if (!capture.isEmpty()) {
                    EXPECT_TRUE(QDir{}.mkpath(capture));
                    EXPECT_TRUE(dialog->grab().save(
                        capture + QString("/hpop-gj-%1-%2.png").arg(scheme).arg(width)));
                }
            }
            correction->setCurrentIndex(1 - scheme);
            fixed->set_value(7, Unit::second);
            dialog->reject();
        });
        EXPECT_FALSE(edit_hpop_settings(options, nullptr));
        EXPECT_EQ(options, saved);
    }
}

TEST_F(QuantityTest, HpopInterpolationValidatesPreservesDraftsAndReopensAtNarrowWidths) {
    apply_workbench_style(*qobject_cast<QApplication *>(QApplication::instance()));
    core::HpopOptions options;
    options.gravity_degree = options.gravity_order = 0;
    QTimer::singleShot(0, [&] {
        auto *dialog = qobject_cast<QDialog *>(QApplication::activeModalWidget());
        ASSERT_TRUE(dialog);
        dialog->findChild<QTabWidget *>("hpopSettingsTabs")->setCurrentIndex(1);
        auto *method = dialog->findChild<QComboBox *>("hpopInterpolation");
        auto *order = dialog->findChild<QSpinBox *>("hpopInterpolationOrder");
        auto *fixed = dialog->findChild<QCheckBox *>("hpopFixedEphemeris");
        ASSERT_TRUE(method);
        ASSERT_TRUE(order);
        ASSERT_TRUE(fixed);
        EXPECT_EQ(method->currentIndex(), 1);
        EXPECT_EQ(order->value(), 7);
        order->setValue(8);
        dialog->findChild<QDialogButtonBox *>()->button(QDialogButtonBox::Ok)->click();
        EXPECT_TRUE(dialog->isVisible());
        EXPECT_TRUE(dialog->findChild<QLabel *>("errorText")->isVisible());
        order->setValue(9);
        method->setCurrentIndex(0);
        order->setValue(6);
        method->setCurrentIndex(1);
        EXPECT_EQ(order->value(), 9);
        method->setCurrentIndex(0);
        EXPECT_EQ(order->value(), 6);
        fixed->setChecked(true);
        dialog->findChild<QDialogButtonBox *>()->button(QDialogButtonBox::Ok)->click();
    });
    ASSERT_TRUE(edit_hpop_settings(options, nullptr));
    EXPECT_EQ(options.interpolation.method, core::OrbitInterpolationMethod::lagrange);
    EXPECT_EQ(options.interpolation.order, 6);
    EXPECT_TRUE(options.report_ephemeris_on_fixed_step);
    const auto saved = options;
    QTimer::singleShot(0, [&] {
        auto *dialog = qobject_cast<QDialog *>(QApplication::activeModalWidget());
        ASSERT_TRUE(dialog);
        dialog->findChild<QTabWidget *>("hpopSettingsTabs")->setCurrentIndex(1);
        auto *method = dialog->findChild<QComboBox *>("hpopInterpolation");
        auto *order = dialog->findChild<QSpinBox *>("hpopInterpolationOrder");
        auto *fixed = dialog->findChild<QCheckBox *>("hpopFixedEphemeris");
        EXPECT_EQ(method->currentIndex(), 0);
        EXPECT_EQ(order->value(), 6);
        EXPECT_TRUE(fixed->isChecked());
        const auto capture = qEnvironmentVariable("ORBITFORGE_HPOP_UI_CAPTURE");
        std::cout << "HPOP interpolation DPR " << dialog->devicePixelRatioF() << '\n';
        for (int selected : {0, 1}) {
            method->setCurrentIndex(selected);
            if (selected == 1) {
                order->setFocus();
                QKeyEvent key(QEvent::KeyPress, Qt::Key_Up, Qt::NoModifier);
                QApplication::sendEvent(order, &key);
                EXPECT_EQ(order->value(), 9);
            }
            for (int width : {430, 720}) {
                dialog->resize(width, 620);
                QEventLoop settled;
                QTimer::singleShot(60, &settled, &QEventLoop::quit);
                settled.exec();
                auto *scroll = dialog->findChild<QScrollArea *>("hpopIntegratorPage");
                scroll->ensureWidgetVisible(order);
                QApplication::processEvents();
                EXPECT_EQ(scroll->horizontalScrollBar()->maximum(), 0);
                EXPECT_EQ(order->value(), selected == 0 ? 6 : 9);
                EXPECT_TRUE(
                    dialog->rect().contains(dialog->findChild<QDialogButtonBox *>()->geometry()));
                if (!capture.isEmpty()) {
                    EXPECT_TRUE(QDir{}.mkpath(capture));
                    EXPECT_TRUE(dialog->grab().save(
                        capture +
                        QString("/hpop-interpolation-%1-%2.png").arg(selected).arg(width)));
                }
            }
        }
        fixed->setChecked(false);
        dialog->reject();
    });
    EXPECT_FALSE(edit_hpop_settings(options, nullptr));
    EXPECT_EQ(options, saved);
}

TEST_F(QuantityTest, HpopRegularizationControlsPreserveDraftsCancelAndNarrowLayouts) {
    apply_workbench_style(*qobject_cast<QApplication *>(QApplication::instance()));
    for (int selected = 0; selected < 5; ++selected) {
        core::HpopOptions options;
        options.gravity_degree = options.gravity_order = 0;
        options.integrator = static_cast<core::HpopIntegrator>(selected);
        options.step_control = core::HpopStepControl::fixed;
        QTimer::singleShot(0, [&] {
            auto *dialog = qobject_cast<QDialog *>(QApplication::activeModalWidget());
            ASSERT_TRUE(dialog);
            dialog->findChild<QTabWidget *>("hpopSettingsTabs")->setCurrentIndex(1);
            auto *enabled = dialog->findChild<QCheckBox *>("hpopTimeRegularization");
            auto *exponent = dialog->findChild<QuantityEditor *>("hpopRegularizationExponent");
            auto *count = dialog->findChild<QSpinBox *>("hpopStepsPerOrbit");
            auto *fixed = dialog->findChild<QuantityEditor *>("hpopFixedStep");
            auto *control = dialog->findChild<QComboBox *>("hpopStepControl");
            ASSERT_TRUE(enabled);
            ASSERT_TRUE(exponent);
            ASSERT_TRUE(count);
            EXPECT_FALSE(enabled->isChecked());
            EXPECT_FALSE(exponent->isEnabled());
            EXPECT_FALSE(count->isEnabled());
            EXPECT_TRUE(fixed->isVisible());
            enabled->setChecked(true);
            EXPECT_TRUE(exponent->isEnabled());
            EXPECT_TRUE(count->isEnabled());
            EXPECT_FALSE(fixed->isVisible());
            exponent->set_value(1.75, Unit::one);
            count->setValue(1024);
            enabled->setChecked(false);
            EXPECT_TRUE(fixed->isVisible());
            enabled->setChecked(true);
            EXPECT_EQ(count->value(), 1024);
            if (control->isEnabled()) {
                control->setCurrentIndex(0);
                EXPECT_TRUE(dialog->findChild<QuantityEditor *>("hpopMaxStep")->isVisible());
                EXPECT_FALSE(fixed->isVisible());
                control->setCurrentIndex(1);
            }
            // An inactive ordinary-step draft must not block a valid regularized edit.
            fixed->findChild<QLineEdit *>()->setText("invalid inactive draft");
            dialog->findChild<QDialogButtonBox *>()->button(QDialogButtonBox::Ok)->click();
        });
        ASSERT_TRUE(edit_hpop_settings(options, nullptr));
        EXPECT_TRUE(options.time_regularization);
        EXPECT_EQ(options.regularization_exponent, 1.75);
        EXPECT_EQ(options.steps_per_orbit, 1024);
        EXPECT_EQ(options.fixed_step_seconds, 10);
        const auto saved = options;
        QTimer::singleShot(0, [&] {
            auto *dialog = qobject_cast<QDialog *>(QApplication::activeModalWidget());
            ASSERT_TRUE(dialog);
            dialog->findChild<QTabWidget *>("hpopSettingsTabs")->setCurrentIndex(1);
            auto *enabled = dialog->findChild<QCheckBox *>("hpopTimeRegularization");
            auto *count = dialog->findChild<QSpinBox *>("hpopStepsPerOrbit");
            EXPECT_TRUE(enabled->isChecked());
            EXPECT_EQ(count->value(), 1024);
            count->setFocus();
            QKeyEvent key(QEvent::KeyPress, Qt::Key_Up, Qt::NoModifier);
            QApplication::sendEvent(count, &key);
            EXPECT_EQ(count->value(), 1025);
            const auto capture = qEnvironmentVariable("ORBITFORGE_HPOP_UI_CAPTURE");
            std::cout << "HPOP regularization DPR " << dialog->devicePixelRatioF() << '\n';
            for (int width : {430, 720}) {
                dialog->resize(width, 620);
                QEventLoop settled;
                QTimer::singleShot(60, &settled, &QEventLoop::quit);
                settled.exec();
                auto *scroll = dialog->findChild<QScrollArea *>("hpopIntegratorPage");
                scroll->ensureWidgetVisible(count);
                QApplication::processEvents();
                EXPECT_EQ(scroll->horizontalScrollBar()->maximum(), 0);
                EXPECT_EQ(count->value(), 1025);
                EXPECT_TRUE(
                    dialog->rect().contains(dialog->findChild<QDialogButtonBox *>()->geometry()));
                if (!capture.isEmpty() && selected == 4) {
                    EXPECT_TRUE(QDir{}.mkpath(capture));
                    EXPECT_TRUE(dialog->grab().save(
                        capture + QString("/hpop-regularization-%1.png").arg(width)));
                }
            }
            enabled->setChecked(false);
            dialog->reject();
        });
        EXPECT_FALSE(edit_hpop_settings(options, nullptr));
        EXPECT_EQ(options, saved);
    }
}

TEST_F(QuantityTest, HpopVopInterpolationMuDraftsUnitsValidationAndNarrowLayouts) {
    apply_workbench_style(*qobject_cast<QApplication *>(QApplication::instance()));
    core::HpopOptions options;
    options.gravity_degree = options.gravity_order = 0;
    for (int pass = 0; pass < 3; ++pass) {
        const auto saved = options;
        QTimer::singleShot(0, [&] {
            auto *dialog = qobject_cast<QDialog *>(QApplication::activeModalWidget());
            ASSERT_TRUE(dialog);
            dialog->findChild<QTabWidget *>("hpopSettingsTabs")->setCurrentIndex(1);
            auto *method = dialog->findChild<QComboBox *>("hpopInterpolation");
            auto *order = dialog->findChild<QSpinBox *>("hpopInterpolationOrder");
            auto *mu = dialog->findChild<QuantityEditor *>("hpopInterpolationMu");
            ASSERT_TRUE(mu);
            if (pass == 0) {
                EXPECT_FALSE(mu->isEnabled());
                method->setCurrentIndex(2);
                EXPECT_TRUE(mu->isEnabled());
                order->setValue(8);
                mu->findChild<QLineEdit *>("quantityValue")->setText("0");
                dialog->findChild<QDialogButtonBox *>()->button(QDialogButtonBox::Ok)->click();
                EXPECT_TRUE(dialog->isVisible());
                EXPECT_TRUE(dialog->findChild<QLabel *>("errorText")->isVisible());
                mu->set_value(4.1e14, Unit::metres_cubed_per_second_squared);
                method->setCurrentIndex(1);
                EXPECT_FALSE(mu->isEnabled());
                EXPECT_EQ(order->value(), 7);
                method->setCurrentIndex(2);
                EXPECT_TRUE(mu->isEnabled());
                EXPECT_EQ(order->value(), 8);
                dialog->findChild<QDialogButtonBox *>()->button(QDialogButtonBox::Ok)->click();
            } else {
                EXPECT_EQ(method->currentIndex(), 2);
                EXPECT_EQ(order->value(), 8);
                EXPECT_DOUBLE_EQ(mu->value(Unit::metres_cubed_per_second_squared), 4.1e14);
                mu->set_unit(Unit::metres_cubed_per_second_squared);
                EXPECT_DOUBLE_EQ(mu->value(Unit::metres_cubed_per_second_squared), 4.1e14);
                mu->set_unit(Unit::kilometres_cubed_per_second_squared);
                for (int width : {430, 720}) {
                    dialog->resize(width, 620);
                    QEventLoop settled;
                    QTimer::singleShot(60, &settled, &QEventLoop::quit);
                    settled.exec();
                    auto *scroll = dialog->findChild<QScrollArea *>("hpopIntegratorPage");
                    scroll->ensureWidgetVisible(mu);
                    QApplication::processEvents();
                    EXPECT_EQ(scroll->horizontalScrollBar()->maximum(), 0);
                    EXPECT_TRUE(dialog->rect().contains(
                        dialog->findChild<QDialogButtonBox *>()->geometry()));
                    const auto capture = qEnvironmentVariable("ORBITFORGE_HPOP_UI_CAPTURE");
                    if (!capture.isEmpty()) {
                        EXPECT_TRUE(QDir{}.mkpath(capture));
                        EXPECT_TRUE(
                            dialog->grab().save(capture + QString("/hpop-vop-%1.png").arg(width)));
                    }
                }
                std::cout << "HPOP VOP DPR " << dialog->devicePixelRatioF() << '\n';
                if (pass == 1) {
                    mu->set_value(4.2e14, Unit::metres_cubed_per_second_squared);
                    dialog->reject();
                } else {
                    method->setCurrentIndex(0);
                    dialog->findChild<QDialogButtonBox *>()->button(QDialogButtonBox::Ok)->click();
                }
            }
        });
        EXPECT_EQ(edit_hpop_settings(options, nullptr), pass != 1);
        if (pass == 1)
            EXPECT_EQ(options, saved);
        else
            EXPECT_DOUBLE_EQ(options.interpolation.vop_mu, 4.1e14);
        if (pass == 0)
            EXPECT_EQ(options.interpolation.method, core::OrbitInterpolationMethod::vop);
        if (pass == 2)
            EXPECT_EQ(options.interpolation.method, core::OrbitInterpolationMethod::lagrange);
    }
}

TEST_F(QuantityTest, HpopVopIntegrationSupportsOnlyRkfBsPreservesDraftAndCancels) {
    apply_workbench_style(*qobject_cast<QApplication *>(QApplication::instance()));
    core::HpopOptions options;
    options.gravity_degree = options.gravity_order = 0;
    for (int pass = 0; pass < 4; ++pass) {
        const auto saved = options;
        QTimer::singleShot(0, [&] {
            auto *dialog = qobject_cast<QDialog *>(QApplication::activeModalWidget());
            ASSERT_TRUE(dialog);
            dialog->findChild<QTabWidget *>("hpopSettingsTabs")->setCurrentIndex(1);
            auto *method = dialog->findChild<QComboBox *>("hpopIntegrator");
            auto *vop = dialog->findChild<QCheckBox *>("hpopVop");
            auto *interpolation = dialog->findChild<QComboBox *>("hpopInterpolation");
            ASSERT_TRUE(vop);
            EXPECT_EQ(interpolation->currentIndex(), 1);
            if (pass == 0) {
                EXPECT_FALSE(vop->isChecked());
                EXPECT_TRUE(vop->isEnabled());
                vop->setFocus();
                QKeyEvent press(QEvent::KeyPress, Qt::Key_Space, Qt::NoModifier);
                QKeyEvent release(QEvent::KeyRelease, Qt::Key_Space, Qt::NoModifier);
                QApplication::sendEvent(vop, &press);
                QApplication::sendEvent(vop, &release);
                EXPECT_TRUE(vop->isChecked());
                for (int selected : {1, 2, 4, 0, 3}) {
                    method->setCurrentIndex(selected);
                    EXPECT_EQ(vop->isEnabled(), selected == 0 || selected == 3);
                    EXPECT_TRUE(vop->isChecked());
                }
            } else {
                EXPECT_TRUE(vop->isChecked());
                EXPECT_EQ(method->currentIndex(), pass == 3 ? 2 : 3);
                EXPECT_EQ(vop->isEnabled(), pass != 3);
                for (int width : {430, 720}) {
                    dialog->resize(width, 620);
                    QEventLoop settled;
                    QTimer::singleShot(60, &settled, &QEventLoop::quit);
                    settled.exec();
                    auto *scroll = dialog->findChild<QScrollArea *>("hpopIntegratorPage");
                    scroll->ensureWidgetVisible(vop);
                    QApplication::processEvents();
                    EXPECT_EQ(scroll->horizontalScrollBar()->maximum(), 0);
                    EXPECT_TRUE(dialog->rect().contains(
                        dialog->findChild<QDialogButtonBox *>()->geometry()));
                    const auto capture = qEnvironmentVariable("ORBITFORGE_HPOP_UI_CAPTURE");
                    if (!capture.isEmpty() && pass == 1) {
                        EXPECT_TRUE(QDir{}.mkpath(capture));
                        EXPECT_TRUE(dialog->grab().save(
                            capture + QString("/hpop-vop-integration-%1.png").arg(width)));
                    }
                }
                RecordProperty("device_pixel_ratio", dialog->devicePixelRatioF());
                if (pass == 1) {
                    vop->setChecked(false);
                    dialog->reject();
                    return;
                }
                method->setCurrentIndex(pass == 2 ? 2 : 0);
            }
            dialog->findChild<QDialogButtonBox *>()->button(QDialogButtonBox::Ok)->click();
        });
        EXPECT_EQ(edit_hpop_settings(options, nullptr), pass != 1);
        if (pass == 1) {
            EXPECT_EQ(options, saved);
        }
        EXPECT_TRUE(options.vop);
        EXPECT_EQ(options.uses_vop(), pass != 2);
        EXPECT_EQ(options.interpolation.method, core::OrbitInterpolationMethod::hermite);
    }
}

TEST_F(QuantityTest, HpopInactiveIntegratorDraftsPersistAcrossAcceptAndReopen) {
    core::HpopOptions options;
    options.gravity_degree = options.gravity_order = 0;
    QTimer::singleShot(0, [&] {
        auto *dialog = qobject_cast<QDialog *>(QApplication::activeModalWidget());
        ASSERT_TRUE(dialog);
        dialog->findChild<QuantityEditor *>("hpopMinStep")->set_value(.25, Unit::second);
        dialog->findChild<QuantityEditor *>("hpopMaxStep")->set_value(120, Unit::second);
        dialog->findChild<QuantityEditor *>("hpopRelativeTolerance")->set_value(1e-10, Unit::one);
        dialog->findChild<QuantityEditor *>("hpopPositionTolerance")->set_value(.02, Unit::metre);
        dialog->findChild<QuantityEditor *>("hpopVelocityTolerance")
            ->set_value(.003, Unit::metres_per_second);
        dialog->findChild<QCheckBox *>("hpopTimeRegularization")->setChecked(true);
        dialog->findChild<QuantityEditor *>("hpopRegularizationExponent")
            ->set_value(1.75, Unit::one);
        dialog->findChild<QSpinBox *>("hpopStepsPerOrbit")->setValue(1024);
        dialog->findChild<QCheckBox *>("hpopTimeRegularization")->setChecked(false);
        dialog->findChild<QComboBox *>("hpopStepControl")->setCurrentIndex(1);
        dialog->findChild<QuantityEditor *>("hpopFixedStep")->set_value(15, Unit::second);
        dialog->findChild<QDialogButtonBox *>()->button(QDialogButtonBox::Ok)->click();
        if (dialog->isVisible())
            dialog->reject();
    });
    ASSERT_TRUE(edit_hpop_settings(options, nullptr));
    EXPECT_DOUBLE_EQ(options.min_step_seconds, .25);
    EXPECT_DOUBLE_EQ(options.max_step_seconds, 120);
    EXPECT_DOUBLE_EQ(options.relative_tolerance, 1e-10);
    EXPECT_DOUBLE_EQ(options.position_tolerance_m, .02);
    EXPECT_DOUBLE_EQ(options.velocity_tolerance_mps, .003);
    EXPECT_DOUBLE_EQ(options.regularization_exponent, 1.75);
    EXPECT_EQ(options.steps_per_orbit, 1024);
    QTimer::singleShot(0, [&] {
        auto *dialog = qobject_cast<QDialog *>(QApplication::activeModalWidget());
        ASSERT_TRUE(dialog);
        EXPECT_DOUBLE_EQ(dialog->findChild<QuantityEditor *>("hpopMaxStep")->value(Unit::second),
                         120);
        dialog->findChild<QuantityEditor *>("hpopFixedStep")->set_value(25, Unit::second);
        dialog->findChild<QComboBox *>("hpopStepControl")->setCurrentIndex(0);
        dialog->findChild<QDialogButtonBox *>()->button(QDialogButtonBox::Ok)->click();
        if (dialog->isVisible())
            dialog->reject();
    });
    ASSERT_TRUE(edit_hpop_settings(options, nullptr));
    EXPECT_DOUBLE_EQ(options.fixed_step_seconds, 25);
}

TEST_F(QuantityTest, HpopInvalidInactiveMuDraftsDoNotBlockAccept) {
    for (const bool third_body : {false, true}) {
        core::HpopOptions options;
        options.gravity_degree = options.gravity_order = 0;
        const auto before = options;
        QTimer::singleShot(0, [&] {
            auto *dialog = qobject_cast<QDialog *>(QApplication::activeModalWidget());
            ASSERT_TRUE(dialog);
            auto *field = dialog->findChild<QuantityEditor *>(third_body ? "hpopSunMu"
                                                                         : "hpopInterpolationMu");
            if (third_body) {
                dialog->findChild<QCheckBox *>("hpopSunGravity")->setChecked(true);
                dialog->findChild<QComboBox *>("hpopSunSource")->setCurrentIndex(2);
            } else
                dialog->findChild<QComboBox *>("hpopInterpolation")->setCurrentIndex(2);
            field->findChild<QLineEdit *>("quantityValue")->setText("unfinished");
            if (third_body)
                dialog->findChild<QCheckBox *>("hpopSunGravity")->setChecked(false);
            else
                dialog->findChild<QComboBox *>("hpopInterpolation")->setCurrentIndex(0);
            dialog->findChild<QDialogButtonBox *>()->button(QDialogButtonBox::Ok)->click();
            if (dialog->isVisible())
                dialog->reject();
        });
        ASSERT_TRUE(edit_hpop_settings(options, nullptr));
        EXPECT_EQ(options.third_body(core::ThirdBody::sun).user_mu,
                  before.third_body(core::ThirdBody::sun).user_mu);
        EXPECT_EQ(options.interpolation.vop_mu, before.interpolation.vop_mu);
    }
}
} // namespace orbitforge::desktop
