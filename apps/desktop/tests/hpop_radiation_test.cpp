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
#include <QPushButton>
#include <QScrollArea>
#include <QScrollBar>
#include <QTabWidget>
#include <QTimer>
#include <QToolButton>
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
TEST_F(QuantityTest, HpopSrpUnitsShadowAssignmentsResizeAndCancelPreserveExactDrafts) {
    apply_workbench_style(*qobject_cast<QApplication *>(QApplication::instance()));
    core::HpopOptions options;
    options.srp.reference_distance_m = 149597870700.12347;
    const auto exact_distance = options.srp.reference_distance_m;
    runtime::CatalogEphemeris entry;
    entry.selection = "de440s";
    entry.info.first = core::Epoch{0};
    entry.info.last = core::Epoch{86400000000000};
    QTimer::singleShot(0, [&] {
        auto *dialog = qobject_cast<QDialog *>(QApplication::activeModalWidget());
        ASSERT_TRUE(dialog);
        auto *outer = dialog->findChild<QTabWidget *>("hpopSettingsTabs");
        auto *forces = dialog->findChild<QTabWidget *>("hpopForceModelTabs");
        forces->setCurrentIndex(2);
        auto *use = dialog->findChild<QCheckBox *>("hpopUseSrp");
        auto *area = dialog->findChild<QuantityEditor *>("hpopSrpAreaMassRatio");
        auto *altitude = dialog->findChild<QuantityEditor *>("hpopSrpAtmosphericAltitude");
        auto *shadow = dialog->findChild<QComboBox *>("hpopSrpShadowModel");
        auto *select_bodies = dialog->findChild<QPushButton *>("hpopSrpSelectBodies");
        ASSERT_TRUE(select_bodies);
        auto *sun = dialog->findChild<QComboBox *>("hpopSrpSunMethod");
        ASSERT_TRUE(sun);
        EXPECT_EQ(sun->count(), 3);
        EXPECT_EQ(sun->currentIndex(), 1);
        use->setChecked(true);
        auto *boundary = dialog->findChild<QCheckBox *>("hpopSrpBoundaryMitigation");
        ASSERT_TRUE(boundary);
        EXPECT_FALSE(boundary->isChecked());
        boundary->setChecked(true);
        EXPECT_TRUE(sun->isVisible());
        sun->setFocus();
        QKeyEvent next(QEvent::KeyPress, Qt::Key_Down, Qt::NoModifier);
        QApplication::sendEvent(sun, &next);
        EXPECT_EQ(sun->currentIndex(), 2);
        area->set_unit(Unit::square_centimetres_per_gram);
        area->findChild<QLineEdit *>("quantityValue")->setText("0.3");
        choose(*area, Unit::square_centimetres_per_kilogram);
        EXPECT_DOUBLE_EQ(area->value(Unit::square_metres_per_kilogram), .03);
        choose(*area, Unit::square_centimetres_per_gram);
        auto *pressure = dialog->findChild<QuantityEditor *>("hpopSrpPressure");
        auto *distance = dialog->findChild<QuantityEditor *>("hpopSrpReferenceDistance");
        ASSERT_TRUE(pressure && distance);
        choose(*pressure, Unit::micropascal);
        text(*pressure)->setText("4.56");
        choose(*distance, Unit::astronomical_unit);
        EXPECT_EQ(distance->value(Unit::metre), exact_distance);
        altitude->set_unit(Unit::kilometre);
        altitude->set_value(23, Unit::kilometre);
        shadow->setCurrentIndex(0);
        EXPECT_FALSE(boundary->isEnabled());
        EXPECT_TRUE(boundary->isChecked());
        EXPECT_FALSE(select_bodies->isEnabled());
        EXPECT_FALSE(altitude->isEnabled());
        shadow->setCurrentIndex(2);
        EXPECT_TRUE(boundary->isEnabled());
        QTimer::singleShot(0, [&] {
            auto *selection = qobject_cast<QDialog *>(QApplication::activeModalWidget());
            ASSERT_TRUE(selection);
            auto *assigned = selection->findChild<QListWidget *>("hpopSrpAssignedBodies");
            ASSERT_TRUE(assigned);
            for (int i = 0; i < assigned->count(); ++i)
                if (assigned->item(i)->data(Qt::UserRole).toInt() == 1)
                    assigned->setCurrentItem(assigned->item(i));
            selection->findChild<QToolButton *>("hpopSrpRemoveBody")->click();
            selection->findChild<QDialogButtonBox *>()->button(QDialogButtonBox::Ok)->click();
        });
        select_bodies->click();
        EXPECT_TRUE(altitude->isEnabled());
        EXPECT_FALSE(dialog->findChild<QuantityEditor *>("hpopSrpMoonRadius")->isEnabled());
        EXPECT_FALSE(dialog->findChild<QLabel *>("hpopSrpEphemerisStatus")
                         ->text()
                         .contains(QStringLiteral("已安装")));
        const auto capture = qEnvironmentVariable("ORBELLE_HPOP_UI_CAPTURE");
        std::cout << "HPOP SRP device pixel ratio: " << dialog->devicePixelRatioF() << '\n';
        for (int width : {720, 430}) {
            dialog->resize(width, width == 720 ? 700 : 620);
            QEventLoop settled;
            QTimer::singleShot(60, &settled, &QEventLoop::quit);
            settled.exec();
            EXPECT_EQ(sun, dialog->findChild<QComboBox *>("hpopSrpSunMethod"));
            EXPECT_EQ(sun->currentIndex(), 2);
            EXPECT_EQ(area, dialog->findChild<QuantityEditor *>("hpopSrpAreaMassRatio"));
            EXPECT_EQ(area->findChild<QLineEdit *>("quantityValue")->text(), "0.3");
            EXPECT_NEAR(area->value(Unit::square_metres_per_kilogram), .03, 1e-16);
            auto *buttons = dialog->findChild<QDialogButtonBox *>();
            EXPECT_TRUE(dialog->rect().contains(buttons->geometry()));
            if (!capture.isEmpty()) {
                EXPECT_TRUE(QDir{}.mkpath(capture));
                EXPECT_TRUE(dialog->grab().save(capture + QString("/hpop-srp-%1.png").arg(width)));
                auto *scroll = dialog->findChild<QScrollArea *>("hpopSrpPage");
                scroll->ensureWidgetVisible(sun);
                QApplication::processEvents();
                EXPECT_TRUE(
                    dialog->grab().save(capture + QString("/hpop-srp-sun-%1.png").arg(width)));
                sun->setCurrentIndex(1);
                QApplication::processEvents();
                EXPECT_TRUE(dialog->grab().save(
                    capture + QString("/hpop-srp-sun-default-%1.png").arg(width)));
                sun->setCurrentIndex(2);
                auto *radius = dialog->findChild<QuantityEditor *>("hpopSrpMoonRadius");
                reveal(radius);
                scroll->ensureWidgetVisible(radius);
                QApplication::processEvents();
                EXPECT_TRUE(
                    dialog->grab().save(capture + QString("/hpop-srp-data-%1.png").arg(width)));
                scroll->verticalScrollBar()->setValue(0);
            }
        }
        outer->setCurrentIndex(1);
        outer->setCurrentIndex(0);
        EXPECT_EQ(forces->currentIndex(), 2);
        dialog->findChild<QDialogButtonBox *>()->button(QDialogButtonBox::Ok)->click();
    });
    EXPECT_TRUE(edit_hpop_settings(options, nullptr, {}, {entry}));
    EXPECT_TRUE(options.srp.enabled);
    EXPECT_TRUE(options.srp.boundary_mitigation);
    EXPECT_EQ(options.srp.sun_position_method, core::SrpSunPositionMethod::apparent);
    EXPECT_NEAR(options.srp.area_mass_ratio_m2_per_kg, .03, 1e-16);
    EXPECT_EQ(options.srp.atmospheric_altitude_m, 23000);
    EXPECT_FALSE(options.srp.moon_shadow);
    EXPECT_EQ(options.srp.reference_distance_m, exact_distance);
    EXPECT_DOUBLE_EQ(options.srp.pressure_at_reference_pa, 4.56e-6);
    const auto saved = options;
    QTimer::singleShot(0, [&] {
        auto *dialog = qobject_cast<QDialog *>(QApplication::activeModalWidget());
        ASSERT_TRUE(dialog);
        EXPECT_TRUE(dialog->findChild<QCheckBox *>("hpopUseSrp")->isChecked());
        auto *sun = dialog->findChild<QComboBox *>("hpopSrpSunMethod");
        EXPECT_EQ(sun->currentIndex(), 2);
        sun->setCurrentIndex(0);
        dialog->findChild<QCheckBox *>("hpopUseSrp")->setChecked(false);
        dialog->findChild<QuantityEditor *>("hpopSrpCr")->set_value(2, Unit::one);
        dialog->reject();
    });
    EXPECT_FALSE(edit_hpop_settings(options, nullptr, {}, {entry}));
    EXPECT_EQ(options, saved);
}

