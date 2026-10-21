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
TEST_F(QuantityTest, HpopUsesInstalledGravityLimitsAndPersistsSelectedModel) {
    apply_workbench_style(*qobject_cast<QApplication *>(QApplication::instance()));
    core::HpopOptions options;
    runtime::CatalogGravity entry;
    entry.selection = "egm2008";
    entry.info = {"EGM2008", {}, {}, "tide_free", core::egm96_mu, core::egm96_radius_m, 2190, 2159};
    QTimer::singleShot(0, [&] {
        auto *dialog = qobject_cast<QDialog *>(QApplication::activeModalWidget());
        ASSERT_TRUE(dialog);
        auto *model = dialog->findChild<QComboBox *>("hpopGravityModel");
        model->setCurrentIndex(model->findData(QStringLiteral("egm2008")));
        auto *degree = dialog->findChild<QSpinBox *>("hpopGravityDegree");
        auto *order = dialog->findChild<QSpinBox *>("hpopGravityOrder");
        EXPECT_EQ(degree->maximum(), 2190);
        degree->setValue(2190);
        EXPECT_EQ(order->maximum(), 2159);
        order->setValue(2190);
        EXPECT_EQ(order->value(), 2159);
        degree->setValue(720);
        EXPECT_EQ(order->maximum(), 720);
        EXPECT_EQ(order->value(), 720);
        degree->setValue(2190);
        order->setValue(2159);
        degree->setFocus();
        QKeyEvent down(QEvent::KeyPress, Qt::Key_Down, Qt::NoModifier);
        QApplication::sendEvent(degree, &down);
        EXPECT_EQ(degree->value(), 2189);
        QKeyEvent up(QEvent::KeyPress, Qt::Key_Up, Qt::NoModifier);
        QApplication::sendEvent(degree, &up);
        EXPECT_EQ(degree->value(), 2190);
        const auto output = qEnvironmentVariable("ORBITFORGE_HPOP_UI_CAPTURE");
        if (!output.isEmpty()) {
            QDir{}.mkpath(output);
            for (const int width : {720, 430}) {
                dialog->resize(width, 700);
                QEventLoop settled;
                QTimer::singleShot(60, &settled, &QEventLoop::quit);
                settled.exec();
                EXPECT_EQ(degree->value(), 2190);
                EXPECT_EQ(order->value(), 2159);
                EXPECT_TRUE(
                    dialog->grab().save(output + QString("/full-gravity-%1.png").arg(width)));
                std::cout << "Full gravity UI width=" << dialog->width()
                          << " DPR=" << dialog->devicePixelRatioF() << '\n';
            }
        }
        dialog->findChild<QDialogButtonBox *>()->button(QDialogButtonBox::Ok)->click();
    });
    EXPECT_TRUE(edit_hpop_settings(options, nullptr, {entry}));
    EXPECT_EQ(options.gravity_model, "egm2008");
    EXPECT_EQ(options.gravity_degree, 2190);
    EXPECT_EQ(options.gravity_order, 2159);
    const auto saved = options;
    QTimer::singleShot(0, [&] {
        auto *dialog = qobject_cast<QDialog *>(QApplication::activeModalWidget());
        ASSERT_TRUE(dialog);
        EXPECT_EQ(dialog->findChild<QSpinBox *>("hpopGravityDegree")->value(), 2190);
        EXPECT_EQ(dialog->findChild<QSpinBox *>("hpopGravityOrder")->value(), 2159);
        dialog->findChild<QSpinBox *>("hpopGravityDegree")->setValue(720);
        dialog->reject();
    });
    EXPECT_FALSE(edit_hpop_settings(options, nullptr, {entry}));
    EXPECT_EQ(options, saved);
    QTimer::singleShot(0, [&] {
        auto *dialog = qobject_cast<QDialog *>(QApplication::activeModalWidget());
        ASSERT_TRUE(dialog);
        dialog->findChild<QDialogButtonBox *>()->button(QDialogButtonBox::Ok)->click();
        EXPECT_TRUE(dialog->isVisible());
        EXPECT_TRUE(dialog->findChild<QLabel *>("errorText")->isVisible());
        dialog->reject();
    });
    EXPECT_FALSE(edit_hpop_settings(options, nullptr));
    EXPECT_EQ(options, saved);
}

