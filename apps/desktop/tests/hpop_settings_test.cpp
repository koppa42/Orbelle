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
#include <QScrollArea>
#include <QSpinBox>
#include <QTabWidget>
#include <QTableWidget>
#include <QTemporaryDir>
#include <QTimer>
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
TEST_F(QuantityTest, OpeningHpopWithDataManagementShowsOnlyTheSettingsWindow) {
    QTemporaryDir temporary;
    ASSERT_TRUE(temporary.isValid());
    EopCatalog catalog(std::filesystem::path(temporary.path().toStdWString()), {}, {}, false);
    core::HpopOptions options;
    const auto original = options;
    for (int services = 0; services < 4; ++services) {
        SCOPED_TRACE(services);
        bool opened_manager = false;
        std::function<std::vector<runtime::CatalogEphemeris>()> manage;
        if (services & 1)
            manage = [&] {
                opened_manager = true;
                return std::vector<runtime::CatalogEphemeris>{};
            };
        WindowShowObserver windows;
        QTimer::singleShot(0, [&] {
            auto *dialog = qobject_cast<QDialog *>(QApplication::activeModalWidget());
            ASSERT_TRUE(dialog);
            EXPECT_EQ(dialog->objectName(), "hpopSettingsDialog");
            EXPECT_EQ(windows.shown.join(", ").toStdString(), "QDialog:hpopSettingsDialog");
            // The weather manager belongs to file mode; manual mode deliberately hides it.
            dialog->findChild<QComboBox *>("hpopDragWeatherSource")->setCurrentIndex(1);
            for (const auto *name : {"hpopManageEphemerides", "hpopDragManageEphemerides",
                                     "hpopSrpManageEphemerides"}) {
                SCOPED_TRACE(name);
                auto *button = dialog->findChild<QWidget *>(name);
                ASSERT_TRUE(button);
                EXPECT_FALSE(button->isWindow());
                EXPECT_EQ(button->isHidden(), !(services & 1));
            }
            for (const auto *name : {"hpopManageOceanTides", "hpopDragManageWeather"}) {
                SCOPED_TRACE(name);
                auto *button = dialog->findChild<QWidget *>(name);
                ASSERT_TRUE(button);
                EXPECT_FALSE(button->isWindow());
                EXPECT_EQ(button->isHidden(), !(services & 2));
            }
            dialog->reject();
        });
        EXPECT_FALSE(edit_hpop_settings(options, nullptr, {}, {}, manage,
                                        services & 2 ? &catalog : nullptr));
        EXPECT_FALSE(opened_manager);
        EXPECT_FALSE(catalog.downloading());
        EXPECT_EQ(options, original);
        EXPECT_EQ(windows.shown.join(", ").toStdString(), "QDialog:hpopSettingsDialog");
    }
}

