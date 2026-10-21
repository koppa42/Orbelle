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
#include <QFile>
#include <QKeyEvent>
#include <QLabel>
#include <QLineEdit>
#include <QPushButton>
#include <QScrollArea>
#include <QScrollBar>
#include <QTabWidget>
#include <QTemporaryDir>
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
TEST_F(QuantityTest, HpopDragUnitsModelsResizeSaveReopenAndCancel) {
    apply_workbench_style(*qobject_cast<QApplication *>(QApplication::instance()));
    core::HpopOptions options;
    runtime::CatalogEphemeris entry;
    entry.selection = "de440s";
    entry.info.first = core::Epoch{0};
    entry.info.last = core::Epoch{86400000000000};
    QTimer::singleShot(0, [&] {
        auto *dialog = qobject_cast<QDialog *>(QApplication::activeModalWidget());
        ASSERT_TRUE(dialog);
        auto *forces = dialog->findChild<QTabWidget *>("hpopForceModelTabs");
        forces->setCurrentIndex(1);
        auto *use = dialog->findChild<QCheckBox *>("hpopUseDrag");
        auto *area = dialog->findChild<QuantityEditor *>("hpopDragAreaMassRatio");
        auto *blend = dialog->findChild<QuantityEditor *>("hpopDragBlendingRange");
        auto *model = dialog->findChild<QComboBox *>("hpopDragDensityModel");
        auto *low = dialog->findChild<QComboBox *>("hpopDragLowAltitudeModel");
        EXPECT_FALSE(area->isEnabled());
        use->setChecked(true);
        auto *cutoff = dialog->findChild<QLabel *>("hpopDragHighAltitudeCutoff");
        ASSERT_TRUE(cutoff);
        EXPECT_TRUE(cutoff->isVisible());
        EXPECT_TRUE(cutoff->text().contains("2500 km"));
        EXPECT_TRUE(dialog->findChild<QCheckBox *>("hpopDragApproximateAltitude")->isVisible());
        area->set_unit(Unit::square_centimetres_per_gram);
        area->findChild<QLineEdit *>("quantityValue")->setText("0.3");
        low->setCurrentIndex(1);
        EXPECT_TRUE(blend->isEnabled());
        blend->set_value(42, Unit::kilometre);
        model->setCurrentIndex(1);
        EXPECT_FALSE(low->isEnabled());
        EXPECT_FALSE(blend->isEnabled());
        model->setCurrentIndex(0);
        EXPECT_EQ(low->currentIndex(), 1);
        EXPECT_EQ(blend->value(Unit::metre), 42000);
        dialog->findChild<QuantityEditor *>("hpopDragCd")->set_value(2.7, Unit::one);
        dialog->findChild<QuantityEditor *>("hpopDragF107")->set_value(180, Unit::one);
        dialog->findChild<QuantityEditor *>("hpopDragF107Average")->set_value(160, Unit::one);
        dialog->findChild<QuantityEditor *>("hpopDragKp")->set_value(4.3, Unit::one);
        dialog->findChild<QCheckBox *>("hpopDragApparentSun")->setChecked(true);
        dialog->findChild<QCheckBox *>("hpopDragApproximateAltitude")->setChecked(true);
        EXPECT_FALSE(dialog->findChild<QLabel *>("hpopDragEphemerisStatus")
                         ->text()
                         .contains(QStringLiteral("已安装")));
        const auto capture = qEnvironmentVariable("ORBITFORGE_HPOP_UI_CAPTURE");
        std::cout << "HPOP drag device pixel ratio: " << dialog->devicePixelRatioF() << '\n';
        for (int width : {720, 430}) {
            dialog->resize(width, width == 720 ? 700 : 620);
            QEventLoop settled;
            QTimer::singleShot(60, &settled, &QEventLoop::quit);
            settled.exec();
            EXPECT_EQ(area, dialog->findChild<QuantityEditor *>("hpopDragAreaMassRatio"));
            EXPECT_EQ(area->findChild<QLineEdit *>("quantityValue")->text(), "0.3");
            auto *buttons = dialog->findChild<QDialogButtonBox *>();
            EXPECT_TRUE(dialog->rect().contains(buttons->geometry()));
            if (!capture.isEmpty()) {
                EXPECT_TRUE(QDir{}.mkpath(capture));
                auto *scroll = dialog->findChild<QScrollArea *>("hpopDragPage");
                scroll->verticalScrollBar()->setValue(0);
                QApplication::processEvents();
                EXPECT_TRUE(dialog->grab().save(capture + QString("/hpop-drag-%1.png").arg(width)));
                auto *approximate = dialog->findChild<QCheckBox *>("hpopDragApproximateAltitude");
                EXPECT_TRUE(approximate->isVisible());
                scroll->ensureWidgetVisible(approximate);
                approximate->setFocus();
                const auto press_space = [&] {
                    QKeyEvent press(QEvent::KeyPress, Qt::Key_Space, Qt::NoModifier);
                    QKeyEvent release(QEvent::KeyRelease, Qt::Key_Space, Qt::NoModifier);
                    QApplication::sendEvent(approximate, &press);
                    QApplication::sendEvent(approximate, &release);
                };
                press_space();
                EXPECT_FALSE(approximate->isChecked());
                press_space();
                EXPECT_TRUE(approximate->isChecked());
                QApplication::processEvents();
                EXPECT_TRUE(
                    dialog->grab().save(capture + QString("/hpop-drag-data-%1.png").arg(width)));
            }
        }
        dialog->findChild<QDialogButtonBox *>()->button(QDialogButtonBox::Ok)->click();
    });
    EXPECT_TRUE(edit_hpop_settings(options, nullptr, {}, {entry}));
    EXPECT_TRUE(options.drag.enabled);
    EXPECT_EQ(options.drag.cd, 2.7);
    EXPECT_NEAR(options.drag.area_mass_ratio_m2_per_kg, .03, 1e-16);
    EXPECT_EQ(options.drag.blending_range_m, 42000);
    EXPECT_EQ(options.drag.f107, 180);
    EXPECT_EQ(options.drag.f107_average, 160);
    EXPECT_EQ(options.drag.kp, 4.3);
    EXPECT_TRUE(options.drag.use_apparent_sun_position);
    EXPECT_TRUE(options.drag.use_approximate_altitude);
    const auto saved = options;
    QTimer::singleShot(0, [&] {
        auto *dialog = qobject_cast<QDialog *>(QApplication::activeModalWidget());
        ASSERT_TRUE(dialog);
        EXPECT_TRUE(dialog->findChild<QCheckBox *>("hpopUseDrag")->isChecked());
        EXPECT_TRUE(dialog->findChild<QCheckBox *>("hpopDragApproximateAltitude")->isChecked());
        dialog->findChild<QCheckBox *>("hpopDragApproximateAltitude")->setChecked(false);
        EXPECT_EQ(dialog->findChild<QuantityEditor *>("hpopDragF107")->value(Unit::one), 180);
        dialog->findChild<QCheckBox *>("hpopUseDrag")->setChecked(false);
        dialog->reject();
    });
    EXPECT_FALSE(edit_hpop_settings(options, nullptr, {}, {entry}));
    EXPECT_EQ(options, saved);
}