TEST_F(QuantityTest, HpopSrpInvalidFieldAndMissingEphemerisReturnToSrpWithoutCommitting) {
    core::HpopOptions options;
    const auto saved = options;
    QTimer::singleShot(0, [&] {
        auto *dialog = qobject_cast<QDialog *>(QApplication::activeModalWidget());
        ASSERT_TRUE(dialog);
        auto *outer = dialog->findChild<QTabWidget *>("hpopSettingsTabs");
        auto *forces = dialog->findChild<QTabWidget *>("hpopForceModelTabs");
        auto *cr = dialog->findChild<QuantityEditor *>("hpopSrpCr")
                       ->findChild<QLineEdit *>("quantityValue");
        auto *use = dialog->findChild<QCheckBox *>("hpopUseSrp");
        auto *accept = dialog->findChild<QDialogButtonBox *>()->button(QDialogButtonBox::Ok);
        use->setChecked(true);
        cr->setText("101");
        outer->setCurrentIndex(2);
        accept->click();
        EXPECT_EQ(options, saved);
        EXPECT_TRUE(dialog->isVisible());
        EXPECT_EQ(outer->currentIndex(), 0);
        EXPECT_EQ(forces->currentIndex(), 2);
        cr->setText("1.7");
        outer->setCurrentIndex(2);
        accept->click();
        EXPECT_EQ(options, saved);
        EXPECT_EQ(outer->currentIndex(), 0);
        EXPECT_EQ(forces->currentIndex(), 2);
        EXPECT_TRUE(
            dialog->findChild<QLabel *>("errorText")->text().contains(QStringLiteral("星历")));
        use->setChecked(false);
        cr->setText("invalid"); // Inactive invalid text must not destroy the last valid draft.
        accept->click();
    });
    EXPECT_TRUE(edit_hpop_settings(options, nullptr));
    EXPECT_EQ(options, saved);
}