TEST_F(QuantityTest, HpopSettingsValidateCancelAndRoundTripWithoutApplyingTheOrbit) {
    OrbitInput input;
    input.propagator = OrbitPropagator::hpop;
    OrbitEditor editor(input);
    apply_workbench_style(*qobject_cast<QApplication *>(QApplication::instance()));
    editor.resize(1100, 740);
    editor.show();
    std::optional<OrbitInput> submitted;
    editor.apply = [&](OrbitInput value) { submitted = std::move(value); };
    const auto capture = qEnvironmentVariable("ORBITFORGE_HPOP_UI_CAPTURE");
    QTimer::singleShot(0, &editor, [&] {
        auto *dialog = editor.findChild<QDialog *>("hpopSettingsDialog");
        ASSERT_NE(dialog, nullptr);
        auto *degree = dialog->findChild<QSpinBox *>("hpopGravityDegree");
        auto *order = dialog->findChild<QSpinBox *>("hpopGravityOrder");
        auto *tabs = dialog->findChild<QTabWidget *>("hpopSettingsTabs");
        ASSERT_NE(degree, nullptr);
        ASSERT_NE(order, nullptr);
        degree->setValue(4);
        EXPECT_EQ(order->value(), 4);
        if (!capture.isEmpty()) {
            ASSERT_TRUE(QDir{}.mkpath(capture));
            QApplication::processEvents();
            EXPECT_TRUE(dialog->grab().save(capture + "/hpop-forces.png"));
            tabs->setCurrentIndex(1);
            QApplication::processEvents();
            EXPECT_TRUE(dialog->grab().save(capture + "/hpop-integration.png"));
            dialog->resize(400, 460);
            QApplication::processEvents();
            EXPECT_TRUE(dialog->grab().save(capture + "/hpop-compact.png"));
        }
        auto *minimum = dialog->findChild<QuantityEditor *>("hpopMinStep");
        auto *maximum = dialog->findChild<QuantityEditor *>("hpopMaxStep");
        minimum->set_value(50, Unit::second);
        maximum->set_value(10, Unit::second);
        dialog->findChild<QDialogButtonBox *>()->button(QDialogButtonBox::Ok)->click();
        EXPECT_TRUE(dialog->isVisible());
        EXPECT_TRUE(dialog->findChild<QLabel *>("errorText")->isVisible());
        minimum->set_value(.01, Unit::second);
        maximum->set_value(30, Unit::second);
        dialog->findChild<QDialogButtonBox *>()->button(QDialogButtonBox::Ok)->click();
    });
    editor.open_propagator_details();
    EXPECT_FALSE(submitted);
    editor.submit();
    ASSERT_TRUE(submitted);
    EXPECT_EQ(submitted->hpop.gravity_degree, 4);
    EXPECT_EQ(submitted->hpop.gravity_order, 4);
    EXPECT_EQ(submitted->hpop.max_step_seconds, 30);
    const auto saved = *submitted;
    editor.set_input(saved);
    QTimer::singleShot(0, &editor, [&] {
        auto *dialog = editor.findChild<QDialog *>("hpopSettingsDialog");
        ASSERT_NE(dialog, nullptr);
        EXPECT_EQ(dialog->findChild<QSpinBox *>("hpopGravityDegree")->value(), 4);
        dialog->findChild<QComboBox *>("hpopIntegrator")->setCurrentIndex(1);
        dialog->reject();
    });
    editor.open_propagator_details();
    editor.submit();
    EXPECT_EQ(submitted->hpop, saved.hpop);
    EXPECT_NEAR(submitted->initial.position_m[0], saved.initial.position_m[0], 1e-7);
    if (!capture.isEmpty()) {
        QApplication::processEvents();
        EXPECT_TRUE(editor.grab().save(capture + "/hpop-editor-wide.png"));
        editor.resize(760, 540);
        QEventLoop settled;
        QTimer::singleShot(50, &settled, &QEventLoop::quit);
        settled.exec(); // Allow queued responsive layouts to settle before the capture.
        EXPECT_TRUE(editor.grab().save(capture + "/hpop-editor-compact.png"));
    }
}