TEST_F(QuantityTest, HpopDragErrorsReturnToDragAndDoNotCommitInvalidOrMissingData) {
    core::HpopOptions options;
    const auto saved = options;
    QTimer::singleShot(0, [&] {
        auto *dialog = qobject_cast<QDialog *>(QApplication::activeModalWidget());
        ASSERT_TRUE(dialog);
        auto *outer = dialog->findChild<QTabWidget *>("hpopSettingsTabs");
        auto *forces = dialog->findChild<QTabWidget *>("hpopForceModelTabs");
        auto *kp = dialog->findChild<QuantityEditor *>("hpopDragKp")
                       ->findChild<QLineEdit *>("quantityValue");
        auto *use = dialog->findChild<QCheckBox *>("hpopUseDrag");
        auto *accept = dialog->findChild<QDialogButtonBox *>()->button(QDialogButtonBox::Ok);
        use->setChecked(true);
        kp->setText("10");
        outer->setCurrentIndex(2);
        accept->click();
        EXPECT_EQ(options, saved);
        EXPECT_TRUE(dialog->isVisible());
        EXPECT_EQ(outer->currentIndex(), 0);
        EXPECT_EQ(forces->currentIndex(), 1);
        kp->setText("3");
        outer->setCurrentIndex(2);
        accept->click();
        EXPECT_EQ(options, saved);
        EXPECT_EQ(forces->currentIndex(), 1);
        EXPECT_TRUE(
            dialog->findChild<QLabel *>("errorText")->text().contains(QStringLiteral("星历")));
        use->setChecked(false);
        kp->setText("invalid");
        accept->click();
    });
    EXPECT_TRUE(edit_hpop_settings(options, nullptr));
    EXPECT_EQ(options, saved);
}

