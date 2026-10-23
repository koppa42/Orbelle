#include "../src/viewer/workbench_style.hpp"
#include <QAbstractItemView>
#include <QApplication>
#include <QComboBox>
#include <QDateTimeEdit>
#include <QDoubleSpinBox>
#include <QKeyEvent>
#include <QLineEdit>
#include <QMouseEvent>
#include <QScrollArea>
#include <QScrollBar>
#include <QSpinBox>
#include <QVBoxLayout>
#include <QWheelEvent>
#ifdef Q_OS_WIN
#include <qt_windows.h>
#endif
#include <gtest/gtest.h>

namespace orbelle::desktop {
namespace {
QApplication &application() {
    if (auto *existing = qobject_cast<QApplication *>(QApplication::instance()))
        return *existing;
    static int argc = 1;
    static char name[] = "workbench-input-test";
    static char *argv[] = {name, nullptr};
    static QApplication app(argc, argv);
    return app;
}

class WorkbenchInput : public testing::Test {
  protected:
    void SetUp() override { apply_workbench_style(application()); }

    static void show(QWidget &window) {
        window.resize(360, 300);
        window.show();
        window.activateWindow();
        QApplication::processEvents();
    }

    static bool wheel(QWidget &widget, QPoint angle, QPoint pixels = {}) {
        const auto point = widget.rect().center();
        QWheelEvent event(point, widget.mapToGlobal(point), pixels, angle, Qt::NoButton,
                          Qt::NoModifier, Qt::NoScrollPhase, false);
        QApplication::sendEvent(&widget, &event);
        return event.isAccepted();
    }