TEST_F(QuantityTest, HpopThirdBodiesPreserveDraftsAndValidateManualConstants) {
    apply_workbench_style(*qobject_cast<QApplication *>(QApplication::instance()));
    core::HpopOptions options;
    bool managed = false;
    QTimer::singleShot(0, [&] {
        auto *dialog = qobject_cast<QDialog *>(QApplication::activeModalWidget());
        ASSERT_TRUE(dialog);
        auto *tabs = dialog->findChild<QTabWidget *>("hpopSettingsTabs");
        ASSERT_TRUE(tabs);
        tabs->setCurrentIndex(0);
        auto *sun = dialog->findChild<QCheckBox *>("hpopSunGravity");
        auto *moon = dialog->findChild<QCheckBox *>("hpopMoonGravity");
        auto *source = dialog->findChild<QComboBox *>("hpopSunSource");
        auto *quantity = dialog->findChild<QuantityEditor *>("hpopSunMu");
        auto *mu = quantity->findChild<QLineEdit *>("quantityValue");
        ASSERT_TRUE(sun && moon && source && mu);
        EXPECT_EQ(source->currentIndex(), 1);
        EXPECT_EQ(dialog->findChild<QComboBox *>("hpopMoonSource")->currentIndex(), 1);
        EXPECT_FALSE(options.third_body(core::ThirdBody::sun).user_mu ||
                     options.third_body(core::ThirdBody::moon).user_mu);
        EXPECT_FALSE(source->isEnabled());
        sun->setChecked(true);
        moon->setChecked(true);
        EXPECT_TRUE(mu->isReadOnly());
        const auto constant = quantity->value(Unit::metres_cubed_per_second_squared);
        QKeyEvent up(QEvent::KeyPress, Qt::Key_Up, Qt::NoModifier);
        QApplication::sendEvent(mu, &up);
        EXPECT_EQ(quantity->value(Unit::metres_cubed_per_second_squared), constant);
        quantity->set_unit(Unit::metres_cubed_per_second_squared);
        EXPECT_EQ(quantity->value(Unit::metres_cubed_per_second_squared), constant);
        quantity->set_unit(Unit::kilometres_cubed_per_second_squared);
        source->setCurrentIndex(2);
        mu->setText("1.3271244e11");
        source->setCurrentIndex(1);
        EXPECT_TRUE(mu->isReadOnly());
        source->setCurrentIndex(2);
        EXPECT_EQ(mu->text(), QStringLiteral("1.3271244e11"));
        dialog->findChild<QToolButton *>("hpopManageEphemerides")->click();
        EXPECT_TRUE(managed);
        EXPECT_EQ(mu->text(), QStringLiteral("1.3271244e11"));
        EXPECT_TRUE(dialog->findChild<QLabel *>("hpopEphemerisStatus")->text().contains("2000"));
        const auto output = qEnvironmentVariable("ORBITFORGE_HPOP_UI_CAPTURE");
        if (!output.isEmpty()) {
            source->setCurrentIndex(1); // Preview the normal, automatic-GM configuration.
            QDir{}.mkpath(output);
            auto *gravity_scroll = dialog->findChild<QScrollArea *>("hpopGravityPage");
            gravity_scroll->ensureWidgetVisible(
                dialog->findChild<QTableWidget *>("hpopThirdBodyTable"));
            QApplication::processEvents();
            EXPECT_TRUE(dialog->grab().save(output + "/third-body-wide.png"));
            dialog->resize(460, 680);
            QEventLoop settled;
            QTimer::singleShot(60, &settled, &QEventLoop::quit);
            settled.exec();
            EXPECT_TRUE(dialog->grab().save(output + "/third-body-compact.png"));
            source->setCurrentIndex(2);
        }
        mu->setText("0");
        dialog->findChild<QDialogButtonBox *>()->button(QDialogButtonBox::Ok)->click();
        EXPECT_TRUE(dialog->isVisible());
        mu->setText("1.3271244e11");
        dialog->findChild<QDialogButtonBox *>()->button(QDialogButtonBox::Ok)->click();
    });
    EXPECT_TRUE(edit_hpop_settings(options, nullptr, {}, {}, [&] {
        managed = true;
        runtime::CatalogEphemeris entry;
        entry.selection = "de440s";
        entry.info.first = core::Epoch{0};
        entry.info.last = core::Epoch{86400000000000};
        return std::vector{entry};
    }));
    EXPECT_TRUE(options.third_body(core::ThirdBody::sun).enabled &&
                options.third_body(core::ThirdBody::moon).enabled);
    ASSERT_TRUE(options.third_body(core::ThirdBody::sun).user_mu);
    EXPECT_EQ(*options.third_body(core::ThirdBody::sun).user_mu, 1.3271244e20);
    EXPECT_FALSE(options.third_body(core::ThirdBody::moon).user_mu);
    const auto saved = options;
    QTimer::singleShot(0, [&] {
        auto *dialog = qobject_cast<QDialog *>(QApplication::activeModalWidget());
        ASSERT_TRUE(dialog);
        EXPECT_TRUE(dialog->findChild<QCheckBox *>("hpopSunGravity")->isChecked());
        dialog->findChild<QCheckBox *>("hpopMoonGravity")->setChecked(false);
        dialog->reject();
    });
    EXPECT_FALSE(edit_hpop_settings(options, nullptr));
    EXPECT_EQ(options, saved);
}

