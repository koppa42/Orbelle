#pragma once

#include <QBoxLayout>
#include <QEvent>
#include <QFormLayout>
#include <QTimer>
#include <QWidget>
#include <functional>
#include <utility>

namespace orbelle::desktop {
// Observe the space actually allocated to a panel (or its scroll viewport).
// Queue/coalesce changes so layouts settle before updating, without rebuilding controls.
class WidthObserver final : public QObject {
  public:
    WidthObserver(QWidget *panel, std::function<void(int)> update)
        : QObject(panel), panel_(panel), update_(std::move(update)) {
        panel->installEventFilter(this);
        schedule();
    }

  protected:
    bool eventFilter(QObject *object, QEvent *event) override {
        if (event->type() == QEvent::Resize || event->type() == QEvent::Show)
            schedule();
        return QObject::eventFilter(object, event);
    }

  private:
    void schedule() {
        if (pending_)
            return;
        pending_ = true;
        QTimer::singleShot(0, this, [this] {
            pending_ = false;
            if (!panel_->isVisible() || width_ == panel_->width())
                return;
            width_ = panel_->width();
            update_(width_);
        });
    }
    QWidget *panel_;
    std::function<void(int)> update_;
    int width_ = -1;
    bool pending_ = false;
};

// A small dead band prevents repeated switching near a breakpoint, including
// when wrapping introduces a vertical scrollbar. Dimensions are logical pixels.
class WidthMode {
  public:
    bool narrow(int width, int breakpoint) {
        narrow_ = width < breakpoint + (narrow_ ? 24 : 0);
        return narrow_;
    }

  private:
    bool narrow_ = true;
};

inline void responsive_columns(QWidget *panel, QBoxLayout *layout, int breakpoint) {
    layout->setDirection(QBoxLayout::TopToBottom);
    new WidthObserver(panel, [layout, breakpoint, mode = WidthMode{}](int width) mutable {
        layout->setDirection(mode.narrow(width, breakpoint) ? QBoxLayout::TopToBottom
                                                            : QBoxLayout::LeftToRight);
    });
}

inline void responsive_form(QFormLayout *form, int breakpoint = 380) {
    form->setRowWrapPolicy(QFormLayout::WrapAllRows);
    new WidthObserver(
        form->parentWidget(), [form, breakpoint, mode = WidthMode{}](int width) mutable {
            form->setRowWrapPolicy(mode.narrow(width, breakpoint) ? QFormLayout::WrapAllRows
                                                                  : QFormLayout::WrapLongRows);
        });
}
} // namespace orbelle::desktop