TEST_F(QuantityTest, HpopSolidTidesModesTruncationResizeReopenAndCancel) {
    apply_workbench_style(*qobject_cast<QApplication *>(QApplication::instance()));
    core::HpopOptions options;
    runtime::CatalogEphemeris entry;
    entry.selection = "de440s";
    entry.info.first = core::Epoch{0};
    entry.info.last = core::Epoch{86400000000000};
    EXPECT_EQ(new_hpop_options().solid_tides.mode, core::SolidTideMode::permanent);
    QTimer::singleShot(0, [&] {
        auto *dialog = qobject_cast<QDialog *>(QApplication::activeModalWidget());
        ASSERT_TRUE(dialog);
        auto *mode = dialog->findChild<QComboBox *>("hpopSolidTideMode");
        auto *truncate = dialog->findChild<QCheckBox *>("hpopSolidTideTruncate");
        auto *time = dialog->findChild<QCheckBox *>("hpopSolidTideTimeDependent");
        ASSERT_TRUE(time);
        ASSERT_TRUE(mode);
        ASSERT_TRUE(truncate);
        EXPECT_FALSE(time->isEnabled());
        EXPECT_FALSE(time->isChecked());
        EXPECT_EQ(mode->count(), 3);
        EXPECT_EQ(mode->currentIndex(), 0);
        EXPECT_FALSE(truncate->isEnabled());
        reveal(mode);
        mode->setFocus();
        QKeyEvent next(QEvent::KeyPress, Qt::Key_Down, Qt::NoModifier);
        QApplication::sendEvent(mode, &next);
        EXPECT_EQ(mode->currentIndex(), 1);
        EXPECT_TRUE(truncate->isEnabled());
        EXPECT_FALSE(time->isEnabled());
        mode->setCurrentIndex(2);
        EXPECT_TRUE(time->isEnabled());
        time->setChecked(true);
        truncate->setChecked(false);
        const auto capture = qEnvironmentVariable("ORBITFORGE_HPOP_UI_CAPTURE");
        std::cout << "HPOP solid tide device pixel ratio: " << dialog->devicePixelRatioF() << '\n';
        for (int width : {720, 430}) {
            dialog->resize(width, width == 720 ? 700 : 620);
            QEventLoop settled;
            QTimer::singleShot(60, &settled, &QEventLoop::quit);
            settled.exec();
            auto *scroll = dialog->findChild<QScrollArea *>("hpopGravityPage");
            scroll->ensureWidgetVisible(time);
            EXPECT_TRUE(time->isChecked());
            QApplication::processEvents();
            EXPECT_EQ(mode, dialog->findChild<QComboBox *>("hpopSolidTideMode"));
            EXPECT_EQ(mode->currentIndex(), 2);
            EXPECT_FALSE(truncate->isChecked());
            EXPECT_TRUE(mode->isVisible());
            auto *buttons = dialog->findChild<QDialogButtonBox *>();
            EXPECT_TRUE(dialog->rect().contains(buttons->geometry()));
            EXPECT_EQ(scroll->horizontalScrollBar()->maximum(), 0);
            if (!capture.isEmpty()) {
                EXPECT_TRUE(QDir{}.mkpath(capture));
                EXPECT_TRUE(
                    dialog->grab().save(capture + QString("/hpop-solid-tides-%1.png").arg(width)));
            }
        }
        mode->setCurrentIndex(0);
        EXPECT_FALSE(time->isEnabled());
        EXPECT_TRUE(time->isChecked());
        EXPECT_FALSE(truncate->isEnabled());
        EXPECT_FALSE(truncate->isChecked());
        mode->setCurrentIndex(2);
        dialog->findChild<QDialogButtonBox *>()->button(QDialogButtonBox::Ok)->click();
    });
    EXPECT_TRUE(edit_hpop_settings(options, nullptr, {}, {entry}));
    EXPECT_EQ(options.solid_tides.mode, core::SolidTideMode::full);
    EXPECT_FALSE(options.solid_tides.truncate_to_gravity);
    EXPECT_TRUE(options.solid_tides.include_time_dependent_terms);
    const auto saved = options;
    QTimer::singleShot(0, [&] {
        auto *dialog = qobject_cast<QDialog *>(QApplication::activeModalWidget());
        ASSERT_TRUE(dialog);
        auto *mode = dialog->findChild<QComboBox *>("hpopSolidTideMode");
        auto *truncate = dialog->findChild<QCheckBox *>("hpopSolidTideTruncate");
        auto *time = dialog->findChild<QCheckBox *>("hpopSolidTideTimeDependent");
        ASSERT_TRUE(time);
        EXPECT_EQ(mode->currentIndex(), 2);
        EXPECT_FALSE(truncate->isChecked());
        EXPECT_TRUE(time->isChecked());
        mode->setCurrentIndex(1);
        EXPECT_FALSE(time->isEnabled());
        time->setChecked(false);
        truncate->setChecked(true);
        dialog->reject();
    });
    EXPECT_FALSE(edit_hpop_settings(options, nullptr, {}, {entry}));
    EXPECT_EQ(options, saved);
}