TEST_F(QuantityTest, HpopPlanetTableSearchAndSourceSwitchingPreserveCbAndManualDrafts) {
    core::HpopOptions options;
    auto &jupiter = options.third_body(core::ThirdBody::jupiter);
    jupiter = {true, core::ThirdBodyGravitySource::cb_file, 1.1e17,
               core::CbGravity{1.26712764e17, "Jupiter.cb", std::string(64, 'a'),
                               core::ThirdBody::jupiter}};
    QTimer::singleShot(0, [&] {
        auto *dialog = qobject_cast<QDialog *>(QApplication::activeModalWidget());
        ASSERT_TRUE(dialog);
        auto *table = dialog->findChild<QTableWidget *>("hpopThirdBodyTable");
        ASSERT_TRUE(table);
        EXPECT_EQ(table->rowCount(), 10);
        EXPECT_EQ(table->columnCount(), 4);
        auto *search = dialog->findChild<QLineEdit *>("hpopThirdBodySearch");
        search->setText("Jupiter");
        EXPECT_FALSE(table->isRowHidden(2));
        EXPECT_TRUE(table->isRowHidden(0));
        auto *source = dialog->findChild<QComboBox *>("hpopJupiterSource");
        auto *gm = dialog->findChild<QuantityEditor *>("hpopJupiterMu");
        EXPECT_EQ(source->currentIndex(), 0);
        EXPECT_EQ(gm->value(Unit::metres_cubed_per_second_squared), 1.26712764e17);
        source->setCurrentIndex(2);
        auto *input = gm->findChild<QLineEdit *>("quantityValue");
        input->setText("1.25e8");
        source->setCurrentIndex(1);
        EXPECT_NEAR(gm->value(Unit::metres_cubed_per_second_squared), runtime::de440_planet_mu[0],
                    1000);
        source->setCurrentIndex(0);
        EXPECT_EQ(gm->value(Unit::metres_cubed_per_second_squared), 1.26712764e17);
        source->setCurrentIndex(2);
        EXPECT_EQ(input->text(), QStringLiteral("1.25e8"));
        dialog->resize(460, 680);
        EXPECT_EQ(input->text(), QStringLiteral("1.25e8"));
        dialog->findChild<QCheckBox *>("hpopPlutoGravity")->setChecked(true);
        dialog->findChild<QDialogButtonBox *>()->button(QDialogButtonBox::Ok)->click();
    });
    EXPECT_TRUE(edit_hpop_settings(options, nullptr));
    EXPECT_EQ(jupiter.source, core::ThirdBodyGravitySource::user_specified);
    EXPECT_EQ(*jupiter.user_mu, 1.25e17);
    ASSERT_TRUE(jupiter.cb);
    EXPECT_EQ(jupiter.cb->mu, 1.26712764e17);
    EXPECT_TRUE(options.third_body(core::ThirdBody::pluto).enabled);
}

