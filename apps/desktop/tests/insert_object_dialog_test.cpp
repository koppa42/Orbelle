#include "../src/viewer/insert_object_dialog.hpp"
#include "../src/viewer/orbit_design_preview.hpp"
#include "../src/viewer/quantity_editor.hpp"
#include "../src/viewer/satellite_creation.hpp"
#include "../src/viewer/workbench_profile.hpp"
#include "../src/viewer/workbench_style.hpp"
#include <QApplication>
#include <QCheckBox>
#include <QComboBox>
#include <QElapsedTimer>
#include <QFile>
#include <QKeyEvent>
#include <QLabel>
#include <QLineEdit>
#include <QPushButton>
#include <QScrollArea>
#include <QScrollBar>
#include <QSlider>
#include <QSpinBox>
#include <QTemporaryDir>
#include <QThread>
#include <QTreeWidget>
#include <QWheelEvent>
#include <cmath>
#include <gtest/gtest.h>
#include <iostream>
#include <memory>
#include <numbers>
#include <orbitforge/runtime/time_frames.hpp>
#include <orbitforge/runtime/two_body.hpp>

namespace orbitforge::desktop {
class InsertionTest : public ::testing::Test {
  protected:
    void SetUp() override {
        static int argc = 1;
        static char name[] = "insertion_test";
        static char *argv[] = {name, nullptr};
        static std::unique_ptr<QApplication> app;
        if (!QApplication::instance())
            app = std::make_unique<QApplication>(argc, argv);
        apply_workbench_style(*qApp);
    }
    static void method(InsertObjectDialog &dialog, InsertObjectMethod value) {
        auto *tree = dialog.findChild<QTreeWidget *>("insertObjectMethods");
        for (int i = 0; i < tree->topLevelItemCount(); ++i)
            if (tree->topLevelItem(i)->data(0, Qt::UserRole).toInt() == static_cast<int>(value))
                tree->setCurrentItem(tree->topLevelItem(i));
    }
    static void click(QWidget &dialog, const char *name) {
        dialog.findChild<QPushButton *>(name)->click();
    }
    QTemporaryDir temporary;
    QString preferences() const { return temporary.filePath("preferences.ini"); }
};

TEST_F(InsertionTest, DefaultsAreExplicitPersistentAndIndependentPerType) {
    {
        InsertObjectDialog dialog(preferences());
        method(dialog, InsertObjectMethod::defaults);
        EXPECT_EQ(dialog.default_method(InsertObjectType::satellite),
                  InsertObjectMethod::define_properties);
        click(dialog, "saveDefaultInsertMethod");
        EXPECT_EQ(dialog.default_method(InsertObjectType::satellite), InsertObjectMethod::defaults);
        dialog.select_type(InsertObjectType::ground_station);
        EXPECT_EQ(dialog.selection()->method, InsertObjectMethod::define_properties);
        dialog.select_type(InsertObjectType::satellite);
        EXPECT_EQ(dialog.selection()->method, InsertObjectMethod::defaults);
    }
    InsertObjectDialog reopened(preferences());
    EXPECT_EQ(reopened.selection()->method, InsertObjectMethod::defaults);
    EXPECT_EQ(reopened.default_method(InsertObjectType::sensor),
              InsertObjectMethod::define_properties);
    QSettings settings(preferences(), QSettings::IniFormat);
    settings.setValue("insertion/default-method/sensor", "tle-file");
    settings.setValue("insertion/default-method/ground-station", "unknown");
    settings.sync();
    InsertObjectDialog corrupted(preferences());
    EXPECT_EQ(corrupted.default_method(InsertObjectType::sensor),
              InsertObjectMethod::define_properties);
    EXPECT_EQ(corrupted.default_method(InsertObjectType::ground_station),
              InsertObjectMethod::define_properties);
}

TEST_F(InsertionTest, ContinuousInsertBlocksDuplicatesAndCloseDoesNotResubmit) {
    InsertObjectDialog dialog(preferences());
    int calls = 0;
    dialog.insert = [&](InsertObjectRequest request) {
        EXPECT_EQ(request.method, InsertObjectMethod::defaults);
        ++calls;
        dialog.set_busy(true);
    };
    dialog.show();
    method(dialog, InsertObjectMethod::defaults);
    dialog.findChild<QCheckBox *>("keepInsertObjectsOpen")->setChecked(true);
    click(dialog, "insertObject");
    EXPECT_TRUE(dialog.isVisible());
    click(dialog, "insertObject");
    EXPECT_EQ(calls, 1);
    dialog.set_busy(false);
    dialog.show_result("对象已插入。");
    click(dialog, "insertObject");
    EXPECT_EQ(calls, 2);
    click(dialog, "closeInsertObjects");
    EXPECT_FALSE(dialog.isVisible());
    EXPECT_EQ(calls, 2);
}

TEST_F(InsertionTest, PropertiesAndEscapeKeepDraftBoundary) {
    InsertObjectDialog dialog(preferences());
    int calls = 0;
    dialog.insert = [&](InsertObjectRequest request) {
        ++calls;
        EXPECT_EQ(request.method, InsertObjectMethod::define_properties);
    };
    dialog.show();
    QKeyEvent escape(QEvent::KeyPress, Qt::Key_Escape, Qt::NoModifier);
    QApplication::sendEvent(&dialog, &escape);
    EXPECT_EQ(calls, 0);
    dialog.show();
    click(dialog, "insertObject");
    EXPECT_EQ(calls, 1);
    EXPECT_FALSE(dialog.isVisible());
}

TEST_F(InsertionTest, ReturnInMethodListSubmitsOnlyOnce) {
    InsertObjectDialog dialog(preferences());
    int calls = 0;
    dialog.insert = [&](InsertObjectRequest) { ++calls; };
    dialog.show();
    auto *tree = dialog.findChild<QTreeWidget *>("insertObjectMethods");
    tree->setFocus();
    QApplication::processEvents();
    QKeyEvent enter(QEvent::KeyPress, Qt::Key_Return, Qt::NoModifier);
    QApplication::sendEvent(tree, &enter);
    EXPECT_EQ(calls, 1);
    EXPECT_FALSE(dialog.isVisible());
}

TEST_F(InsertionTest, SensorRequiresExplicitExistingParentAndHasOnlyTwoMethods) {
    InsertObjectDialog dialog(preferences());
    dialog.select_type(InsertObjectType::sensor);
    auto *insert = dialog.findChild<QPushButton *>("insertObject");
    EXPECT_FALSE(insert->isEnabled());
    dialog.set_parents(
        {{"sat-a", "A", ObjectKind::satellite}, {"sat-b", "B", ObjectKind::satellite}}, "sat-a");
    EXPECT_TRUE(insert->isEnabled());
    EXPECT_EQ(dialog.selection()->parent_id, "sat-a");
    auto *methods = dialog.findChild<QTreeWidget *>("insertObjectMethods");
    int count = 0;
    for (int i = 0; i < methods->topLevelItemCount(); ++i)
        count += !methods->topLevelItem(i)->isHidden();
    EXPECT_EQ(count, 2);
    dialog.set_parents({{"sat-b", "B", ObjectKind::satellite}});
    EXPECT_FALSE(insert->isEnabled());
    EXPECT_EQ(dialog.selection()->parent_id, "sat-a");
    dialog.set_parents({{"sat-b", "B", ObjectKind::satellite}}, "sat-b");
    EXPECT_TRUE(insert->isEnabled());
}

TEST_F(InsertionTest, NarrowLayoutKeepsActionsReachableAndPreservesSelection) {
    InsertObjectDialog dialog(preferences());
    dialog.set_scenario("<b>A long scenario name</b>");
    dialog.select_type(InsertObjectType::sensor);
    dialog.show();
    for (const QSize size : {QSize(680, 440), QSize(360, 300), QSize(680, 440)}) {
        dialog.resize(size);
        for (int i = 0; i < 4; ++i)
            QApplication::processEvents();
        auto *button = dialog.findChild<QPushButton *>("insertObject");
        EXPECT_TRUE(
            dialog.rect().contains(QRect(button->mapTo(&dialog, QPoint{}), button->size())));
        EXPECT_EQ(dialog.findChild<QScrollArea *>("insertOptionsScroll")
                      ->horizontalScrollBar()
                      ->maximum(),
                  0);
        EXPECT_EQ(dialog.selection()->type, InsertObjectType::sensor);
        EXPECT_TRUE(button->parentWidget()->rect().contains(button->geometry()));
    }
    EXPECT_EQ(dialog.findChild<QLabel *>("insertDestination")->textFormat(), Qt::PlainText);
}

TEST_F(InsertionTest, PreferenceWriteFailureIsVisibleAndDoesNotChangeDefault) {
    QFile file(temporary.filePath("blocked"));
    ASSERT_TRUE(file.open(QIODevice::WriteOnly));
    file.close();
    InsertObjectDialog dialog(file.fileName() + "/preferences.ini");
    method(dialog, InsertObjectMethod::defaults);
    click(dialog, "saveDefaultInsertMethod");
    EXPECT_EQ(dialog.default_method(InsertObjectType::satellite),
              InsertObjectMethod::define_properties);
    EXPECT_EQ(dialog.findChild<QLabel *>("insertObjectResult")->text(),
              "无法保存添加偏好，请检查配置目录是否可写。");
}

const QString valid_tle = "1 00005U 58002B   00179.78495062  .00000023  00000-0  28098-4 0  4753\n"
                          "2 00005  34.2682 348.7242 1859667 331.7664  19.3264 10.82419157413667";
TEST_F(InsertionTest, SingleTleImportValidatesBeforePublishing) {
    const auto plain = parse_single_tle(valid_tle);
    const auto named =
        parse_single_tle(QChar(0xfeff) + QString("VANGUARD\r\n") + valid_tle + "\r\n");
    EXPECT_EQ(plain.line1, named.line1);
    EXPECT_EQ(plain.line2, named.line2);
    auto bad = valid_tle;
    bad[68] = '0';
    EXPECT_THROW((void)parse_single_tle(bad), std::exception);
    EXPECT_THROW((void)parse_single_tle(valid_tle + "\n" + valid_tle), std::exception);
    EXPECT_THROW((void)parse_single_tle(QString(4097, 'x')), std::exception);
    QFile file(temporary.filePath("satellite.tle"));
    ASSERT_TRUE(file.open(QIODevice::WriteOnly));
    file.write(valid_tle.toUtf8());
    file.close();
    EXPECT_EQ(read_single_tle_file(file.fileName()).line1, plain.line1);
    EXPECT_THROW((void)read_single_tle_file(temporary.filePath("missing.tle")), std::exception);
}

TEST_F(InsertionTest, WizardCircularAndPolarStatesMatchIndependentGeometry) {
    const auto &earth = core::central_body(core::OriginId::earth);
    const double r = earth.equatorial_radius_m + 500000;
    for (int type : {0, 1}) {
        OrbitWizardDialog wizard(core::Epoch{12345});
        wizard.findChild<QuantityEditor *>("wizardInclination")
            ->set_value(type == 1 ? 90 : 53, Unit::degree);
        click(wizard, "acceptOrbitWizard");
        ASSERT_TRUE(wizard.input());
        const auto &state = wizard.input()->initial;
        EXPECT_EQ(state.epoch, core::Epoch{12345});
        EXPECT_NEAR(state.position_m[0], r, 1e-7);
        EXPECT_NEAR(state.position_m[1], 0, 1e-7);
        EXPECT_NEAR(state.position_m[2], 0, 1e-7);
        const double v = std::sqrt(earth.mu.metres_cubed_per_second_squared / r);
        const double inclination = type == 1 ? std::numbers::pi / 2 : 53 * std::numbers::pi / 180;
        EXPECT_NEAR(state.velocity_mps[1], v * std::cos(inclination), 1e-7);
        EXPECT_NEAR(state.velocity_mps[2], v * std::sin(inclination), 1e-7);
    }
}

TEST_F(InsertionTest, WizardCustomUnitsValidationAndCancellation) {
    OrbitWizardDialog wizard(core::Epoch{0});
    auto *type = wizard.findChild<QComboBox *>("orbitWizardPreset");
    type->setCurrentIndex(5);
    auto *a = wizard.findChild<QuantityEditor *>("wizardSemimajor");
    auto *e = wizard.findChild<QuantityEditor *>("wizardEccentricity");
    a->set_value(7000000, Unit::metre);
    a->set_unit(Unit::metre);
    e->set_value(0.2, Unit::one);
    click(wizard, "acceptOrbitWizard");
    EXPECT_FALSE(wizard.input());
    EXPECT_FALSE(wizard.findChild<QLabel *>("orbitWizardError")->text().isEmpty());
    e->set_value(0.01, Unit::one);
    type->setCurrentIndex(0);
    type->setCurrentIndex(5);
    EXPECT_DOUBLE_EQ(a->value(Unit::metre), 7000000);
    click(wizard, "acceptOrbitWizard");
    ASSERT_TRUE(wizard.input());
    const auto &earth = core::central_body(core::OriginId::earth);
    const auto elements = runtime::to_keplerian(wizard.input()->initial, earth.mu);
    EXPECT_NEAR(elements.semi_major_axis_m, 7000000, 1e-6);
    EXPECT_NEAR(elements.eccentricity, 0.01, 1e-12);
    OrbitWizardDialog cancelled(core::Epoch{0});
    cancelled.reject();
    EXPECT_FALSE(cancelled.input());
}

TEST_F(InsertionTest, WizardSlidersPreservePreciseInputUnitsAndHiddenDrafts) {
    OrbitWizardDialog wizard(core::Epoch{0});
    auto *height = wizard.findChild<QuantityEditor *>("wizardAltitude");
    auto *height_slider = wizard.findChild<QSlider *>("wizardAltitudeSlider");
    auto *raan = wizard.findChild<QuantityEditor *>("wizardRaan");
    auto *raan_slider = wizard.findChild<QSlider *>("wizardRaanSlider");
    ASSERT_NE(height_slider, nullptr);
    ASSERT_NE(raan_slider, nullptr);
    height_slider->setValue(750);
    EXPECT_DOUBLE_EQ(height->value(Unit::metre), 750000);
    const double precise_height = 35786123.456789;
    height->set_value(precise_height, Unit::metre);
    height->set_unit(Unit::metre);
    EXPECT_NEAR(height->value(Unit::metre), precise_height, 1e-7);
    EXPECT_GE(height_slider->maximum(), 35787);
    EXPECT_TRUE(height_slider->toolTip().contains("m"));
    height_slider->setValue(36000);
    EXPECT_DOUBLE_EQ(height->value(Unit::metre), 36000000);
    EXPECT_EQ(height->unit(), Unit::metre);
    const double precise_angle = 1.23456789123;
    raan->set_value(precise_angle, Unit::radian);
    raan->set_unit(Unit::radian);
    EXPECT_NEAR(raan->value(Unit::radian), precise_angle, 1e-12);
    raan_slider->setValue(900);
    EXPECT_NEAR(raan->value(Unit::radian), std::numbers::pi / 2, 1e-12);
    EXPECT_EQ(raan->unit(), Unit::radian);
    height->findChild<QLineEdit *>("quantityValue")->setText("-");
    auto *type = wizard.findChild<QComboBox *>("orbitWizardPreset");
    type->setCurrentIndex(5);
    EXPECT_TRUE(height_slider->isHidden() || !height_slider->isVisibleTo(&wizard));
    type->setCurrentIndex(0);
    EXPECT_EQ(height->findChild<QLineEdit *>("quantityValue")->text(), "-");
    height_slider->setValue(800);
    EXPECT_DOUBLE_EQ(height->value(Unit::metre), 800000);
    EXPECT_TRUE(wizard.findChild<QLabel *>("orbitWizardError")->text().isEmpty());

    wizard.show();
    QApplication::processEvents();
    height->setFocus();
    QWheelEvent wheel(QPointF(5, 5), QPointF(5, 5), {}, QPoint(0, 120), Qt::NoButton,
                      Qt::NoModifier, Qt::NoScrollPhase, false);
    QApplication::sendEvent(height_slider, &wheel);
    EXPECT_DOUBLE_EQ(height->value(Unit::metre), 800000);
    EXPECT_FALSE(wheel.isAccepted());
    raan_slider->setFocus();
    QKeyEvent right(QEvent::KeyPress, Qt::Key_Right, Qt::NoModifier);
    QApplication::sendEvent(raan_slider, &right);
    EXPECT_NEAR(raan->value(Unit::degree), 90.1, 1e-10);
}

TEST_F(InsertionTest, WizardNineTypesProduceValidDraftsAndMatchingPropagators) {
    for (int index = 0; index < 9; ++index) {
        OrbitWizardDialog wizard(core::Epoch{42});
        auto *type = wizard.findChild<QComboBox *>("orbitWizardPreset");
        ASSERT_EQ(type->count(), 9);
        type->setCurrentIndex(index);
        click(wizard, "acceptOrbitWizard");
        ASSERT_TRUE(wizard.input())
            << index << ": "
            << wizard.findChild<QLabel *>("orbitWizardError")->text().toStdString();
        EXPECT_EQ(wizard.input()->initial.epoch, core::Epoch{42});
        EXPECT_EQ(wizard.input()->propagator, (index == 0 || index == 3 || index == 5)
                                                  ? application::OrbitPropagator::two_body
                                                  : application::OrbitPropagator::j2_secular);
    }
}

TEST_F(InsertionTest, WizardNoSolutionDoesNotPublishAndInactiveDraftIsPreserved) {
    OrbitWizardDialog wizard(core::Epoch{0});
    auto *type = wizard.findChild<QComboBox *>("orbitWizardPreset");
    type->setCurrentIndex(8);
    auto *height = wizard.findChild<QuantityEditor *>("wizardAltitude");
    height->set_value(50000, Unit::kilometre);
    click(wizard, "acceptOrbitWizard");
    EXPECT_FALSE(wizard.input());
    EXPECT_TRUE(wizard.findChild<QLabel *>("orbitWizardError")->text().contains("没有可行解"));
    // An incomplete hidden height must not invalidate the custom orbit.
    height->findChild<QLineEdit *>("quantityValue")->setText("-");
    type->setCurrentIndex(5);
    click(wizard, "acceptOrbitWizard");
    ASSERT_TRUE(wizard.input());
    type->setCurrentIndex(8);
    EXPECT_EQ(height->findChild<QLineEdit *>("quantityValue")->text(), "-");
}

TEST_F(InsertionTest, WizardSunDriverRepeatRatioAndNarrowLayout) {
    OrbitWizardDialog wizard(core::Epoch{0});
    auto *type = wizard.findChild<QComboBox *>("orbitWizardPreset");
    type->setCurrentIndex(8);
    wizard.findChild<QComboBox *>("wizardSunDriver")->setCurrentIndex(1);
    wizard.findChild<QuantityEditor *>("wizardSunInclination")->set_value(98, Unit::degree);
    auto *preview = wizard.findChild<QLabel *>("orbitWizardPreview");
    EXPECT_TRUE(preview->text().contains("98.000000"));
    type->setCurrentIndex(7);
    wizard.findChild<QSpinBox *>("wizardRepeatRevolutions")->setValue(233);
    wizard.findChild<QSpinBox *>("wizardRepeatDays")->setValue(16);
    EXPECT_TRUE(preview->text().contains("重复周期"));
    wizard.show();
    for (const QSize size : {QSize(980, 590), QSize(360, 300), QSize(980, 590)}) {
        wizard.resize(size);
        for (int iteration = 0; iteration < 5; ++iteration)
            QApplication::processEvents();
        auto *next = wizard.findChild<QPushButton *>("acceptOrbitWizard");
        EXPECT_TRUE(wizard.rect().contains(QRect(next->mapTo(&wizard, QPoint{}), next->size())));
        EXPECT_EQ(
            wizard.findChild<QScrollArea *>("orbitWizardScroll")->horizontalScrollBar()->maximum(),
            0);
        EXPECT_EQ(wizard.findChild<QSpinBox *>("wizardRepeatRevolutions")->value(), 233);
        auto *parameters = wizard.findChild<QWidget *>("orbitWizardParameterScroll");
        auto *summary = wizard.findChild<QWidget *>("orbitWizardSummary");
        if (size.width() == 360)
            EXPECT_GE(summary->geometry().top(), parameters->geometry().bottom());
        else
            EXPECT_GE(summary->geometry().left(), parameters->geometry().right());
    }
    type->setCurrentIndex(5);
    wizard.resize(980, 500);
    for (int iteration = 0; iteration < 8; ++iteration)
        QApplication::processEvents();
    auto *parameter_scroll = wizard.findChild<QScrollArea *>("orbitWizardParameterScroll");
    EXPECT_GT(parameter_scroll->verticalScrollBar()->maximum(), 0);
    EXPECT_EQ(parameter_scroll->horizontalScrollBar()->maximum(), 0);
    auto *outer_scroll = wizard.findChild<QScrollArea *>("orbitWizardScroll");
    EXPECT_EQ(outer_scroll->verticalScrollBar()->maximum(), 0);
    EXPECT_EQ(outer_scroll->horizontalScrollBar()->maximum(), 0);
}

TEST_F(InsertionTest, WizardMapUsesRealFramesAndClearsInvalidOrSupersededPreviews) {
    std::shared_ptr<const core::IFrameProvider> frames = runtime::make_frame_provider();
    std::shared_ptr<const core::IEarthOrientationProvider> eop =
        runtime::load_eop_snapshot(ORBITFORGE_TEST_EOP);
    const auto time = runtime::make_builtin_time_provider();
    OrbitWizardDialog wizard(time->from_utc({2026, 10, 1, 0, 0, 0}));
    QImage imagery(360, 180, QImage::Format_RGB32);
    imagery.fill(Qt::darkBlue);
    wizard.set_preview_context(imagery, frames, eop);
    wizard.show();
    auto *preview =
        static_cast<OrbitDesignPreview *>(wizard.findChild<QWidget *>("orbitDesignPreview"));
    const auto await_preview = [&] {
        QElapsedTimer timer;
        timer.start();
        while (!preview->ready() && timer.elapsed() < 5000) {
            QApplication::processEvents();
            QThread::msleep(5);
        }
        return preview->ready();
    };
    auto *raan = wizard.findChild<QuantityEditor *>("wizardRaan");
    raan->set_value(45, Unit::degree);
    raan->set_value(120, Unit::degree);
    ASSERT_TRUE(await_preview());
    auto *map = static_cast<GroundMap *>(wizard.findChild<QWidget *>("wizardGroundMap"));
    ASSERT_EQ(map->track_count(), 1u);
    const double radius = 6878136.6, angle = 120 * std::numbers::pi / 180;
    const auto initial =
        frames
            ->gcrf_to_itrf(time->from_utc({2026, 10, 1, 0, 0, 0}),
                           eop->at(time->from_utc({2026, 10, 1, 0, 0, 0})).sample)
            .apply_position({radius * std::cos(angle), radius * std::sin(angle), 0});
    const auto expected = ground_coordinates(initial);
    EXPECT_NEAR(map->marker_coordinates(0).x(), expected.x(), 1e-8);
    EXPECT_NEAR(map->marker_coordinates(0).y(), expected.y(), 1e-8);
    const auto *identity = map;
    raan->set_value(125, Unit::degree);
    EXPECT_FALSE(preview->ready());
    EXPECT_EQ(map->track_count(), 1u);
    const auto stable_hint = wizard.findChild<QLabel *>("orbitPreviewStatus")->text();
    const auto stable_bounds = map->geometry();
    ASSERT_TRUE(await_preview());
    EXPECT_EQ(wizard.findChild<QLabel *>("orbitPreviewStatus")->text(), stable_hint);
    EXPECT_EQ(map->geometry(), stable_bounds);
    wizard.resize(360, 300);
    QApplication::processEvents();
    EXPECT_EQ(wizard.findChild<QWidget *>("wizardGroundMap"), identity);
    raan->findChild<QLineEdit *>("quantityValue")->setText("-");
    EXPECT_FALSE(preview->ready());
    EXPECT_EQ(map->track_count(), 0u);
    raan->set_value(30, Unit::degree);
    ASSERT_TRUE(await_preview());
    wizard.findChild<QComboBox *>("wizardPreviewRevolutions")->setCurrentIndex(1);
    ASSERT_TRUE(await_preview());
    // A closing dialog cancels work and never publishes a satellite input.
    raan->set_value(60, Unit::degree);
    wizard.reject();
    EXPECT_FALSE(wizard.input());
}
TEST_F(InsertionTest, PreviewCachesExactRotationsAndRefinesLatestDesignOnly) {
    class CountingFrames final : public core::IFrameProvider {
      public:
        core::PositionRotation
        gcrf_to_itrf(core::Epoch epoch, const core::EarthOrientationSample &eop) const override {
            ++calls;
            if (QThread::currentThread() == gui_thread)
                called_on_gui = true;
            if (pause_next.exchange(false)) {
                waiting = true;
                while (!resume.load())
                    QThread::msleep(1);
            }
            return source->gcrf_to_itrf(epoch, eop);
        }
        core::StateTransform
        gcrf_to_itrf_state(core::Epoch epoch,
                           const core::EarthOrientationState &eop) const override {
            return source->gcrf_to_itrf_state(epoch, eop);
        }
        std::shared_ptr<const core::IFrameProvider> source = runtime::make_frame_provider();
        QThread *gui_thread = QThread::currentThread();
        mutable std::atomic<int> calls{0};
        mutable std::atomic_bool called_on_gui{false};
        mutable std::atomic_bool pause_next{false}, waiting{false}, resume{false};
    };
    const auto frames = std::make_shared<CountingFrames>();
    const std::shared_ptr<const core::IEarthOrientationProvider> eop =
        runtime::load_eop_snapshot(ORBITFORGE_TEST_EOP);
    const auto time = runtime::make_builtin_time_provider();
    core::EarthOrbitDesignRequest request;
    request.epoch = time->from_utc({2026, 10, 1, 0, 0, 0});
    QImage imagery(360, 180, QImage::Format_RGB32);
    imagery.fill(Qt::darkBlue);
    OrbitDesignPreview preview;
    frames->pause_next = true;
    preview.set_context(imagery, frames, eop);
    preview.set_design(core::design_earth_orbit(request));
    preview.show();
    auto *map = static_cast<GroundMap *>(preview.findChild<QWidget *>("wizardGroundMap"));
    const auto await_full = [&] {
        QElapsedTimer timer;
        timer.start();
        while (!preview.ready() && timer.elapsed() < 5000) {
            QApplication::processEvents();
            QThread::msleep(1);
        }
        EXPECT_TRUE(preview.ready());
    };
    QElapsedTimer prepare;
    prepare.start();
    while (!frames->waiting.load() && prepare.elapsed() < 5000) {
        QApplication::processEvents();
        QThread::msleep(1);
    }
    for (int i = 0; i < 8; ++i) {
        request.raan_rad = 0.1 * i;
        preview.set_design(core::design_earth_orbit(request));
        QApplication::processEvents();
    }
    frames->resume = true;
    EXPECT_TRUE(frames->waiting.load());
    await_full();
    ASSERT_EQ(map->track_coordinates(0).size(), 361u);
    const int original_calls = frames->calls.load();
    // Continuous input must not cancel/restart geometry-independent preparation.
    EXPECT_EQ(original_calls, 257 + 361);
    request.raan_rad = 1.2;
    preview.set_design(core::design_earth_orbit(request));
    await_full();
    EXPECT_EQ(frames->calls.load(), original_calls);
    // An identical physical input must not invalidate an already complete preview.
    preview.set_design(core::design_earth_orbit(request));
    EXPECT_TRUE(preview.ready());

    request.altitude_m = 600000;
    frames->waiting = false;
    frames->resume = false;
    frames->pause_next = true;
    preview.set_design(core::design_earth_orbit(request));
    QElapsedTimer pending;
    pending.start();
    while (!frames->waiting.load() && pending.elapsed() < 5000) {
        QApplication::processEvents();
        QThread::msleep(1);
    }
    // Even while detailed sampling is blocked, every event turn projects the latest
    // input from exact cached rotations. No worker result is needed to follow edits.
    for (int i = 0; i < 30; ++i) {
        request.altitude_m = 650000 + i * 1000;
        request.raan_rad = 0.01 * i;
        const auto moving = core::design_earth_orbit(request);
        preview.set_design(moving);
        QApplication::processEvents();
        const auto initial =
            runtime::to_cartesian(moving.elements, core::central_body(core::OriginId::earth).mu);
        const auto expected = ground_coordinates(
            frames->source->gcrf_to_itrf(request.epoch, eop->at(request.epoch).sample)
                .apply_position(initial.position_m));
        EXPECT_NEAR(map->marker_coordinates(0).x(), expected.x(), 1e-8);
        EXPECT_NEAR(map->marker_coordinates(0).y(), expected.y(), 1e-8);
    }
    request.altitude_m = 900000;
    const auto design = core::design_earth_orbit(request);
    preview.set_design(design);
    frames->resume = true;
    EXPECT_TRUE(frames->waiting.load());
    await_full();
    EXPECT_GT(frames->calls.load(), original_calls);
    ASSERT_EQ(map->track_coordinates(0).size(), 361u);
    const auto session = runtime::make_two_body_session(
        runtime::to_cartesian(design.elements, core::central_body(core::OriginId::earth).mu),
        core::central_body(core::OriginId::earth).mu, "reference");
    for (int index : {0, 71, 170, 359, 360}) {
        const core::Epoch epoch{request.epoch.tai_nanoseconds() +
                                static_cast<std::int64_t>(
                                    std::llround(design.nodal_period_seconds * index / 360 * 1e9))};
        const auto state = session.sample(std::array{epoch}).states().front();
        const auto expected =
            ground_coordinates(frames->source->gcrf_to_itrf(epoch, eop->at(epoch).sample)
                                   .apply_position(state.position_m));
        EXPECT_NEAR(map->track_coordinates(0)[index].x(), expected.x(), 1e-8);
        EXPECT_NEAR(map->track_coordinates(0)[index].y(), expected.y(), 1e-8);
    }
    // Provider replacement invalidates both grids even with identical epoch/geometry.
    const auto replacement = std::make_shared<CountingFrames>();
    preview.set_context(imagery, replacement, eop);
    await_full();
    EXPECT_GT(replacement->calls.load(), 0);
    preview.findChild<QComboBox *>("wizardPreviewRevolutions")->setCurrentIndex(1);
    await_full();
    EXPECT_EQ(map->track_coordinates(0).size(), 1081u);
    request.raan_rad = 2;
    preview.set_design(core::design_earth_orbit(request));
    preview.set_design(std::nullopt);
    QApplication::processEvents();
    EXPECT_FALSE(preview.ready());
    EXPECT_EQ(map->track_count(), 0u);
    EXPECT_FALSE(frames->called_on_gui.load());
}

TEST_F(InsertionTest, PreviewPrefetchDoesNotRequireDataBeyondTheRequestedView) {
    class LimitedEop final : public core::IEarthOrientationProvider {
      public:
        core::EarthOrientationState at(core::Epoch epoch) const override {
            if (epoch.tai_nanoseconds() > end.tai_nanoseconds())
                throw core::EopCoverageError("End of test coverage");
            return source->at(epoch);
        }
        const core::EopDataInfo &data_info() const noexcept override { return source->data_info(); }
        std::shared_ptr<const core::IEarthOrientationProvider> source =
            runtime::load_eop_snapshot(ORBITFORGE_TEST_EOP);
        core::Epoch end{0};
    };
    const auto eop = std::make_shared<LimitedEop>();
    core::EarthOrbitDesignRequest request;
    request.epoch = runtime::make_builtin_time_provider()->from_utc({2026, 10, 1, 0, 0, 0});
    const auto design = core::design_earth_orbit(request);
    eop->end =
        core::Epoch{request.epoch.tai_nanoseconds() +
                    static_cast<std::int64_t>(std::llround(design.nodal_period_seconds * 1e9))};
    QImage imagery(360, 180, QImage::Format_RGB32);
    imagery.fill(Qt::darkBlue);
    OrbitDesignPreview preview;
    preview.set_context(imagery, runtime::make_frame_provider(), eop);
    preview.set_design(design);
    preview.show();
    QElapsedTimer timer;
    timer.start();
    while (!preview.ready() && timer.elapsed() < 5000) {
        QApplication::processEvents();
        QThread::msleep(1);
    }
    ASSERT_TRUE(preview.ready());
    auto *map = static_cast<GroundMap *>(preview.findChild<QWidget *>("wizardGroundMap"));
    EXPECT_EQ(map->track_coordinates(0).size(), 361u);
    // A longer requested interval still fails instead of extending actual coverage.
    request.altitude_m += 1000000;
    preview.set_design(core::design_earth_orbit(request));
    timer.restart();
    while (map->track_count() && timer.elapsed() < 5000) {
        QApplication::processEvents();
        QThread::msleep(1);
    }
    EXPECT_FALSE(preview.ready());
    EXPECT_EQ(map->track_count(), 0u);
}

TEST_F(InsertionTest, PreviewDestructionDiscardsQueuedWorkerCompletion) {
    class PausedEop final : public core::IEarthOrientationProvider {
      public:
        core::EarthOrientationState at(core::Epoch epoch) const override {
            entered = true;
            while (!resume.load())
                QThread::msleep(1);
            return source->at(epoch);
        }
        const core::EopDataInfo &data_info() const noexcept override { return source->data_info(); }
        std::shared_ptr<const core::IEarthOrientationProvider> source =
            runtime::load_eop_snapshot(ORBITFORGE_TEST_EOP);
        mutable std::atomic_bool entered{false}, resume{false};
    };
    const auto eop = std::make_shared<PausedEop>();
    core::EarthOrbitDesignRequest request;
    request.epoch = runtime::make_builtin_time_provider()->from_utc({2026, 10, 1, 0, 0, 0});
    QImage imagery(360, 180, QImage::Format_RGB32);
    imagery.fill(Qt::darkBlue);
    auto preview = std::make_unique<OrbitDesignPreview>();
    preview->set_context(imagery, runtime::make_frame_provider(), eop);
    preview->set_design(core::design_earth_orbit(request));
    QElapsedTimer timer;
    timer.start();
    while (!eop->entered.load() && timer.elapsed() < 5000) {
        QApplication::processEvents();
        QThread::msleep(1);
    }
    const bool worker_started = eop->entered.load();
    // Do not deliver any GUI event between releasing the worker and destroying
    // its context. The destructor must join it, then discard its queued result.
    eop->resume = true;
    preview.reset();
    EXPECT_TRUE(worker_started);
    QApplication::sendPostedEvents(nullptr, QEvent::MetaCall);
    QApplication::processEvents();
}

// Explicit interaction profile, excluded from CI timing gates. Run with
// --gtest_also_run_disabled_tests --gtest_filter=*WizardPreviewInteractionProfile.
TEST_F(InsertionTest, DISABLED_WizardPreviewInteractionProfile) {
    const auto output = qEnvironmentVariable("ORBITFORGE_PREVIEW_PROFILE");
    std::unique_ptr<WorkbenchProfile> profile;
    if (!output.isEmpty())
        profile = std::make_unique<WorkbenchProfile>(output);
    const auto time = runtime::make_builtin_time_provider();
    OrbitWizardDialog wizard(time->from_utc({2026, 10, 1, 0, 0, 0}));
    QImage imagery(360, 180, QImage::Format_RGB32);
    imagery.fill(Qt::darkBlue);
    wizard.set_preview_context(imagery, runtime::make_frame_provider(),
                               runtime::load_eop_snapshot(ORBITFORGE_TEST_EOP));
    wizard.show();
    auto *preview =
        static_cast<OrbitDesignPreview *>(wizard.findChild<QWidget *>("orbitDesignPreview"));
    auto *map = static_cast<GroundMap *>(wizard.findChild<QWidget *>("wizardGroundMap"));
    class PaintObserver final : public QObject {
      public:
        explicit PaintObserver(GroundMap *map) : map_(map) { map->installEventFilter(this); }
        int changes = 0;
        QElapsedTimer gap;
        std::vector<double> gaps;

      protected:
        bool eventFilter(QObject *, QEvent *event) override {
            if (event->type() == QEvent::Paint && map_->track_count()) {
                const auto point = map_->track_coordinates(0).back();
                if (point != previous_) {
                    previous_ = point;
                    ++changes;
                    if (gap.isValid())
                        gaps.push_back(gap.nsecsElapsed() * 1e-6);
                    gap.start();
                }
            }
            return false;
        }

      private:
        GroundMap *map_;
        QPointF previous_;
    } paints(map);
    const auto wait = [&] {
        QElapsedTimer timer;
        timer.start();
        while ((!preview->ready() || !map->tracks_ready() || map->tracks_transitioning()) &&
               timer.elapsed() < 5000) {
            QApplication::processEvents();
            QThread::msleep(1);
        }
        EXPECT_TRUE(preview->ready());
        EXPECT_TRUE(map->tracks_ready());
        EXPECT_FALSE(map->tracks_transitioning());
        return timer.nsecsElapsed() * 1e-6;
    };
    for (const char *parameter : {"wizardRaan", "wizardAltitude"}) {
        auto *field = wizard.findChild<QuantityEditor *>(parameter);
        const auto unit = field->unit();
        for (int circles : {0, 1}) {
            wizard.findChild<QComboBox *>("wizardPreviewRevolutions")->setCurrentIndex(circles);
            wait();
            std::vector<double> latency;
            for (int index = 0; index < 12; ++index) {
                const auto previous = map->track_coordinates(0).back();
                QElapsedTimer input;
                input.start();
                field->set_value(200 + index, unit);
                while ((map->track_coordinates(0).back() == previous || !map->tracks_ready()) &&
                       input.elapsed() < 5000) {
                    QApplication::processEvents();
                    QThread::msleep(1);
                }
                latency.push_back(input.nsecsElapsed() * 1e-6);
                wait();
            }
            std::sort(latency.begin(), latency.end());
            for (const int input_interval : {16, 8, 4}) {
                if (profile)
                    profile->phase(QStringLiteral("%1-%2rev-%3ms")
                                       .arg(parameter)
                                       .arg(circles == 0 ? 1 : 3)
                                       .arg(input_interval),
                                   true);
                paints.changes = 0;
                paints.gaps.clear();
                paints.gap.start();
                int changes = 0, inputs = 0;
                auto marker = map->track_coordinates(0).back();
                QElapsedTimer drag;
                drag.start();
                while (drag.elapsed() < 1600) {
                    if (drag.elapsed() >= inputs * input_interval) {
                        const ProfileScope edit("input_handler_ms");
                        field->set_value(250 + inputs % 100, unit);
                        ++inputs;
                    }
                    QApplication::processEvents();
                    const auto next = map->track_coordinates(0).back();
                    if (next != marker && map->tracks_ready()) {
                        ++changes;
                        marker = next;
                    }
                    QThread::msleep(1);
                }
                const auto paint_max =
                    paints.gaps.empty() ? 0
                                        : *std::max_element(paints.gaps.begin(), paints.gaps.end());
                const auto paint_count = paints.changes;
                const auto settle_ms = wait();
                std::cout << "preview_profile parameter=" << parameter
                          << " circles=" << (circles == 0 ? 1 : 3)
                          << " visible_p50_ms=" << latency[6]
                          << " visible_max_ms=" << latency.back() << " drag_inputs=" << inputs
                          << " input_interval_ms=" << input_interval
                          << " rendered_updates=" << changes << " paint_updates=" << paint_count
                          << " max_paint_gap_ms=" << paint_max << " release_settle_ms=" << settle_ms
                          << '\n';
            }
        }
    }
    if (profile)
        profile->finish({{"kind", "orbit-wizard-input-and-paint"}});
}
} // namespace orbitforge::desktop