TEST_F(QuantityTest, HpopWeatherFileSwitchesFieldsRetainsDraftsAndReopens) {
    apply_workbench_style(*qobject_cast<QApplication *>(QApplication::instance()));
    QTemporaryDir temporary;
    ASSERT_TRUE(temporary.isValid());
    const auto root = std::filesystem::path(temporary.path().toStdWString());
    EopCatalog catalog(root / "data", {}, {}, false);
    QFile file(temporary.path() + "/weather.csv");
    ASSERT_TRUE(file.open(QIODevice::WriteOnly));
    file.write(
        "DATE,BSRN,ND,KP1,KP2,KP3,KP4,KP5,KP6,KP7,KP8,KP_SUM,"
        "AP1,AP2,AP3,AP4,AP5,AP6,AP7,AP8,AP_AVG,CP,C9,ISN,F10.7_OBS,F10.7_ADJ,"
        "F10.7_DATA_TYPE,F10.7_OBS_CENTER81,F10.7_OBS_LAST81,F10.7_ADJ_CENTER81,F10.7_ADJ_LAST81\n"
        "2000-01-01,1,1,30,30,30,30,30,30,30,30,240,15,15,15,15,15,15,15,15,15,0,0,0,150,150,OBS,"
        "150,150,150,150\n");
    file.close();
    catalog.data().install_space_weather(root / "weather.csv");
    std::ofstream cssi(root / "cssi.dat");
    cssi << "2000 1 2000 2 1999 12 2\n2000 1 170 150 20 15\n2000 2 180 160 22 16\n";
    cssi.close();
    catalog.data().install_space_weather(root / "cssi.dat", "cssi");
    core::HpopOptions options;
    runtime::CatalogEphemeris entry;
    entry.selection = "de440s";
    QTimer::singleShot(0, [&] {
        auto *dialog = qobject_cast<QDialog *>(QApplication::activeModalWidget());
        ASSERT_TRUE(dialog);
        dialog->findChild<QTabWidget *>("hpopForceModelTabs")->setCurrentIndex(1);
        dialog->findChild<QCheckBox *>("hpopUseDrag")->setChecked(true);
        dialog->findChild<QPushButton *>("hpopDragManageEphemerides")->click();
        QTimer::singleShot(5000, dialog, &QDialog::reject);
        auto *source = dialog->findChild<QComboBox *>("hpopDragWeatherSource");
        auto *f107 = dialog->findChild<QuantityEditor *>("hpopDragF107");
        auto *selection = dialog->findChild<QComboBox *>("hpopDragWeatherFile");
        f107->set_value(183, Unit::one);
        EXPECT_TRUE(f107->isEnabled());
        EXPECT_FALSE(selection->isEnabled());
        source->setCurrentIndex(1);
        EXPECT_FALSE(f107->isEnabled());
        EXPECT_TRUE(selection->isEnabled());
        EXPECT_EQ(selection->currentData().toString(), "celestrak");
        EXPECT_FALSE(selection->currentText().contains(QStringLiteral("已安装")));
        auto *update = dialog->findChild<QComboBox *>("hpopDragGeomagneticUpdate");
        update->setFocus();
        QKeyEvent key(QEvent::KeyPress, Qt::Key_End, Qt::NoModifier);
        QApplication::sendEvent(update, &key);
        EXPECT_EQ(update->count(), 4);
        EXPECT_EQ(update->currentIndex(), 3);
        QKeyEvent previous(QEvent::KeyPress, Qt::Key_Up, Qt::NoModifier);
        QApplication::sendEvent(update, &previous);
        EXPECT_EQ(update->currentIndex(), 2);
        EXPECT_EQ(update->currentText(), QStringLiteral("三小时插值（保持均值）"));
        auto *geomagnetic_source = dialog->findChild<QComboBox *>("hpopDragGeomagneticSource");
        geomagnetic_source->setCurrentIndex(1);
        selection->setCurrentIndex(selection->findData("cssi"));
        EXPECT_EQ(geomagnetic_source->currentIndex(), 0);
        EXPECT_FALSE(geomagnetic_source->isEnabled());
        source->setCurrentIndex(0);
        EXPECT_EQ(f107->value(Unit::one), 183);
        source->setCurrentIndex(1);
        const auto capture = qEnvironmentVariable("ORBITFORGE_HPOP_UI_CAPTURE");
        std::cout << "HPOP weather device pixel ratio: " << dialog->devicePixelRatioF() << '\n';
        for (const int width : {720, 430}) {
            dialog->resize(width, 620);
            QEventLoop settled;
            QTimer::singleShot(60, &settled, &QEventLoop::quit);
            settled.exec();
            EXPECT_EQ(selection, dialog->findChild<QComboBox *>("hpopDragWeatherFile"));
            EXPECT_EQ(selection->currentData().toString(), "cssi");
            auto *scroll = dialog->findChild<QScrollArea *>("hpopDragPage");
            scroll->ensureWidgetVisible(geomagnetic_source);
            QApplication::processEvents();
            EXPECT_EQ(scroll->horizontalScrollBar()->maximum(), 0);
            EXPECT_TRUE(
                dialog->rect().contains(dialog->findChild<QDialogButtonBox *>()->geometry()));
            if (!capture.isEmpty()) {
                EXPECT_TRUE(QDir{}.mkpath(capture));
                EXPECT_TRUE(
                    dialog->grab().save(capture + QString("/hpop-weather-%1.png").arg(width)));
            }
        }
        dialog->findChild<QDialogButtonBox *>()->button(QDialogButtonBox::Ok)->click();
    });
    ASSERT_TRUE(edit_hpop_settings(
        options, nullptr, {}, {entry}, [entry] { return std::vector{entry}; }, &catalog));
    EXPECT_EQ(options.drag.weather_model, "cssi");
    EXPECT_EQ(options.drag.geomagnetic_update, core::GeomagneticUpdate::three_hourly_interpolated);
    EXPECT_EQ(options.drag.geomagnetic_source, core::GeomagneticSource::ap);
    EXPECT_EQ(options.drag.f107, 183);
    const auto saved = options;
    QTimer::singleShot(0, [&] {
        auto *dialog = qobject_cast<QDialog *>(QApplication::activeModalWidget());
        ASSERT_TRUE(dialog);
        EXPECT_EQ(dialog->findChild<QComboBox *>("hpopDragWeatherSource")->currentIndex(), 1);
        EXPECT_EQ(dialog->findChild<QComboBox *>("hpopDragGeomagneticUpdate")->currentIndex(), 2);
        auto *geomagnetic_source = dialog->findChild<QComboBox *>("hpopDragGeomagneticSource");
        EXPECT_EQ(geomagnetic_source->currentIndex(), 0);
        EXPECT_FALSE(geomagnetic_source->isEnabled());
        auto *file = dialog->findChild<QComboBox *>("hpopDragWeatherFile");
        file->setCurrentIndex(file->findData("celestrak"));
        EXPECT_TRUE(geomagnetic_source->isEnabled());
        geomagnetic_source->setCurrentIndex(1);
        dialog->findChild<QComboBox *>("hpopDragWeatherSource")->setCurrentIndex(0);
        dialog->reject();
    });
    EXPECT_FALSE(edit_hpop_settings(
        options, nullptr, {}, {entry}, [entry] { return std::vector{entry}; }, &catalog));
    EXPECT_EQ(options, saved);
}
} // namespace orbitforge::desktop