TEST_F(QuantityTest, HpopSourceSwitchingKeepsExactSiAndRecoversAnUnavailableCbDisplay) {
    core::HpopOptions options;
    auto &sun = options.third_body(core::ThirdBody::sun);
    const auto exact = std::nextafter(runtime::de440_sun_mu, 0.0);
    sun = {true, core::ThirdBodyGravitySource::user_specified, exact, std::nullopt};
    QTimer::singleShot(0, [&] {
        auto *dialog = qobject_cast<QDialog *>(QApplication::activeModalWidget());
        ASSERT_TRUE(dialog);
        auto *source = dialog->findChild<QComboBox *>("hpopSunSource");
        auto *gm = dialog->findChild<QuantityEditor *>("hpopSunMu");
        gm->set_unit(Unit::metres_cubed_per_second_squared);
        source->setCurrentIndex(1);
        gm->set_unit(Unit::kilometres_cubed_per_second_squared);
        source->setCurrentIndex(0); // Missing Cb constants display a nonnumeric placeholder.
        source->setCurrentIndex(2);
        EXPECT_EQ(gm->unit(), Unit::metres_cubed_per_second_squared);
        EXPECT_EQ(gm->value(Unit::metres_cubed_per_second_squared), exact);
        dialog->findChild<QDialogButtonBox *>()->button(QDialogButtonBox::Ok)->click();
    });
    EXPECT_TRUE(edit_hpop_settings(options, nullptr));
    EXPECT_EQ(*sun.user_mu, exact);
}