TEST_F(QuantityTest, HpopRelativityKeyboardResizeReopenAndCancel) {
    apply_workbench_style(*qobject_cast<QApplication *>(QApplication::instance()));
    core::HpopOptions options;
    options.gravity_degree = options.gravity_order = 0;
    runtime::CatalogEphemeris entry;
    entry.selection = "de440s";
    entry.info.first = core::Epoch{0};
    entry.info.last = core::Epoch{86400000000000};
    QTimer::singleShot(0, [&] {
        auto *dialog = qobject_cast<QDialog *>(QApplication::activeModalWidget());
        ASSERT_TRUE(dialog);
        auto *outer = dialog->findChild<QTabWidget *>("hpopSettingsTabs");
        auto *forces = dialog->findChild<QTabWidget *>("hpopForceModelTabs");
        auto *relativity = dialog->findChild<QCheckBox *>("hpopRelativisticAccelerations");
        ASSERT_TRUE(relativity);
        forces->setCurrentIndex(3);
        reveal(relativity);
        relativity->setFocus();
        EXPECT_FALSE(relativity->isChecked());
        QKeyEvent press(QEvent::KeyPress, Qt::Key_Space, Qt::NoModifier),
            release(QEvent::KeyRelease, Qt::Key_Space, Qt::NoModifier);
        QApplication::sendEvent(relativity, &press);
        QApplication::sendEvent(relativity, &release);
        EXPECT_TRUE(relativity->isChecked());
        const auto capture = qEnvironmentVariable("ORBELLE_HPOP_UI_CAPTURE");
        std::cout << "HPOP relativity device pixel ratio: " << dialog->devicePixelRatioF() << '\n';
        for (int width : {720, 430}) {
            dialog->resize(width, width == 720 ? 700 : 620);
            QEventLoop settled;
            QTimer::singleShot(60, &settled, &QEventLoop::quit);
            settled.exec();
            auto *scroll = dialog->findChild<QScrollArea *>("hpopAdditionalPage");
            scroll->ensureWidgetVisible(relativity);
            QApplication::processEvents();
            EXPECT_TRUE(relativity->isVisible());
            EXPECT_TRUE(relativity->isChecked());
            EXPECT_EQ(relativity, dialog->findChild<QCheckBox *>("hpopRelativisticAccelerations"));
            EXPECT_EQ(scroll->horizontalScrollBar()->maximum(), 0);
            EXPECT_TRUE(
                dialog->rect().contains(dialog->findChild<QDialogButtonBox *>()->geometry()));
            if (!capture.isEmpty()) {
                EXPECT_TRUE(QDir{}.mkpath(capture));
                EXPECT_TRUE(
                    dialog->grab().save(capture + QString("/hpop-relativity-%1.png").arg(width)));
            }
        }
        outer->setCurrentIndex(1);
        outer->setCurrentIndex(0);
        EXPECT_EQ(forces->currentIndex(), 3);
        EXPECT_TRUE(relativity->isChecked());
        dialog->findChild<QDialogButtonBox *>()->button(QDialogButtonBox::Ok)->click();
    });
    EXPECT_TRUE(edit_hpop_settings(options, nullptr, {}, {entry}));
    EXPECT_TRUE(options.include_relativistic_accelerations);
    const auto saved = options;
    QTimer::singleShot(0, [&] {
        auto *dialog = qobject_cast<QDialog *>(QApplication::activeModalWidget());
        ASSERT_TRUE(dialog);
        auto *relativity = dialog->findChild<QCheckBox *>("hpopRelativisticAccelerations");
        ASSERT_TRUE(relativity);
        EXPECT_TRUE(relativity->isChecked());
        relativity->setChecked(false);
        dialog->reject();
    });
    EXPECT_FALSE(edit_hpop_settings(options, nullptr, {}, {entry}));
    EXPECT_EQ(options, saved);
}

