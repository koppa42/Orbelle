#include "../src/viewer/coverage_map.hpp"
#include "../src/viewer/epoch_editor.hpp"
#include "../src/viewer/object_selection.hpp"
#include "../src/viewer/report_dialog.hpp"
#include "../src/viewer/report_plot.hpp"
#include "../src/viewer/report_selection.hpp"
#include "../src/viewer/workbench_style.hpp"
#include "../src/viewer/workspace.hpp"
#include "window_show_observer.hpp"
#include <QApplication>
#include <QDialogButtonBox>
#include <QDir>
#include <QKeyEvent>
#include <QLabel>
#include <QLineEdit>
#include <QListWidget>
#include <QPushButton>
#include <QScrollArea>
#include <QScrollBar>
#include <QTabBar>
#include <QTreeWidget>
#include <algorithm>
#include <cmath>
#include <gtest/gtest.h>
#include <numbers>
#include <orbelle/runtime/ephemeris.hpp>
#include <orbelle/runtime/time_frames.hpp>
#include <orbelle/runtime/two_body.hpp>
#include <stdexcept>

#include "support/report_test_support.hpp"

namespace orbelle::desktop {
namespace {
using namespace report_test;
TEST(ReportSelection, SearchSourcesStableKeysAndDeletedChoices) {
    (void)test_app();
    std::vector<ReportChoice> choices{
        {"builtin:0", QStringLiteral("场景摘要"), QStringLiteral("摘要"),
         QStringLiteral("场景时段与对象数量"), QStringLiteral("当前对象"), false,
         QStringLiteral("不适用于当前对象范围")},
        {"builtin:2", QStringLiteral("位置与速度"), QStringLiteral("时间序列"),
         QStringLiteral("GCRF 或 ITRF"), QStringLiteral("卫星")}};
    for (int i = 0; i < 200; ++i)
        choices.push_back({QString("template:%1").arg(i),
                           QStringLiteral("同名模板"),
                           QStringLiteral("位置与速度"),
                           QStringLiteral("时间序列表与曲线"),
                           QStringLiteral("卫星 %1").arg(i),
                           true,
                           {}});
    ReportSelectionDialog dialog(choices, "template:100");
    dialog.show();
    auto *list = dialog.findChild<QListWidget *>("reportSelectionList");
    auto *search = dialog.findChild<QLineEdit *>("reportSelectionSearch");
    auto *source = dialog.findChild<QTabBar *>("reportSelectionSource");
    auto *confirm = dialog.findChild<QDialogButtonBox *>()->button(QDialogButtonBox::Ok);
    EXPECT_EQ(dialog.selection(), "template:100");
    list->setCurrentRow(0);
    EXPECT_FALSE(confirm->isEnabled());
    EXPECT_TRUE(dialog.findChild<QLabel *>("reportSelectionWarning")->isVisible());
    search->setText("gcrf");
    EXPECT_EQ(dialog.selection(), QString{});
    EXPECT_FALSE(confirm->isEnabled());
    EXPECT_FALSE(list->item(1)->isHidden());
    EXPECT_TRUE(list->item(0)->isHidden());
    list->setCurrentRow(1);
    source->setCurrentIndex(2);
    EXPECT_FALSE(confirm->isEnabled());
    EXPECT_TRUE(dialog.findChild<QLabel *>("reportSelectionEmpty")->isVisible());
    search->clear();
    EXPECT_TRUE(list->item(1)->isHidden());
    list->setCurrentRow(102);
    EXPECT_TRUE(list->currentItem()
                    ->data(Qt::AccessibleDescriptionRole)
                    .toString()
                    .contains(QStringLiteral("卫星 100")));
    list->setFocus();
    QKeyEvent down(QEvent::KeyPress, Qt::Key_Down, Qt::NoModifier);
    QApplication::sendEvent(list, &down);
    EXPECT_EQ(dialog.selection(), "template:101");
    list->setCurrentRow(102);
    std::reverse(choices.begin(), choices.end());
    dialog.set_choices(choices);
    EXPECT_EQ(dialog.selection(), "template:100");
    EXPECT_TRUE(confirm->isEnabled());
    auto &long_name = *std::ranges::find(choices, QString("template:100"), &ReportChoice::key);
    long_name.name = QStringLiteral("沿海观测任务 / 卫星位置与速度的自定义报告 · ").repeated(8);
    dialog.set_choices(choices);
    dialog.resize(360, 540);
    QCoreApplication::processEvents();
    for (auto *scroll : dialog.findChildren<QScrollArea *>())
        EXPECT_EQ(scroll->horizontalScrollBar()->maximum(), 0);
    const auto directory = qEnvironmentVariable("ORBELLE_REPORT_UI_CAPTURE");
    if (!directory.isEmpty()) {
        QDir().mkpath(directory);
        EXPECT_TRUE(dialog.grab().save(directory + "/report-selector-long-name.png"));
    }
    std::erase_if(choices, [](const auto &choice) { return choice.key == "template:100"; });
    dialog.set_choices(choices);
    EXPECT_TRUE(dialog.selection().isEmpty());
    EXPECT_FALSE(confirm->isEnabled());
    dialog.accept();
    EXPECT_TRUE(dialog.isVisible());
    dialog.reject();
}

TEST(ReportSelection, CurveFieldsSearchKeepsCheckedItemsAndValidatesLimit) {
    (void)test_app();
    const std::vector<ReportCurveField> fields{
        {0, "state.x", "X (km)", "km"},
        {1, "state.y", "Y (km)", "km"},
        {2, "state.z", "Z (km)", "km"},
        {3, "state.vx", "Vx (km/s)", "km/s"},
        {4, "state.vy", "Vy (km/s)", "km/s"},
        {5, "state.vz", "Vz (km/s)", "km/s"},
        {6, "motion.speed", QStringLiteral("速率 (km/s)"), "km/s"}};
    ReportCurveSelectionDialog chooser(fields, {"state.x", "state.y"});
    chooser.show();
    auto *list = chooser.findChild<QTreeWidget *>("reportCurveList");
    auto *search = chooser.findChild<QLineEdit *>("reportCurveSearch");
    auto *confirm = chooser.findChild<QDialogButtonBox *>()->button(QDialogButtonBox::Ok);
    search->setText("vx");
    EXPECT_TRUE(list->topLevelItem(0)->isHidden());
    EXPECT_FALSE(list->topLevelItem(3)->isHidden());
    list->topLevelItem(3)->setCheckState(0, Qt::Checked);
    EXPECT_EQ(chooser.selection(), (QStringList{"state.x", "state.y", "state.vx"}));
    search->setText("no-such-field");
    EXPECT_TRUE(chooser.findChild<QLabel *>("reportCurveCount")
                    ->text()
                    .contains(QStringLiteral("没有匹配")));
    EXPECT_TRUE(confirm->isEnabled()); // Searching does not discard hidden checked fields.
    search->clear();
    const auto capture = qEnvironmentVariable("ORBELLE_REPORT_UI_CAPTURE");
    for (int width : {560, 360}) {
        chooser.resize(width, 480);
        QCoreApplication::processEvents();
        EXPECT_EQ(list->horizontalScrollBar()->maximum(), 0);
        if (!capture.isEmpty()) {
            QDir().mkpath(capture);
            EXPECT_TRUE(
                chooser.grab().save(capture + QString("/curve-selector-%1.png").arg(width)));
        }
    }
    for (int i = 0; i < list->topLevelItemCount(); ++i)
        list->topLevelItem(i)->setCheckState(0, Qt::Checked);
    EXPECT_FALSE(confirm->isEnabled());
    chooser.accept();
    EXPECT_EQ(chooser.result(), QDialog::Rejected);
    chooser.findChild<QPushButton *>("reportCurveClear")->click();
    EXPECT_TRUE(chooser.selection().isEmpty());
    EXPECT_FALSE(confirm->isEnabled());
    list->topLevelItem(0)->setCheckState(0, Qt::Checked);
    EXPECT_TRUE(confirm->isEnabled());
    chooser.accept();
    EXPECT_EQ(chooser.result(), QDialog::Accepted);
}

TEST(ReportSelection, ColumnSearchOrderDefaultsAndEmptySelection) {
    (void)test_app();
    ReportColumnSelectionDialog chooser(
        {{"object.name", QStringLiteral("对象名称"), ""},
         {"time.epoch", QStringLiteral("时间"), "TAI"},
         {"state.x", QStringLiteral("X 位置"), "m"},
         {"state.y", QStringLiteral("一个较长的 Y 位置字段名称，用于检查窄窗口"), "km"}},
        {"state.x", "object.name", "time.epoch"}, {"object.name", "time.epoch", "state.x"});
    chooser.show();
    auto *list = chooser.findChild<QTreeWidget *>("reportColumnList");
    auto *search = chooser.findChild<QLineEdit *>("reportColumnSearch");
    auto *confirm = chooser.findChild<QPushButton *>("reportColumnConfirm");
    auto *down = chooser.findChild<QPushButton *>("reportColumnDown");
    auto *up = chooser.findChild<QPushButton *>("reportColumnUp");
    ASSERT_TRUE(list && search && confirm && down && up);
    EXPECT_EQ(chooser.selection(), (QStringList{"state.x", "object.name", "time.epoch"}));
    list->setCurrentItem(list->topLevelItem(0));
    down->click();
    EXPECT_EQ(chooser.selection(), (QStringList{"object.name", "state.x", "time.epoch"}));
    up->click();
    EXPECT_EQ(chooser.selection().front(), "state.x");
    search->setText("state.y");
    EXPECT_TRUE(list->topLevelItem(0)->isHidden());
    EXPECT_FALSE(list->topLevelItem(3)->isHidden());
    EXPECT_FALSE(down->isEnabled());
    EXPECT_FALSE(up->isEnabled());
    list->topLevelItem(3)->setCheckState(0, Qt::Checked);
    search->setText("no-match");
    EXPECT_EQ(chooser.selection().size(), 4);
    EXPECT_TRUE(confirm->isEnabled());
    search->clear();
    const auto capture = qEnvironmentVariable("ORBELLE_REPORT_UI_CAPTURE");
    for (int width : {560, 340}) {
        chooser.resize(width, 500);
        QCoreApplication::processEvents();
        EXPECT_EQ(chooser.width(), width);
        EXPECT_EQ(list->horizontalScrollBar()->maximum(), 0);
        for (auto *button : chooser.findChildren<QPushButton *>())
            EXPECT_TRUE(
                chooser.rect().contains(button->mapTo(&chooser, button->rect().bottomRight())));
        if (!capture.isEmpty()) {
            QDir().mkpath(capture);
            EXPECT_TRUE(
                chooser.grab().save(capture + QString("/column-selector-%1.png").arg(width)));
        }
    }
    chooser.findChild<QPushButton *>("reportColumnClear")->click();
    EXPECT_FALSE(confirm->isEnabled());
    chooser.accept();
    EXPECT_EQ(chooser.result(), QDialog::Rejected);
    chooser.findChild<QPushButton *>("reportColumnDefaults")->click();
    EXPECT_EQ(chooser.selection(), (QStringList{"object.name", "time.epoch", "state.x"}));
    chooser.findChild<QPushButton *>("reportColumnAll")->click();
    EXPECT_EQ(chooser.selection().size(), 4);
    chooser.accept();
    EXPECT_EQ(chooser.result(), QDialog::Accepted);
}
} // namespace
} // namespace orbelle::desktop