TEST_F(QuantityTest, HpopNestedPagesPreserveDraftsAndKeepUnavailableModelsInactive) {
    apply_workbench_style(*qobject_cast<QApplication *>(QApplication::instance()));
    core::HpopOptions options;
    QTimer::singleShot(0, [&] {
        auto *dialog = qobject_cast<QDialog *>(QApplication::activeModalWidget());
        ASSERT_TRUE(dialog);
        auto *outer = dialog->findChild<QTabWidget *>("hpopSettingsTabs");
        auto *forces = dialog->findChild<QTabWidget *>("hpopForceModelTabs");
        ASSERT_TRUE(outer && forces);
        EXPECT_EQ(outer->count(), 3);
        EXPECT_EQ(outer->tabText(0), QStringLiteral("力模型"));
        EXPECT_EQ(outer->tabText(1), QStringLiteral("积分器"));
        EXPECT_EQ(outer->tabText(2), QStringLiteral("协方差"));
        EXPECT_EQ(forces->count(), 4);
        EXPECT_EQ(forces->tabText(0), QStringLiteral("引力"));
        EXPECT_EQ(forces->tabText(1), QStringLiteral("阻力"));
        EXPECT_EQ(forces->tabText(2), QStringLiteral("太阳辐射压"));
        EXPECT_EQ(forces->tabText(3), QStringLiteral("其他"));
        auto *gravity = dialog->findChild<QScrollArea *>("hpopGravityPage");
        EXPECT_TRUE(
            gravity->isAncestorOf(dialog->findChild<QFrame *>("hpopCentralGravitySection")));
        EXPECT_TRUE(gravity->isAncestorOf(dialog->findChild<QTableWidget *>("hpopThirdBodyTable")));
        auto *integration = dialog->findChild<QScrollArea *>("hpopIntegratorPage");
        auto *altitude = dialog->findChild<QuantityEditor *>("hpopMinimumAltitude");
        EXPECT_TRUE(integration->isAncestorOf(altitude));
        auto *sun = dialog->findChild<QuantityEditor *>("hpopSunMu");
        dialog->findChild<QCheckBox *>("hpopSunGravity")->setChecked(true);
        dialog->findChild<QComboBox *>("hpopSunSource")->setCurrentIndex(2);
        sun->set_unit(Unit::metres_cubed_per_second_squared);
        auto *draft = sun->findChild<QLineEdit *>("quantityValue");
        draft->setText("1.23456789e20");
        const auto capture = qEnvironmentVariable("ORBITFORGE_HPOP_UI_CAPTURE");
        const auto screenshot = [&](const QString &name) {
            QApplication::processEvents();
            if (!capture.isEmpty()) {
                QEventLoop settled;
                QTimer::singleShot(60, &settled, &QEventLoop::quit);
                settled.exec();
                QDir{}.mkpath(capture);
                EXPECT_TRUE(dialog->grab().save(capture + "/" + name + ".png"));
            }
        };
        screenshot("hpop-gravity");
        for (int index = 1; index < 4; ++index) {
            forces->setCurrentIndex(index);
            if (index == 2) {
                EXPECT_FALSE(dialog->findChild<QCheckBox *>("hpopUseSrp")->isChecked());
                EXPECT_FALSE(dialog->findChild<QuantityEditor *>("hpopSrpCr")->isEnabled());
            } else if (index == 1) {
                EXPECT_FALSE(dialog->findChild<QCheckBox *>("hpopUseDrag")->isChecked());
                EXPECT_FALSE(dialog->findChild<QuantityEditor *>("hpopDragCd")->isEnabled());
            } else {
                EXPECT_EQ(forces->currentWidget()->findChildren<QCheckBox *>().size(), 1);
                EXPECT_FALSE(
                    dialog->findChild<QCheckBox *>("hpopRelativisticAccelerations")->isChecked());
                EXPECT_TRUE(
                    forces->currentWidget()->findChild<QWidget *>("hpopAdditionalPendingSection"));
            }
            screenshot(QStringLiteral("hpop-force-%1").arg(index));
        }
        outer->setCurrentIndex(2);
        EXPECT_FALSE(dialog->findChild<QCheckBox *>("hpopComputeCovariance")->isEnabled());
        screenshot("hpop-covariance");
        outer->setCurrentIndex(1);
        altitude->set_unit(Unit::kilometre);
        altitude->set_value(10, Unit::kilometre);
        screenshot("hpop-integrator");
        outer->setCurrentIndex(0);
        EXPECT_EQ(forces->currentIndex(), 3);
        forces->setCurrentIndex(0);
        dialog->resize(460, 680);
        screenshot("hpop-gravity-compact");
        EXPECT_EQ(dialog->findChild<QuantityEditor *>("hpopSunMu"), sun);
        EXPECT_EQ(draft->text(), QStringLiteral("1.23456789e20"));
        EXPECT_EQ(sun->unit(), Unit::metres_cubed_per_second_squared);
        EXPECT_EQ(altitude->value(Unit::metre), 10000);
        outer->setCurrentIndex(2);
        dialog->findChild<QDialogButtonBox *>()->button(QDialogButtonBox::Ok)->click();
    });
    EXPECT_TRUE(edit_hpop_settings(options, nullptr));
    EXPECT_EQ(options.minimum_altitude_m, 10000);
    EXPECT_EQ(*options.third_body(core::ThirdBody::sun).user_mu, 1.23456789e20);
    const auto saved = options;
    QTimer::singleShot(0, [&] {
        auto *dialog = qobject_cast<QDialog *>(QApplication::activeModalWidget());
        ASSERT_TRUE(dialog);
        dialog->findChild<QTabWidget *>("hpopForceModelTabs")->setCurrentIndex(2);
        dialog->findChild<QTabWidget *>("hpopSettingsTabs")->setCurrentIndex(2);
        dialog->findChild<QSpinBox *>("hpopGravityDegree")->setValue(2);
        dialog->reject();
    });
    EXPECT_FALSE(edit_hpop_settings(options, nullptr));
    EXPECT_EQ(options, saved);
}