TEST_F(QuantityTest, HpopRelativityMissingEphemerisReturnsToAdditionalWithoutPublishingDraft) {
    core::HpopOptions options;
    options.gravity_degree = options.gravity_order = 0;
    const auto saved = options;
    QTimer::singleShot(0, [&] {
        auto *dialog = qobject_cast<QDialog *>(QApplication::activeModalWidget());
        ASSERT_TRUE(dialog);
        auto *relativity = dialog->findChild<QCheckBox *>("hpopRelativisticAccelerations");
        ASSERT_TRUE(relativity);
        relativity->setChecked(true);
        auto *outer = dialog->findChild<QTabWidget *>("hpopSettingsTabs");
        outer->setCurrentIndex(2);
        auto *ok = dialog->findChild<QDialogButtonBox *>()->button(QDialogButtonBox::Ok);
        ok->click();
        EXPECT_TRUE(dialog->isVisible());
        EXPECT_EQ(options, saved);
        EXPECT_EQ(outer->currentIndex(), 0);
        EXPECT_EQ(dialog->findChild<QTabWidget *>("hpopForceModelTabs")->currentIndex(), 3);
        EXPECT_TRUE(
            dialog->findChild<QLabel *>("errorText")->text().contains(QStringLiteral("相对论")));
        EXPECT_TRUE(relativity->isVisible());
        relativity->setChecked(false);
        ok->click();
    });
    EXPECT_TRUE(edit_hpop_settings(options, nullptr));
    EXPECT_EQ(options, saved);
}

