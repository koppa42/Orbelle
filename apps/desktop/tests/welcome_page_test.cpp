#include "../src/viewer/welcome_page.hpp"
#include "../src/viewer/workbench_style.hpp"
#include <QAction>
#include <QApplication>
#include <QDir>
#include <QKeyEvent>
#include <QPushButton>
#include <QScrollArea>
#include <QScrollBar>
#include <gtest/gtest.h>
#include <memory>

namespace orbitforge::desktop {
TEST(WelcomePage, ActionsRemainReachableAtNarrowAndWideSizes) {
    static int argc = 1;
    static char executable[] = "welcome_test";
    static char *argv[] = {executable, nullptr};
    static QApplication app(argc, argv);
    apply_workbench_style(app);
    QAction create, open, example;
    int created = 0, opened = 0, tried = 0;
    QObject::connect(&create, &QAction::triggered, [&] { ++created; });
    QObject::connect(&open, &QAction::triggered, [&] { ++opened; });
    QObject::connect(&example, &QAction::triggered, [&] { ++tried; });
    const std::unique_ptr<QWidget> page(make_welcome_page(&create, &open, &example));
    auto *scroll = qobject_cast<QScrollArea *>(page.get());
    ASSERT_NE(scroll, nullptr);
    for (const auto size : {QSize(320, 360), QSize(1280, 760)}) {
        page->resize(size);
        page->show();
        QCoreApplication::processEvents();
        EXPECT_EQ(scroll->horizontalScrollBar()->maximum(), 0);
        for (const auto *name : {"welcomeCreate", "welcomeOpen", "welcomeExample"}) {
            auto *button = page->findChild<QPushButton *>(name);
            ASSERT_NE(button, nullptr);
            scroll->ensureWidgetVisible(button);
            QCoreApplication::processEvents();
            const QRect visible(button->mapTo(scroll->viewport(), QPoint(0, 0)), button->size());
            EXPECT_TRUE(scroll->viewport()->rect().contains(visible));
            button->setFocus(Qt::TabFocusReason);
            QKeyEvent press(QEvent::KeyPress, Qt::Key_Space, Qt::NoModifier);
            QKeyEvent release(QEvent::KeyRelease, Qt::Key_Space, Qt::NoModifier);
            QCoreApplication::sendEvent(button, &press);
            QCoreApplication::sendEvent(button, &release);
        }
        const auto captures = qEnvironmentVariable("ORBITFORGE_WELCOME_CAPTURE");
        if (!captures.isEmpty()) {
            ASSERT_TRUE(QDir().mkpath(captures));
            scroll->verticalScrollBar()->setValue(0);
            EXPECT_TRUE(page->grab().save(captures + QString("/welcome-%1.png").arg(size.width())));
        }
    }
    EXPECT_EQ(created, 2);
    EXPECT_EQ(opened, 2);
    EXPECT_EQ(tried, 2);
}
} // namespace orbitforge::desktop