    static void key(QWidget &widget, int code, const QString &text = {}) {
        QKeyEvent press(QEvent::KeyPress, code, Qt::NoModifier, text);
        QKeyEvent release(QEvent::KeyRelease, code, Qt::NoModifier, text);
        QApplication::sendEvent(&widget, &press);
        QApplication::sendEvent(&widget, &release);
    }
};

TEST_F(WorkbenchInput, ClosedCombosIgnoreWheelWithOrWithoutFocus) {
    QWidget window;
    auto *layout = new QVBoxLayout(&window);
    auto *anchor = new QLineEdit(&window);
    layout->addWidget(anchor);
    for (const bool editable : {false, true}) {
        auto *combo = new QComboBox(&window);
        combo->setEditable(editable);
        combo->addItems({"First", "Second", "Third"});
        combo->setCurrentIndex(1);
        layout->addWidget(combo);
        show(window);
        for (const bool focused : {false, true}) {
            SCOPED_TRACE(testing::Message() << "editable=" << editable << " focused=" << focused);
            (focused ? static_cast<QWidget *>(combo) : anchor)->setFocus(Qt::TabFocusReason);
            ASSERT_EQ(combo->hasFocus(), focused);
            for (const auto delta : {QPoint(0, 120), QPoint(0, -120), QPoint(120, 0)}) {
                EXPECT_FALSE(wheel(*combo, delta));
                EXPECT_EQ(combo->currentIndex(), 1);
                EXPECT_EQ(combo->currentText(), "Second");
            }
            EXPECT_FALSE(wheel(*combo, {}, QPoint(0, -15)));
            EXPECT_EQ(combo->currentIndex(), 1);
            EXPECT_EQ(combo->hasFocus(), focused);
        }
    }
}

TEST_F(WorkbenchInput, NumericAndDateEditorsIgnoreWheelEvenWhileEditing) {
    QWidget window;
    auto *layout = new QVBoxLayout(&window);
    auto *anchor = new QLineEdit(&window);
    auto *integer = new QSpinBox(&window);
    auto *decimal = new QDoubleSpinBox(&window);
    auto *date = new QDateTimeEdit(QDateTime(QDate(2026, 10, 23), QTime(12, 30)), &window);
    integer->setRange(0, 1000);
    integer->setValue(60);
    decimal->setValue(1.25);
    const auto initial_date = date->dateTime();
    layout->addWidget(anchor);
    layout->addWidget(integer);
    layout->addWidget(decimal);
    layout->addWidget(date);
    show(window);
    for (auto *editor :
         {static_cast<QAbstractSpinBox *>(integer), static_cast<QAbstractSpinBox *>(decimal),
          static_cast<QAbstractSpinBox *>(date)}) {
        for (const bool focused : {false, true}) {
            (focused ? static_cast<QWidget *>(editor) : anchor)->setFocus(Qt::TabFocusReason);
            ASSERT_EQ(editor->hasFocus(), focused);
            EXPECT_FALSE(wheel(*editor, QPoint(0, 120)));
            EXPECT_FALSE(wheel(*editor, QPoint(0, -120)));
            EXPECT_FALSE(wheel(*editor, {}, QPoint(0, -15)));
            EXPECT_EQ(integer->value(), 60);
            EXPECT_DOUBLE_EQ(decimal->value(), 1.25);
            EXPECT_EQ(date->dateTime(), initial_date);
            EXPECT_EQ(editor->hasFocus(), focused);
        }
    }
}

TEST_F(WorkbenchInput, KeyboardSelectionAndNumericEditingRemainAvailable) {
    QWidget window;
    auto *layout = new QVBoxLayout(&window);
    auto *combo = new QComboBox(&window);
    auto *integer = new QSpinBox(&window);
    auto *editable = new QComboBox(&window);
    combo->addItems({"First", "Second", "Third"});
    integer->setRange(0, 1000);
    integer->setValue(60);
    editable->setEditable(true);
    editable->addItems({"First", "Second"});
    layout->addWidget(combo);
    layout->addWidget(integer);
    layout->addWidget(editable);
    show(window);
    combo->setFocus(Qt::TabFocusReason);
    key(*combo, Qt::Key_Down);
    EXPECT_EQ(combo->currentIndex(), 1);
    integer->setFocus(Qt::TabFocusReason);
    key(*integer, Qt::Key_Up);
    EXPECT_EQ(integer->value(), 61);
    integer->selectAll();
    key(*integer, Qt::Key_4, "4");
    key(*integer, Qt::Key_2, "2");
    key(*integer, Qt::Key_Return);
    EXPECT_EQ(integer->value(), 42);
    editable->setFocus(Qt::TabFocusReason);
    editable->lineEdit()->selectAll();
    key(*editable->lineEdit(), Qt::Key_X, "x");
    EXPECT_EQ(editable->currentText(), "x");
}

TEST_F(WorkbenchInput, ClickedPopupStillScrollsAndAcceptsASelection) {
    QWidget window;
    auto *layout = new QVBoxLayout(&window);
    auto *combo = new QComboBox(&window);
    for (int row = 0; row < 80; ++row)
        combo->addItem(QString("Option %1").arg(row));
    combo->setMaxVisibleItems(6);
    combo->view()->setMaximumHeight(160);
    layout->addWidget(combo);
    show(window);
    const QPoint point(combo->width() - 10, combo->height() / 2);
    QMouseEvent press(QEvent::MouseButtonPress, point, combo->mapToGlobal(point), Qt::LeftButton,
                      Qt::LeftButton, Qt::NoModifier);
    QApplication::sendEvent(combo, &press);
    QApplication::processEvents();
    ASSERT_TRUE(combo->view()->isVisible());
    auto *scroll = combo->view()->verticalScrollBar();
    ASSERT_GT(scroll->maximum(), 0);
    const auto original = scroll->value();
    EXPECT_TRUE(wheel(*combo->view()->viewport(), QPoint(0, -120)));
    EXPECT_GT(scroll->value(), original);
    combo->view()->setCurrentIndex(combo->model()->index(20, 0));
    key(*combo->view(), Qt::Key_Return);
    EXPECT_EQ(combo->currentIndex(), 20);
    EXPECT_FALSE(combo->view()->isVisible());
}

#ifdef Q_OS_WIN
TEST_F(WorkbenchInput, NativeWheelScrollsParentWithoutChangingOrFocusingItsEditors) {
    if (QApplication::platformName() != "windows")
        GTEST_SKIP() << "Native wheel propagation requires the Windows platform plugin";
    QWidget window;
    auto *layout = new QVBoxLayout(&window);
    auto *anchor = new QLineEdit(&window);
    auto *area = new QScrollArea(&window);
    auto *content = new QWidget;
    auto *form = new QVBoxLayout(content);
    auto *combo = new QComboBox(content);
    auto *integer = new QSpinBox(content);
    combo->addItems({"First", "Second", "Third"});
    combo->setCurrentIndex(1);
    integer->setValue(60);
    form->addWidget(combo);
    form->addWidget(integer);
    form->addStretch();
    content->setMinimumHeight(1000);
    area->setWidget(content);
    area->setWidgetResizable(true);
    layout->addWidget(anchor);
    layout->addWidget(area);
    show(window);
    auto *scroll = area->verticalScrollBar();
    ASSERT_GT(scroll->maximum(), 0);
    const auto handle = reinterpret_cast<HWND>(window.winId());
    for (auto *editor : {static_cast<QWidget *>(combo), static_cast<QWidget *>(integer)}) {
        for (const bool focused : {false, true}) {
            scroll->setValue(0);
            (focused ? editor : anchor)->setFocus(Qt::TabFocusReason);
            ASSERT_EQ(editor->hasFocus(), focused);
            const auto logical = editor->mapTo(&window, editor->rect().center());
            const auto ratio = window.devicePixelRatioF();
            POINT point{qRound(logical.x() * ratio), qRound(logical.y() * ratio)};
            ASSERT_TRUE(ClientToScreen(handle, &point));
            SendMessageW(handle, WM_MOUSEWHEEL, MAKEWPARAM(0, WORD(-WHEEL_DELTA)),
                         MAKELPARAM(point.x, point.y));
            QApplication::processEvents();
            EXPECT_GT(scroll->value(), 0);
            EXPECT_EQ(combo->currentIndex(), 1);
            EXPECT_EQ(integer->value(), 60);
            EXPECT_EQ(editor->hasFocus(), focused);
            EXPECT_EQ(anchor->hasFocus(), !focused);
        }
    }
}
#endif
} // namespace
} // namespace orbelle::desktop