TEST_F(QuantityTest, HpopValidationReturnsToTheOffscreenFieldAndDoesNotCommitTheDraft) {
    core::HpopOptions options;
    const auto saved = options;
    QTimer::singleShot(0, [&] {
        auto *dialog = qobject_cast<QDialog *>(QApplication::activeModalWidget());
        ASSERT_TRUE(dialog);
        auto *outer = dialog->findChild<QTabWidget *>("hpopSettingsTabs");
        auto *forces = dialog->findChild<QTabWidget *>("hpopForceModelTabs");
        auto *source = dialog->findChild<QComboBox *>("hpopSunSource");
        dialog->findChild<QCheckBox *>("hpopSunGravity")->setChecked(true);
        source->setCurrentIndex(2);
        auto *mu = dialog->findChild<QuantityEditor *>("hpopSunMu")
                       ->findChild<QLineEdit *>("quantityValue");
        mu->setText("0");
        auto *search = dialog->findChild<QLineEdit *>("hpopThirdBodySearch");
        search->setText("Mars");
        forces->setCurrentIndex(2);
        outer->setCurrentIndex(2);
        auto *accept = dialog->findChild<QDialogButtonBox *>()->button(QDialogButtonBox::Ok);
        accept->click();
        EXPECT_TRUE(dialog->isVisible());
        EXPECT_EQ(options, saved);
        EXPECT_EQ(outer->currentIndex(), 0);
        EXPECT_EQ(forces->currentIndex(), 0);
        EXPECT_TRUE(search->text().isEmpty());
        EXPECT_FALSE(dialog->findChild<QTableWidget *>("hpopThirdBodyTable")->isRowHidden(0));
        EXPECT_TRUE(dialog->findChild<QLabel *>("errorText")->isVisible());
        source->setCurrentIndex(1);
        dialog->findChild<QuantityEditor *>("hpopMinStep")->set_value(50, Unit::second);
        dialog->findChild<QuantityEditor *>("hpopMaxStep")->set_value(10, Unit::second);
        outer->setCurrentIndex(2);
        accept->click();
        EXPECT_EQ(options, saved);
        EXPECT_EQ(outer->currentIndex(), 1);
        dialog->reject();
    });
    EXPECT_FALSE(edit_hpop_settings(options, nullptr));
    EXPECT_EQ(options, saved);
}

TEST_F(QuantityTest, HpopNewDesktopDefaultsUseEgm2008AndKeepExplicitModelChoices) {
    OrbitEditor editor(OrbitInput{});
    runtime::CatalogGravity entry;
    entry.selection = "egm2008";
    entry.info = {"EGM2008", {}, {}, "tide_free", core::egm96_mu, 6378137, 2190, 2159};
    editor.gravity_models = [entry] { return std::vector{entry}; };
    editor.set_propagator(OrbitPropagator::hpop);
    QTimer::singleShot(0, &editor, [&] {
        auto *dialog = editor.findChild<QDialog *>("hpopSettingsDialog");
        ASSERT_TRUE(dialog);
        EXPECT_EQ(dialog->findChild<QComboBox *>("hpopGravityModel")->currentData().toString(),
                  "egm2008");
        EXPECT_EQ(dialog->findChild<QComboBox *>("hpopGravityModel")->currentText(), "EGM2008");
        EXPECT_EQ(dialog->findChild<QSpinBox *>("hpopGravityDegree")->value(), 21);
        EXPECT_EQ(dialog->findChild<QSpinBox *>("hpopGravityOrder")->value(), 21);
        const auto capture = qEnvironmentVariable("ORBITFORGE_HPOP_UI_CAPTURE");
        if (!capture.isEmpty()) {
            QEventLoop settled;
            QTimer::singleShot(60, &settled, &QEventLoop::quit);
            settled.exec();
            EXPECT_TRUE(QDir{}.mkpath(capture));
            EXPECT_TRUE(dialog->grab().save(capture + "/hpop-egm2008.png"));
        }
        dialog->findChild<QDialogButtonBox *>()->button(QDialogButtonBox::Ok)->click();
    });
    editor.open_propagator_details();
    std::optional<OrbitInput> saved;
    editor.apply = [&](OrbitInput input) { saved = input; };
    editor.submit();
    ASSERT_TRUE(saved);
    EXPECT_EQ(saved->hpop.gravity_model, "egm2008");
    OrbitInput explicit_builtin;
    explicit_builtin.propagator = OrbitPropagator::hpop;
    editor.set_input(explicit_builtin);
    editor.set_propagator(OrbitPropagator::two_body);
    editor.set_propagator(OrbitPropagator::hpop);
    editor.submit();
    ASSERT_TRUE(saved);
    EXPECT_EQ(saved->hpop, explicit_builtin.hpop);
}
} // namespace orbitforge::desktop
