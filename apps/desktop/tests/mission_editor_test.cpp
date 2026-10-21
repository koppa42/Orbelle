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
#include <QTabBar>
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
TEST_F(QuantityTest, MissionEditorValidatesSequencePreservesDraftAndCancelsWithoutApplying) {
    apply_workbench_style(*qobject_cast<QApplication *>(QApplication::instance()));
    OrbitEditor editor(OrbitInput{});
    editor.resize(1100, 760);
    editor.show();
    editor.set_propagator(OrbitPropagator::mission);
    std::optional<OrbitInput> submitted;
    editor.apply = [&](OrbitInput input) { submitted = std::move(input); };
    const auto capture = qEnvironmentVariable("ORBITFORGE_MISSION_UI_CAPTURE");
    QTimer::singleShot(0, &editor, [&] {
        auto *dialog = editor.findChild<QDialog *>("missionEditorDialog");
        ASSERT_NE(dialog, nullptr);
        auto *list = dialog->findChild<QListWidget *>("missionSegments");
        ASSERT_EQ(list->count(), 1);
        auto *duration = dialog->findChild<QuantityEditor *>("missionCoastDuration");
        duration->set_value(600, Unit::second);
        dialog->findChild<QPushButton *>("missionAddImpulse")->click();
        dialog->findChild<QLineEdit *>("missionSegmentName")->setText(QStringLiteral("升轨点火"));
        dialog->findChild<QuantityEditor *>("missionDeltaV0")
            ->set_value(120, Unit::metres_per_second);
        dialog->findChild<QComboBox *>("missionManeuverFrame")->setCurrentIndex(1);
        dialog->findChild<QPushButton *>("missionMoveUp")->click();
        EXPECT_EQ(list->currentRow(), 0);
        dialog->findChild<QPushButton *>("missionMoveDown")->click();
        EXPECT_EQ(list->currentRow(), 1);
        dialog->findChild<QPushButton *>("missionAddCoast")->click();
        auto *ok = dialog->findChild<QDialogButtonBox *>()->button(QDialogButtonBox::Ok);
        ok->click();
        EXPECT_TRUE(dialog->isVisible());
        EXPECT_TRUE(dialog->findChild<QLabel *>("errorText")->isVisible());
        text(*duration)->setText("unfinished");
        list->setCurrentRow(1);
        EXPECT_EQ(list->currentRow(), 2);
        duration->set_value(6600, Unit::second);
        list->setCurrentRow(1);
        EXPECT_EQ(list->currentRow(), 1);
        if (!capture.isEmpty()) {
            ASSERT_TRUE(QDir{}.mkpath(capture));
            const auto settled = [] {
                QEventLoop loop;
                QTimer::singleShot(80, &loop, &QEventLoop::quit);
                loop.exec();
            };
            settled();
            EXPECT_TRUE(dialog->grab().save(capture + "/mission-wide.png"));
            dialog->resize(440, 760);
            settled();
            EXPECT_TRUE(dialog->grab().save(capture + "/mission-narrow.png"));
            for (auto *scroll : dialog->findChildren<QScrollArea *>())
                EXPECT_EQ(scroll->horizontalScrollBar()->maximum(), 0);
            EXPECT_TRUE(ok->isVisible());
            dialog->resize(900, 690);
            settled();
            EXPECT_DOUBLE_EQ(dialog->findChild<QuantityEditor *>("missionDeltaV0")
                                 ->value(Unit::metres_per_second),
                             120);
        }
        ok->click();
    });
    editor.open_propagator_details();
    EXPECT_FALSE(submitted);
    editor.submit();
    ASSERT_TRUE(submitted);
    ASSERT_EQ(submitted->mission.segments.size(), 3U);
    EXPECT_EQ(submitted->mission.segments[1].delta_v_mps[0], 120);
    const auto saved = *submitted;
    editor.set_input(saved);
    QTimer::singleShot(0, &editor, [&] {
        auto *dialog = editor.findChild<QDialog *>("missionEditorDialog");
        ASSERT_NE(dialog, nullptr);
        EXPECT_EQ(dialog->findChild<QListWidget *>("missionSegments")->count(), 3);
        dialog->findChild<QPushButton *>("missionRemove")->click();
        dialog->reject();
    });
    editor.open_propagator_details();
    editor.submit();
    EXPECT_EQ(submitted->mission, saved.mission);
}