TEST_F(QuantityTest, HpopSolidTidesMissingEphemerisReturnsToGravityAndTruncatedPointMassNeedsNone) {
    core::HpopOptions options;
    options.gravity_degree = options.gravity_order = 0;
    const auto saved = options;
    QTimer::singleShot(0, [&] {
        auto *dialog = qobject_cast<QDialog *>(QApplication::activeModalWidget());
        ASSERT_TRUE(dialog);
        auto *mode = dialog->findChild<QComboBox *>("hpopSolidTideMode");
        auto *truncate = dialog->findChild<QCheckBox *>("hpopSolidTideTruncate");
        auto *time = dialog->findChild<QCheckBox *>("hpopSolidTideTimeDependent");
        ASSERT_TRUE(time);
        EXPECT_FALSE(time->isEnabled());
        mode->setCurrentIndex(2);
        EXPECT_TRUE(time->isEnabled());
        time->setChecked(true);
        truncate->setChecked(false);
        auto *outer = dialog->findChild<QTabWidget *>("hpopSettingsTabs");
        auto *forces = dialog->findChild<QTabWidget *>("hpopForceModelTabs");
        outer->setCurrentIndex(2);
        auto *accept = dialog->findChild<QDialogButtonBox *>()->button(QDialogButtonBox::Ok);
        accept->click();
        EXPECT_TRUE(dialog->isVisible());
        EXPECT_EQ(options, saved);
        EXPECT_EQ(outer->currentIndex(), 0);
        EXPECT_EQ(forces->currentIndex(), 0);
        EXPECT_TRUE(
            dialog->findChild<QLabel *>("errorText")->text().contains(QStringLiteral("固体潮")));
        truncate->setChecked(true);
        accept->click();
    });
    EXPECT_TRUE(edit_hpop_settings(options, nullptr));
    EXPECT_EQ(options.solid_tides.mode, core::SolidTideMode::full);
    EXPECT_TRUE(options.solid_tides.truncate_to_gravity);
    EXPECT_FALSE(options.uses_full_solid_tides());
    EXPECT_TRUE(options.solid_tides.include_time_dependent_terms);
    EXPECT_FALSE(options.uses_utc_force());
}