TEST_F(QuantityTest, HpopSatelliteMassUnitsKeyboardResizeReopenAndCancel) {
    apply_workbench_style(*qobject_cast<QApplication *>(QApplication::instance()));
    core::HpopOptions options;
    options.gravity_degree = options.gravity_order = 0;
    options.drag.enabled = options.srp.enabled = true;
    options.srp.area_mass_ratio_m2_per_kg = .03;
    runtime::CatalogEphemeris entry;
    entry.selection = "de440s";
    entry.info.first = core::Epoch{0};
    entry.info.last = core::Epoch{86400000000000};
    QTimer::singleShot(0, [&] {
        auto *dialog = qobject_cast<QDialog *>(QApplication::activeModalWidget());
        ASSERT_TRUE(dialog);
        auto *mass = dialog->findChild<QuantityEditor *>("hpopSatelliteMass");
        auto *drag_area = dialog->findChild<QLabel *>("hpopDragCompatibleArea");
        auto *srp_area = dialog->findChild<QLabel *>("hpopSrpCompatibleArea");
        ASSERT_TRUE(mass && drag_area && srp_area);
        EXPECT_EQ(mass->value(Unit::kilogram), 1000);
        auto *forces = dialog->findChild<QTabWidget *>("hpopForceModelTabs");
        forces->setCurrentIndex(3);
        reveal(mass);
        choose(*mass, Unit::tonne);
        text(*mass)->setFocus();
        text(*mass)->selectAll();
        QKeyEvent typing(QEvent::KeyPress, Qt::Key_1, Qt::NoModifier, QStringLiteral("1.25"));
        QApplication::sendEvent(text(*mass), &typing);
        EXPECT_EQ(mass->value(Unit::kilogram), 1250);
        choose(*mass, Unit::gram);
        EXPECT_EQ(mass->value(Unit::gram), 1250000);
        choose(*mass, Unit::pound);
        EXPECT_EQ(mass->value(Unit::kilogram), 1250);
        choose(*mass, Unit::milligram);
        EXPECT_EQ(mass->value(Unit::milligram), 1250000000);
        choose(*mass, Unit::tonne);
        EXPECT_EQ(text(*mass)->text(), QStringLiteral("1.25"));
        EXPECT_EQ(drag_area->text(), QStringLiteral("25 m²"));
        EXPECT_EQ(srp_area->text(), QStringLiteral("37.5 m²"));
        const auto capture = qEnvironmentVariable("ORBELLE_HPOP_UI_CAPTURE");
        std::cout << "HPOP mass device pixel ratio: " << dialog->devicePixelRatioF() << '\n';
        for (int width : {720, 430}) {
            dialog->resize(width, width == 720 ? 700 : 620);
            for (const int page_index : {3, 1, 2}) {
                forces->setCurrentIndex(page_index);
                QEventLoop settled;
                QTimer::singleShot(60, &settled, &QEventLoop::quit);
                settled.exec();
                auto *scroll = qobject_cast<QScrollArea *>(forces->currentWidget());
                ASSERT_TRUE(scroll);
                EXPECT_EQ(scroll->horizontalScrollBar()->maximum(), 0);
                EXPECT_EQ(mass, dialog->findChild<QuantityEditor *>("hpopSatelliteMass"));
                EXPECT_EQ(mass->value(Unit::kilogram), 1250);
                EXPECT_EQ(mass->unit(), Unit::tonne);
                EXPECT_TRUE(
                    dialog->rect().contains(dialog->findChild<QDialogButtonBox *>()->geometry()));
                if (!capture.isEmpty()) {
                    EXPECT_TRUE(QDir{}.mkpath(capture));
                    EXPECT_TRUE(dialog->grab().save(
                        capture + QString("/hpop-mass-%1-%2.png").arg(page_index).arg(width)));
                }
            }
        }
        dialog->findChild<QDialogButtonBox *>()->button(QDialogButtonBox::Ok)->click();
    });
    EXPECT_TRUE(edit_hpop_settings(options, nullptr, {}, {entry}));
    EXPECT_EQ(options.satellite_mass_kg, 1250);
    EXPECT_EQ(options.drag.area_mass_ratio_m2_per_kg, .02);
    EXPECT_EQ(options.srp.area_mass_ratio_m2_per_kg, .03);
    const auto saved = options;
    QTimer::singleShot(0, [&] {
        auto *dialog = qobject_cast<QDialog *>(QApplication::activeModalWidget());
        ASSERT_TRUE(dialog);
        auto *mass = dialog->findChild<QuantityEditor *>("hpopSatelliteMass");
        ASSERT_TRUE(mass);
        EXPECT_EQ(mass->value(Unit::kilogram), 1250);
        text(*mass)->setText("900");
        EXPECT_EQ(dialog->findChild<QLabel *>("hpopDragCompatibleArea")->text(),
                  QStringLiteral("18 m²"));
        dialog->reject();
    });
    EXPECT_FALSE(edit_hpop_settings(options, nullptr, {}, {entry}));
    EXPECT_EQ(options, saved);
}

