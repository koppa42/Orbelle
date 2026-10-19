#include "workbench_window.hpp"
#include "workbench_style.hpp"
#include "workbench_theme.hpp"
#include <QAbstractButton>
#include <QApplication>
#include <QEvent>
#include <QHBoxLayout>
#include <QMenuBar>
#include <QPainter>
#include <QShowEvent>
#include <QToolBar>
#include <QToolButton>
#ifdef Q_OS_WIN
#include <qt_windows.h>
#include <windowsx.h>
#endif

namespace orbitforge::desktop {
namespace {
QIcon caption_icon(bool restore) {
    QIcon icon;
    for (int size : {16, 24, 32, 48}) {
        QPixmap image(size, size);
        image.fill(Qt::transparent);
        QPainter painter(&image);
        painter.setRenderHint(QPainter::Antialiasing);
        painter.scale(size / 16.0, size / 16.0);
        painter.setPen(QPen(theme::color(theme::text_soft), 1.1));
        if (restore) {
            painter.drawPolyline(QPolygonF{QPointF(5, 4), QPointF(5, 2), QPointF(13, 2),
                                           QPointF(13, 10), QPointF(11, 10)});
            painter.drawRect(QRectF(3, 5, 7, 7));
        } else {
            painter.drawRect(QRectF(3, 3, 10, 10));
        }
        icon.addPixmap(image);
    }
    return icon;
}
} // namespace

void WorkbenchWindow::install_title_bar(QWidget *bar, QHBoxLayout *layout) {
#ifdef Q_OS_WIN
    title_bar_ = bar;
    // Set before any Vulkan child surfaces exist. Never change flags on resize or maximize.
    setWindowFlag(Qt::FramelessWindowHint);
    const auto button = [&](const char *name, const QString &label, const QIcon &icon) {
        auto *control = new QToolButton(bar);
        control->setObjectName(name);
        control->setProperty("captionButton", true);
        control->setAccessibleName(label);
        control->setToolTip(label);
        control->setIcon(icon);
        control->setIconSize(QSize(16, 16));
        control->setFixedSize(40, 32);
        control->setFocusPolicy(Qt::StrongFocus);
        layout->addWidget(control);
        return control;
    };
    QPixmap line(16, 16);
    line.fill(Qt::transparent);
    {
        QPainter painter(&line);
        painter.setPen(theme::color(theme::text_soft));
        painter.drawLine(3, 8, 12, 8);
    }
    auto *minimize = button("windowMinimize", QStringLiteral("最小化"), QIcon(line));
    maximize_ = button("windowMaximize", QStringLiteral("最大化"), caption_icon(false));
    auto *close = button("windowClose", QStringLiteral("关闭 · Alt+F4"),
                         workbench_icon(WorkbenchIcon::close));
    connect(minimize, &QToolButton::clicked, this, &WorkbenchWindow::minimize_window);
    connect(maximize_, &QToolButton::clicked, this,
            [this] { isMaximized() ? restore_window() : maximize_window(); });
    // Dispatch the ordinary close event so the workbench retains save/discard/cancel.
    connect(close, &QToolButton::clicked, this, &QWidget::close);
#else
    Q_UNUSED(bar);
    Q_UNUSED(layout);
#endif
}

void WorkbenchWindow::maximize_window() {
    if (!isMaximized() && !isMinimized() && !isFullScreen())
        restore_geometry_ = geometry();
    showMaximized();
}

void WorkbenchWindow::minimize_window() {
    if (!isMaximized() && !isMinimized() && !isFullScreen())
        restore_geometry_ = geometry();
    showMinimized();
}

void WorkbenchWindow::restore_window() {
    if (isMinimized() && windowState().testFlag(Qt::WindowMaximized)) {
        showMaximized();
        return;
    }
    showNormal();
    // Qt can replace normalGeometry with the maximized client rect after native minimize /
    // restore. Preserve the last deliberate normal bounds across the entire cycle.
    if (restore_geometry_.isValid())
        setGeometry(restore_geometry_);
}

void WorkbenchWindow::update_window_buttons() {
    if (!maximize_)
        return;
    const auto label = isMaximized() ? QStringLiteral("还原") : QStringLiteral("最大化");
    maximize_->setIcon(caption_icon(isMaximized()));
    maximize_->setToolTip(label);
    maximize_->setAccessibleName(label);
}

void WorkbenchWindow::changeEvent(QEvent *event) {
    QMainWindow::changeEvent(event);
    if (event->type() == QEvent::WindowStateChange)
        update_window_buttons();
}

void WorkbenchWindow::showEvent(QShowEvent *event) {
#ifdef Q_OS_WIN
    if (title_bar_ && QApplication::platformName() == "windows") {
        const auto handle = reinterpret_cast<HWND>(winId());
        const auto style = GetWindowLongPtr(handle, GWL_STYLE);
        // Keep native resize, snap, system menu and minimize/maximize animations.
        const auto desired =
            style | WS_THICKFRAME | WS_CAPTION | WS_SYSMENU | WS_MINIMIZEBOX | WS_MAXIMIZEBOX;
        if (style != desired) {
            SetWindowLongPtr(handle, GWL_STYLE, desired);
            SetWindowPos(handle, nullptr, 0, 0, 0, 0,
                         SWP_NOMOVE | SWP_NOSIZE | SWP_NOZORDER | SWP_NOACTIVATE |
                             SWP_FRAMECHANGED);
        }
    }
#endif
    update_window_buttons();
    QMainWindow::showEvent(event);
}

Qt::Edges WorkbenchWindow::resize_edges(const QPoint &position) const {
    if (!title_bar_ || isMaximized() || isFullScreen() || !rect().contains(position))
        return {};
    constexpr int grip = 6; // Qt logical pixels, independent of display scale.
    Qt::Edges edges;
    if (minimumWidth() != maximumWidth()) {
        if (position.x() < grip)
            edges |= Qt::LeftEdge;
        if (position.x() >= width() - grip)
            edges |= Qt::RightEdge;
    }
    if (minimumHeight() != maximumHeight()) {
        if (position.y() < grip)
            edges |= Qt::TopEdge;
        if (position.y() >= height() - grip)
            edges |= Qt::BottomEdge;
    }
    return edges;
}

bool WorkbenchWindow::title_drag_target(const QPoint &position) const {
    if (!title_bar_ || !title_bar_->isVisible() ||
        !title_bar_->rect().contains(title_bar_->mapFrom(this, position)))
        return false;
    for (auto *child = childAt(position); child && child != title_bar_;
         child = child->parentWidget()) {
        if (qobject_cast<QAbstractButton *>(child) || qobject_cast<QMenuBar *>(child) ||
            qobject_cast<QToolBar *>(child))
            return false;
    }
    return true;
}

bool WorkbenchWindow::nativeEvent(const QByteArray &type, void *message, qintptr *result) {
#ifdef Q_OS_WIN
    if (title_bar_) {
        const auto *msg = static_cast<MSG *>(message);
        // Native caption gestures must update Qt's state as well as the HWND. Otherwise a
        // double-click can leave Qt's saved normal geometry out of sync with Windows.
        if (msg->message == WM_NCLBUTTONDBLCLK && msg->wParam == HTCAPTION) {
            isMaximized() ? restore_window() : maximize_window();
            *result = 0;
            return true;
        }
        if (msg->message == WM_SYSCOMMAND) {
            switch (msg->wParam & 0xfff0) {
            case SC_MAXIMIZE:
                maximize_window();
                break;
            case SC_MINIMIZE:
                minimize_window();
                break;
            case SC_RESTORE:
                restore_window();
                break;
            default:
                return QMainWindow::nativeEvent(type, message, result);
            }
            *result = 0;
            return true;
        }
        if (msg->message == WM_NCCALCSIZE && msg->wParam) {
            // Maximized client bounds must exclude the taskbar on the current monitor.
            if (IsZoomed(msg->hwnd) && !IsIconic(msg->hwnd)) {
                MONITORINFO monitor{sizeof(MONITORINFO)};
                if (GetMonitorInfo(MonitorFromWindow(msg->hwnd, MONITOR_DEFAULTTONEAREST),
                                   &monitor))
                    reinterpret_cast<NCCALCSIZE_PARAMS *>(msg->lParam)->rgrc[0] = monitor.rcWork;
            }
            *result = 0;
            return true;
        }
        if (msg->message == WM_NCHITTEST) {
            POINT point{GET_X_LPARAM(msg->lParam), GET_Y_LPARAM(msg->lParam)};
            ScreenToClient(msg->hwnd, &point);
            const QPoint position(qRound(point.x / devicePixelRatioF()),
                                  qRound(point.y / devicePixelRatioF()));
            const auto edges = resize_edges(position);
            if (edges.testFlag(Qt::TopEdge))
                *result = edges.testFlag(Qt::LeftEdge)    ? HTTOPLEFT
                          : edges.testFlag(Qt::RightEdge) ? HTTOPRIGHT
                                                          : HTTOP;
            else if (edges.testFlag(Qt::BottomEdge))
                *result = edges.testFlag(Qt::LeftEdge)    ? HTBOTTOMLEFT
                          : edges.testFlag(Qt::RightEdge) ? HTBOTTOMRIGHT
                                                          : HTBOTTOM;
            else if (edges.testFlag(Qt::LeftEdge))
                *result = HTLEFT;
            else if (edges.testFlag(Qt::RightEdge))
                *result = HTRIGHT;
            else
                *result = title_drag_target(position) ? HTCAPTION : HTCLIENT;
            return true;
        }
    }
#endif
    return QMainWindow::nativeEvent(type, message, result);
}
} // namespace orbitforge::desktop