TEST_F(QuantityTest, HpopLoveNumberFollowsGravityFileAndCancelPreservesSelection) {
    core::HpopOptions options;
    runtime::CatalogGravity custom;
    custom.selection = "custom";
    custom.info = {"Custom", {}, {}, "tide_free", core::egm96_mu, core::egm96_radius_m, 21, 21, .3};
    const auto original = options;
    QTimer::singleShot(0, [&] {
        auto *dialog = qobject_cast<QDialog *>(QApplication::activeModalWidget());
        ASSERT_TRUE(dialog);
        auto *love = dialog->findChild<QLabel *>("hpopSolidTideK20");
        auto *model = dialog->findChild<QComboBox *>("hpopGravityModel");
        ASSERT_TRUE(love);
        ASSERT_TRUE(model);
        EXPECT_DOUBLE_EQ(love->text().toDouble(), .30190);
        model->setCurrentIndex(model->findData(QStringLiteral("custom")));
        EXPECT_DOUBLE_EQ(love->text().toDouble(), .3);
        auto *mode = dialog->findChild<QComboBox *>("hpopSolidTideMode");
        mode->setCurrentIndex(1);
        dialog->resize(430, 620);
        QApplication::processEvents();
        EXPECT_EQ(love, dialog->findChild<QLabel *>("hpopSolidTideK20"));
        EXPECT_DOUBLE_EQ(love->text().toDouble(), .3);
        dialog->reject();
    });
    EXPECT_FALSE(edit_hpop_settings(options, nullptr, {custom}, {}));
    EXPECT_EQ(options, original);
    options.gravity_model = "custom";
    QTimer::singleShot(0, [&] {
        auto *dialog = qobject_cast<QDialog *>(QApplication::activeModalWidget());
        ASSERT_TRUE(dialog);
        EXPECT_DOUBLE_EQ(dialog->findChild<QLabel *>("hpopSolidTideK20")->text().toDouble(), .3);
        dialog->findChild<QComboBox *>("hpopSolidTideMode")->setCurrentIndex(1);
        dialog->findChild<QDialogButtonBox *>()->button(QDialogButtonBox::Ok)->click();
    });
    EXPECT_TRUE(edit_hpop_settings(options, nullptr, {custom}, {}));
    EXPECT_EQ(options.gravity_model, "custom");
    EXPECT_EQ(options.solid_tides.mode, core::SolidTideMode::permanent);
}