TEST_F(QuantityTest, HpopSatelliteMassInvalidValueReturnsToAdditionalWithoutCommitting) {
    core::HpopOptions options;
    options.gravity_degree = options.gravity_order = 0;
    options.srp.area_mass_ratio_m2_per_kg = 9999;
    const auto saved = options;
    QTimer::singleShot(0, [&] {
        auto *dialog = qobject_cast<QDialog *>(QApplication::activeModalWidget());
        ASSERT_TRUE(dialog);
        auto *mass = dialog->findChild<QuantityEditor *>("hpopSatelliteMass");
        ASSERT_TRUE(mass);
        auto *outer = dialog->findChild<QTabWidget *>("hpopSettingsTabs");
        auto *ok = dialog->findChild<QDialogButtonBox *>()->button(QDialogButtonBox::Ok);
        for (const char *invalid : {"0", "-1", "1e309", "1e308", "5e-324"}) {
            text(*mass)->setText(invalid);
            outer->setCurrentIndex(2);
            ok->click();
            EXPECT_TRUE(dialog->isVisible());
            EXPECT_EQ(options, saved);
            EXPECT_EQ(outer->currentIndex(), 0);
            EXPECT_EQ(dialog->findChild<QTabWidget *>("hpopForceModelTabs")->currentIndex(), 3);
            EXPECT_FALSE(dialog->findChild<QLabel *>("errorText")->text().isEmpty());
            EXPECT_TRUE(mass->isVisible());
        }
        dialog->reject();
    });
    EXPECT_FALSE(edit_hpop_settings(options, nullptr));
    EXPECT_EQ(options, saved);
}