TEST_F(QuantityTest, MissionTimelineUpdatesExactBurnTimesAndRecoversFromIncompleteInput) {
    using namespace std::chrono_literals;
    apply_workbench_style(*qobject_cast<QApplication *>(QApplication::instance()));
    core::MissionPlan plan{
        core::MissionCoastModel::two_body,
        {core::MissionSegment::coast(600s + 1ns), core::MissionSegment::impulse({3, 4, 0}),
         core::MissionSegment::impulse({0, 0, 12}), core::MissionSegment::coast(6600s)}};
    core::HpopOptions hpop;
    QWidget parent;
    QTimer::singleShot(0, &parent, [&] {
        auto *dialog = parent.findChild<QDialog *>("missionEditorDialog");
        ASSERT_NE(dialog, nullptr);
        auto *list = dialog->findChild<QListWidget *>("missionSegments");
        auto *coverage = dialog->findChild<QLabel *>("missionCoverage");
        auto *timing = dialog->findChild<QLabel *>("missionSegmentTiming");
        auto *duration = dialog->findChild<QuantityEditor *>("missionCoastDuration");
        auto *error = dialog->findChild<QLabel *>("errorText");
        EXPECT_FALSE(coverage->property("warning").toBool());
        EXPECT_TRUE(list->item(1)->toolTip().contains("T+00:10:00.000000001"));
        EXPECT_TRUE(list->item(2)->toolTip().contains("T+00:10:00.000000001"));
        duration->set_value(1200, Unit::second);
        EXPECT_TRUE(list->item(1)->toolTip().contains("T+00:20:00"));
        list->setCurrentRow(1);
        EXPECT_TRUE(timing->text().contains("T+00:20:00"));
        EXPECT_TRUE(
            dialog->findChild<QLabel *>("missionImpulseMagnitude")->text().contains("5 m/s"));
        EXPECT_TRUE(dialog->findChild<QLabel *>("missionSummary")->text().contains("17 m/s"));
        dialog->findChild<QComboBox *>("missionManeuverFrame")->setCurrentIndex(2);
        EXPECT_TRUE(list->item(1)->toolTip().contains("LVLH"));
        auto *dv = dialog->findChild<QuantityEditor *>("missionDeltaV0");
        text(*dv)->setText("-");
        EXPECT_FALSE(error->isHidden());
        EXPECT_TRUE(coverage->text().contains(QStringLiteral("输入尚未完成")));
        list->setCurrentRow(2);
        EXPECT_EQ(list->currentRow(), 1);
        // Returning to the previous number emits no valueChanged signal.
        text(*dv)->setText("3");
        EXPECT_TRUE(error->isHidden());
        EXPECT_FALSE(coverage->property("warning").toBool());
        list->setCurrentRow(0);
        duration->set_value(1e-9, Unit::second);
        EXPECT_TRUE(coverage->property("warning").toBool());
        EXPECT_TRUE(coverage->text().contains(QStringLiteral("延长滑行段")));
        duration->set_value(600, Unit::second);
        EXPECT_FALSE(coverage->property("warning").toBool());
        auto *ok = dialog->findChild<QDialogButtonBox *>()->button(QDialogButtonBox::Ok);
        ok->click();
    });
    ASSERT_TRUE(edit_mission(plan, hpop, core::Epoch{0}, core::Epoch{0},
                             core::Epoch{7'200'000'000'000}, &parent));
    EXPECT_EQ(plan.segments[0].duration_nanoseconds, 600'000'000'000);
    EXPECT_EQ(plan.segments[1].frame, core::ManeuverFrame::lvlh);
}

TEST_F(QuantityTest, MissionCompactPagesPreserveUnitsAndDraftsAndHandleEmptySequence) {
    using namespace std::chrono_literals;
    apply_workbench_style(*qobject_cast<QApplication *>(QApplication::instance()));
    core::MissionPlan plan{core::MissionCoastModel::two_body,
                           {core::MissionSegment::coast(600s),
                            core::MissionSegment::impulse({120, 0, 0}, core::ManeuverFrame::vnc,
                                                          std::string(300, 'A')),
                            core::MissionSegment::coast(6600s)}};
    const auto original = plan;
    core::HpopOptions hpop;
    QWidget parent;
    const auto capture = qEnvironmentVariable("ORBITFORGE_MISSION_UI_CAPTURE");
    QTimer::singleShot(0, &parent, [&] {
        auto *dialog = parent.findChild<QDialog *>("missionEditorDialog");
        ASSERT_NE(dialog, nullptr);
        const auto settled = [] {
            QEventLoop loop;
            QTimer::singleShot(80, &loop, &QEventLoop::quit);
            loop.exec();
        };
        const auto screenshot = [&](const char *name) {
            if (!capture.isEmpty()) {
                EXPECT_TRUE(QDir{}.mkpath(capture));
                EXPECT_TRUE(dialog->grab().save(capture + '/' + name + ".png"));
            }
        };
        auto *list = dialog->findChild<QListWidget *>("missionSegments");
        auto *navigation = dialog->findChild<QTabBar *>("missionNavigation");
        auto *scroll = dialog->findChild<QScrollArea *>("missionParameters");
        auto *dv = dialog->findChild<QuantityEditor *>("missionDeltaV0");
        auto *ok = dialog->findChild<QDialogButtonBox *>()->button(QDialogButtonBox::Ok);
        list->setCurrentRow(1);
        dv->set_unit(Unit::kilometres_per_second);
        text(*dv)->setText("1e");
        for (const auto size : {QSize{980, 740}, QSize{400, 520}, QSize{440, 760}}) {
            dialog->resize(size);
            navigation->setCurrentIndex(1);
            settled();
            EXPECT_EQ(list->currentRow(), 1);
            EXPECT_EQ(dv->unit(), Unit::kilometres_per_second);
            EXPECT_EQ(text(*dv)->text(), "1e");
            EXPECT_EQ(scroll->horizontalScrollBar()->maximum(), 0);
            EXPECT_GT(scroll->viewport()->height(), 130);
            EXPECT_TRUE(dialog->rect().contains(ok->mapTo(dialog, ok->rect().bottomRight())));
            screenshot(size.width() == 400   ? "mission-short-invalid"
                       : size.width() == 440 ? "mission-narrow-invalid"
                                             : "mission-wide-long-name");
        }
        EXPECT_TRUE(navigation->isVisible());
        navigation->setCurrentIndex(0);
        settled();
        EXPECT_TRUE(list->isVisible());
        EXPECT_FALSE(scroll->isVisible());
        screenshot("mission-narrow-sequence");
        // Save returns to the invalid field rather than losing the hidden draft.
        ok->click();
        EXPECT_EQ(navigation->currentIndex(), 1);
        EXPECT_TRUE(text(*dv)->hasFocus());
        settled();
        EXPECT_TRUE(scroll->viewport()->rect().contains(
            text(*dv)->mapTo(scroll->viewport(), text(*dv)->rect().center())));
        text(*dv)->setText("0.12");
        dialog->resize(980, 740);
        settled();
        EXPECT_FALSE(navigation->isVisible());
        EXPECT_TRUE(list->isVisible());
        EXPECT_TRUE(scroll->isVisible());
        EXPECT_DOUBLE_EQ(dv->value(Unit::metres_per_second), 120);
        screenshot("mission-wide-restored");
        // List shortcuts reorder only the selected segment; Enter opens parameters,
        // without accidentally accepting the whole plan through QDialog's default button.
        const auto key = [&](int value, Qt::KeyboardModifiers modifiers = Qt::NoModifier) {
            QKeyEvent press(QEvent::KeyPress, value, modifiers);
            QKeyEvent release(QEvent::KeyRelease, value, modifiers);
            QApplication::sendEvent(list, &press);
            QApplication::sendEvent(list, &release);
        };
        list->setFocus();
        key(Qt::Key_Up, Qt::AltModifier);
        EXPECT_EQ(list->currentRow(), 0);
        EXPECT_EQ(list->item(0)->text(), QString(300, 'A'));
        key(Qt::Key_Down, Qt::AltModifier);
        EXPECT_EQ(list->currentRow(), 1);
        navigation->setCurrentIndex(0);
        key(Qt::Key_Return);
        EXPECT_TRUE(dialog->isVisible());
        EXPECT_EQ(navigation->currentIndex(), 1);
        key(Qt::Key_Delete);
        EXPECT_EQ(list->count(), 2);
        while (list->count() > 0)
            dialog->findChild<QPushButton *>("missionRemove")->click();
        EXPECT_TRUE(dialog->findChild<QLabel *>("missionEmpty")->isVisible());
        EXPECT_FALSE(dialog->findChild<QLineEdit *>("missionSegmentName")->isVisible());
        EXPECT_FALSE(navigation->isTabEnabled(1));
        EXPECT_FALSE(dialog->findChild<QPushButton *>("missionMoveUp")->isEnabled());
        EXPECT_FALSE(dialog->findChild<QPushButton *>("missionRemove")->isEnabled());
        settled();
        screenshot("mission-empty");
        dialog->findChild<QPushButton *>("missionAddCoast")->click();
        EXPECT_EQ(list->count(), 1);
        EXPECT_TRUE(navigation->isTabEnabled(1));
        EXPECT_TRUE(dialog->findChild<QLabel *>("errorText")->isHidden());
        dialog->reject();
    });
    EXPECT_FALSE(edit_mission(plan, hpop, core::Epoch{0}, core::Epoch{0},
                              core::Epoch{7'200'000'000'000}, &parent));
    EXPECT_EQ(plan, original);
}
} // namespace orbitforge::desktop