TEST_F(QuantityTest, HpopSecularFileCapabilityKeyboardDraftAndValidation) {
    apply_workbench_style(*qobject_cast<QApplication *>(QApplication::instance()));
    core::HpopOptions options;
    runtime::CatalogGravity custom;
    custom.selection = "custom";
    custom.info = {"Secular", {}, {}, "tide_free", core::egm96_mu, core::egm96_radius_m, 8, 4};
    custom.info.secular_variations = core::GravitySecularVariations{
        core::Epoch{-32184000000LL}, {{2, 0, 1.162755e-11, 0}, {2, 1, -.337e-11, 1.606e-11}}};
    QTimer::singleShot(0, [&] {
        auto *dialog = qobject_cast<QDialog *>(QApplication::activeModalWidget());
        ASSERT_TRUE(dialog);
        auto *use = dialog->findChild<QCheckBox *>("hpopSecularVariations");
        auto *model = dialog->findChild<QComboBox *>("hpopGravityModel");
        auto *status = dialog->findChild<QLabel *>("hpopSecularStatus");
        ASSERT_TRUE(use);
        ASSERT_TRUE(model);
        ASSERT_TRUE(status);
        EXPECT_FALSE(use->isEnabled());
        model->setCurrentIndex(model->findData(QStringLiteral("custom")));
        EXPECT_TRUE(use->isEnabled());
        EXPECT_TRUE(status->text().contains("2451545.000000000"));
        use->setFocus();
        QKeyEvent press(QEvent::KeyPress, Qt::Key_Space, Qt::NoModifier);
        QKeyEvent release(QEvent::KeyRelease, Qt::Key_Space, Qt::NoModifier);
        QApplication::sendEvent(use, &press);
        QApplication::sendEvent(use, &release);
        EXPECT_TRUE(use->isChecked());
        std::cout << "HPOP secular device pixel ratio: " << dialog->devicePixelRatioF() << '\n';
        for (int width : {720, 430}) {
            dialog->resize(width, width == 720 ? 700 : 620);
            QEventLoop settled;
            QTimer::singleShot(60, &settled, &QEventLoop::quit);
            settled.exec();
            EXPECT_EQ(use, dialog->findChild<QCheckBox *>("hpopSecularVariations"));
            EXPECT_TRUE(use->isChecked());
            auto *forces = dialog->findChild<QTabWidget *>("hpopForceModelTabs");
            auto *scroll = qobject_cast<QScrollArea *>(forces->currentWidget());
            ASSERT_TRUE(scroll);
            EXPECT_EQ(scroll->horizontalScrollBar()->maximum(), 0);
            EXPECT_TRUE(
                dialog->rect().contains(dialog->findChild<QDialogButtonBox *>()->geometry()));
            const auto capture = qEnvironmentVariable("ORBITFORGE_HPOP_UI_CAPTURE");
            if (!capture.isEmpty()) {
                EXPECT_TRUE(QDir{}.mkpath(capture));
                EXPECT_TRUE(
                    dialog->grab().save(capture + QString("/hpop-secular-%1.png").arg(width)));
            }
        }
        dialog->findChild<QDialogButtonBox *>()->button(QDialogButtonBox::Ok)->click();
    });
    EXPECT_TRUE(edit_hpop_settings(options, nullptr, {custom}, {}));
    EXPECT_TRUE(options.include_secular_variations);
    EXPECT_EQ(options.gravity_model, "custom");
    const auto saved = options;
    QTimer::singleShot(0, [&] {
        auto *dialog = qobject_cast<QDialog *>(QApplication::activeModalWidget());
        ASSERT_TRUE(dialog);
        auto *use = dialog->findChild<QCheckBox *>("hpopSecularVariations");
        auto *model = dialog->findChild<QComboBox *>("hpopGravityModel");
        EXPECT_TRUE(use->isChecked());
        model->setCurrentIndex(0);
        EXPECT_TRUE(use->isEnabled()); // A stale checked draft can always be switched off.
        dialog->findChild<QTabWidget *>("hpopSettingsTabs")->setCurrentIndex(2);
        dialog->findChild<QDialogButtonBox *>()->button(QDialogButtonBox::Ok)->click();
        EXPECT_TRUE(dialog->isVisible());
        EXPECT_FALSE(dialog->findChild<QLabel *>("errorText")->text().isEmpty());
        EXPECT_EQ(dialog->findChild<QTabWidget *>("hpopSettingsTabs")->currentIndex(), 0);
        EXPECT_EQ(options, saved);
        use->setChecked(false);
        EXPECT_FALSE(use->isEnabled());
        model->setCurrentIndex(model->findData(QStringLiteral("custom")));
        EXPECT_TRUE(use->isEnabled());
        dialog->reject();
    });
    EXPECT_FALSE(edit_hpop_settings(options, nullptr, {custom}, {}));
    EXPECT_EQ(options, saved);
    QTimer::singleShot(0, [&] {
        auto *dialog = qobject_cast<QDialog *>(QApplication::activeModalWidget());
        ASSERT_TRUE(dialog);
        dialog->findChild<QComboBox *>("hpopGravityModel")->setCurrentIndex(1);
        auto *use = dialog->findChild<QCheckBox *>("hpopSecularVariations");
        EXPECT_FALSE(use->isEnabled());
        EXPECT_TRUE(use->isChecked());
        dialog->findChild<QDialogButtonBox *>()->button(QDialogButtonBox::Ok)->click();
    });
    EXPECT_TRUE(edit_hpop_settings(options, nullptr, {custom}, {}));
    EXPECT_EQ(options.gravity_degree, 0);
    EXPECT_TRUE(options.include_secular_variations);
}
} // namespace orbitforge::desktop