TEST_F(QuantityTest, HpopSrpBodyAssignmentSearchRadiusDraftsNestedCancelAndNarrowLayout) {
    apply_workbench_style(*qobject_cast<QApplication *>(QApplication::instance()));
    core::HpopOptions options;
    options.srp.enabled = true;
    runtime::CatalogEphemeris entry;
    entry.selection = "de440s";
    const auto visit = [&](bool accept_main) {
        QTimer::singleShot(0, [&] {
            auto *dialog = qobject_cast<QDialog *>(QApplication::activeModalWidget());
            ASSERT_TRUE(dialog);
            dialog->findChild<QTabWidget *>("hpopForceModelTabs")->setCurrentIndex(2);
            auto *select = dialog->findChild<QPushButton *>("hpopSrpSelectBodies");
            const auto summary = dialog->findChild<QLabel *>("hpopSrpEclipsingSummary")->text();
            QTimer::singleShot(0, [&] {
                auto *selection = qobject_cast<QDialog *>(QApplication::activeModalWidget());
                ASSERT_TRUE(selection);
                auto *available = selection->findChild<QListWidget *>("hpopSrpAvailableBodies");
                auto *assigned = selection->findChild<QListWidget *>("hpopSrpAssignedBodies");
                auto *search = selection->findChild<QLineEdit *>("hpopSrpBodySearch");
                ASSERT_TRUE(available && assigned && search);
                search->setText("vEnUs");
                for (int i = 0; i < available->count(); ++i)
                    if (!available->item(i)->isHidden()) {
                        EXPECT_EQ(available->item(i)->data(Qt::UserRole).toInt(), 3);
                        available->setCurrentItem(available->item(i));
                    }
                selection->findChild<QToolButton *>("hpopSrpAddBody")->click();
                ASSERT_EQ(assigned->count(), 3);
                auto *radius = selection->findChild<QuantityEditor *>("hpopSrpRadius3");
                ASSERT_TRUE(radius);
                radius->set_unit(Unit::kilometre);
                radius->findChild<QLineEdit *>("quantityValue")->setText("6000.125");
                const auto capture = qEnvironmentVariable("ORBELLE_HPOP_UI_CAPTURE");
                std::cout << "HPOP SRP bodies DPR: " << selection->devicePixelRatioF() << '\n';
                for (int width : {430, 720}) {
                    selection->resize(width, 520);
                    QEventLoop settled;
                    QTimer::singleShot(60, &settled, &QEventLoop::quit);
                    settled.exec();
                    EXPECT_EQ(radius->findChild<QLineEdit *>("quantityValue")->text(), "6000.125");
                    EXPECT_TRUE(selection->rect().contains(
                        selection->findChild<QDialogButtonBox *>()->geometry()));
                    EXPECT_EQ(available->horizontalScrollBar()->maximum(), 0);
                    EXPECT_EQ(assigned->horizontalScrollBar()->maximum(), 0);
                    if (!capture.isEmpty()) {
                        EXPECT_TRUE(QDir{}.mkpath(capture));
                        EXPECT_TRUE(selection->grab().save(
                            capture + QString("/hpop-srp-bodies-%1.png").arg(width)));
                    }
                }
                radius->findChild<QLineEdit *>("quantityValue")->setText("-1");
                selection->findChild<QDialogButtonBox *>()->button(QDialogButtonBox::Ok)->click();
                EXPECT_TRUE(selection->isVisible());
                EXPECT_FALSE(
                    selection->findChild<QLabel *>("hpopSrpBodiesError")->text().isEmpty());
                radius->findChild<QLineEdit *>("quantityValue")->setText("6000.125");
                selection->findChild<QDialogButtonBox *>()->button(QDialogButtonBox::Ok)->click();
            });
            select->click();
            EXPECT_NE(dialog->findChild<QLabel *>("hpopSrpEclipsingSummary")->text(), summary);
            QTimer::singleShot(0, [&] {
                auto *selection = qobject_cast<QDialog *>(QApplication::activeModalWidget());
                ASSERT_TRUE(selection);
                auto *assigned = selection->findChild<QListWidget *>("hpopSrpAssignedBodies");
                ASSERT_EQ(assigned->count(), 3);
                for (int i = 0; i < assigned->count(); ++i)
                    if (assigned->item(i)->data(Qt::UserRole).toInt() == 3)
                        assigned->setCurrentItem(assigned->item(i));
                EXPECT_EQ(
                    selection->findChild<QuantityEditor *>("hpopSrpRadius3")->value(Unit::metre),
                    6000125);
                selection->findChild<QToolButton *>("hpopSrpRemoveBody")->click();
                selection->reject();
            });
            select->click();
            EXPECT_TRUE(
                dialog->findChild<QLabel *>("hpopSrpEclipsingSummary")->text().contains("Venus"));
            if (accept_main)
                dialog->findChild<QDialogButtonBox *>()->button(QDialogButtonBox::Ok)->click();
            else
                dialog->reject();
        });
        return edit_hpop_settings(options, nullptr, {}, {entry});
    };
    const auto before = options;
    EXPECT_FALSE(visit(false));
    EXPECT_EQ(options, before);
    EXPECT_TRUE(visit(true));
    ASSERT_EQ(options.srp.additional_eclipsing_bodies.size(), 1U);
    EXPECT_EQ(options.srp.additional_eclipsing_bodies[0],
              (core::SrpEclipsingBody{core::ThirdBody::venus, 6000125}));
}
} // namespace orbelle::desktop
