#include "../src/viewer/workbench_style.hpp"
#include "../src/viewer/workbench_theme.hpp"
#include "../src/viewer/workbench_window.hpp"
#include <QApplication>
#include <QCloseEvent>
#include <QEventLoop>
#include <QHBoxLayout>
#include <QLabel>
#include <QLineEdit>
#include <QMenuBar>
#include <QTimer>
#include <QToolButton>
#include <cmath>
#ifdef Q_OS_WIN
#include <qt_windows.h>
#endif
#include <gtest/gtest.h>

namespace orbitforge::desktop {
namespace {
#ifdef Q_OS_WIN
QApplication &application() {
    if (auto *existing = qobject_cast<QApplication *>(QApplication::instance()))
        return *existing;
    static int argc = 1;
    static char name[] = "workbench-window-test";
    static char *argv[] = {name, nullptr};
    static QApplication app(argc, argv);
    return app;
}
#endif
double luminance(const theme::Token &token) {
    const auto color = theme::color(token);
    const auto channel = [](double value) {
        return value <= 0.04045 ? value / 12.92 : std::pow((value + 0.055) / 1.055, 2.4);
    };
    return 0.2126 * channel(color.redF()) + 0.7152 * channel(color.greenF()) +
           0.0722 * channel(color.blueF());
}
double contrast(const theme::Token &a, const theme::Token &b) {
    const auto first = luminance(a), second = luminance(b);
    return (std::max(first, second) + 0.05) / (std::min(first, second) + 0.05);
}
} // namespace

TEST(WorkbenchTheme, TextAndInteractiveStatesRemainLegible) {
    for (const auto &surface : {theme::canvas, theme::chrome, theme::surface, theme::raised,
                                theme::overlay, theme::field, theme::accent_wash}) {
        for (const auto &text : {theme::text, theme::text_soft, theme::text_muted})
            EXPECT_GE(contrast(text, surface), 4.5) << text.name << " on " << surface.name;
    }
    for (const auto &fill : {theme::accent_deep, theme::accent_bright, theme::sakura_deep})
        EXPECT_GE(contrast(theme::on_accent, fill), 4.5);
    for (const auto &status : {theme::success, theme::warning, theme::danger})
        EXPECT_GE(contrast(status, theme::surface), 4.5);
}

#ifdef Q_OS_WIN
TEST(WorkbenchWindow, NativeCaptionCommandsPreserveNormalGeometry) {
    auto &app = application();
    class NativeWindow final : public WorkbenchWindow {
      public:
        using WorkbenchWindow::nativeEvent;
    } window;
    auto *bar = new QWidget(&window);
    bar->setFixedHeight(44);
    auto *layout = new QHBoxLayout(bar);
    window.setMenuWidget(bar);
    window.install_title_bar(bar, layout);
    window.setGeometry(100, 100, 960, 640);
    window.show();
    const auto settle = [&] {
        QEventLoop loop;
        QTimer::singleShot(150, &loop, &QEventLoop::quit);
        loop.exec();
        app.processEvents();
    };
    settle();
    const auto original = window.geometry();
    const auto send = [&](UINT message, WPARAM command) {
        MSG msg{};
        msg.message = message;
        msg.wParam = command;
        qintptr result = -1;
        EXPECT_TRUE(window.nativeEvent("windows_generic_MSG", &msg, &result));
        EXPECT_EQ(result, 0);
        settle();
    };
    send(WM_SYSCOMMAND, SC_MAXIMIZE);
    EXPECT_TRUE(window.isMaximized());
    send(WM_NCLBUTTONDBLCLK, HTCAPTION);
    EXPECT_FALSE(window.isMaximized());
    EXPECT_EQ(window.geometry(), original);
    send(WM_NCLBUTTONDBLCLK, HTCAPTION);
    EXPECT_TRUE(window.isMaximized());
    send(WM_SYSCOMMAND, SC_MINIMIZE);
    EXPECT_TRUE(window.isMinimized());
    send(WM_SYSCOMMAND, SC_RESTORE);
    EXPECT_TRUE(window.isMaximized());
    send(WM_SYSCOMMAND, SC_RESTORE);
    EXPECT_FALSE(window.isMaximized());
    EXPECT_EQ(window.geometry(), original);
}

TEST(WorkbenchWindow, TitleControlsDoNotStealDragAndResizeTargets) {
    auto &app = application();
    apply_workbench_style(app);
    WorkbenchWindow window;
    auto *bar = new QWidget(&window);
    bar->setFixedHeight(44);
    auto *layout = new QHBoxLayout(bar);
    auto *name = new QLabel(QStringLiteral("星弦 Orbelle"), bar);
    auto *menu = new QMenuBar(bar);
    menu->addMenu(QStringLiteral("场景"));
    layout->addWidget(name);
    layout->addWidget(menu);
    layout->addStretch();
    window.setMenuWidget(bar);
    window.install_title_bar(bar, layout);
    auto *draft = new QLineEdit("unsaved draft", &window);
    window.setCentralWidget(draft);
    window.resize(960, 640);
    window.show();
    app.processEvents();
    EXPECT_TRUE(window.title_drag_target(name->mapTo(&window, name->rect().center())));
    EXPECT_FALSE(window.title_drag_target(menu->mapTo(&window, menu->rect().center())));
    for (const auto *id : {"windowMinimize", "windowMaximize", "windowClose"}) {
        auto *button = window.findChild<QToolButton *>(id);
        ASSERT_NE(button, nullptr);
        EXPECT_FALSE(button->accessibleName().isEmpty());
        EXPECT_EQ(button->focusPolicy(), Qt::StrongFocus);
        EXPECT_FALSE(window.title_drag_target(button->mapTo(&window, button->rect().center())));
    }
    const auto w = window.width(), h = window.height();
    EXPECT_EQ(window.resize_edges(QPoint(1, 1)), Qt::LeftEdge | Qt::TopEdge);
    EXPECT_EQ(window.resize_edges(QPoint(w - 1, 1)), Qt::RightEdge | Qt::TopEdge);
    EXPECT_EQ(window.resize_edges(QPoint(1, h - 1)), Qt::LeftEdge | Qt::BottomEdge);
    EXPECT_EQ(window.resize_edges(QPoint(w - 1, h - 1)), Qt::RightEdge | Qt::BottomEdge);
    EXPECT_EQ(window.resize_edges(QPoint(1, h / 2)), Qt::Edges(Qt::LeftEdge));
    EXPECT_EQ(window.resize_edges(QPoint(w - 1, h / 2)), Qt::Edges(Qt::RightEdge));
    EXPECT_EQ(window.resize_edges(QPoint(w / 2, 1)), Qt::Edges(Qt::TopEdge));
    EXPECT_EQ(window.resize_edges(QPoint(w / 2, h - 1)), Qt::Edges(Qt::BottomEdge));
    EXPECT_EQ(window.resize_edges(QPoint(w / 2, h / 2)), Qt::Edges{});
    auto *maximize = window.findChild<QToolButton *>("windowMaximize");
    maximize->click();
    app.processEvents();
    EXPECT_TRUE(window.isMaximized());
    EXPECT_EQ(maximize->accessibleName(), QStringLiteral("还原"));
    EXPECT_EQ(window.resize_edges(QPoint(1, 1)), Qt::Edges{});
    maximize->click();
    app.processEvents();
    EXPECT_FALSE(window.isMaximized());
    EXPECT_EQ(window.centralWidget(), draft);
    EXPECT_EQ(draft->text(), "unsaved draft");
    window.setFixedSize(window.size());
    EXPECT_EQ(window.resize_edges(QPoint(1, 1)), Qt::Edges{});
}

TEST(WorkbenchWindow, CloseButtonHonorsVetoAndDoesNotCommitDraft) {
    auto &app = application();
    class VetoWindow final : public WorkbenchWindow {
      public:
        bool allow_close = false;
        int requests = 0;
        void closeEvent(QCloseEvent *event) override {
            ++requests;
            allow_close ? event->accept() : event->ignore();
        }
    } window;
    auto *bar = new QWidget(&window);
    auto *layout = new QHBoxLayout(bar);
    window.setMenuWidget(bar);
    window.install_title_bar(bar, layout);
    window.show();
    app.processEvents();
    auto *close = window.findChild<QToolButton *>("windowClose");
    ASSERT_NE(close, nullptr);
    close->click();
    EXPECT_EQ(window.requests, 1);
    EXPECT_TRUE(window.isVisible());
    window.allow_close = true;
    close->click();
    EXPECT_EQ(window.requests, 2);
    EXPECT_FALSE(window.isVisible());
}
#endif
} // namespace orbitforge::desktop
