#pragma once
#include <QApplication>
#include <QEvent>
#include <QStringList>
#include <QWidget>

namespace orbelle::desktop {
// Final visibility alone misses controls that briefly show before layout insertion.
class WindowShowObserver final : public QObject {
  public:
    WindowShowObserver() { qApp->installEventFilter(this); }
    ~WindowShowObserver() override { qApp->removeEventFilter(this); }
    QStringList shown;

  protected:
    bool eventFilter(QObject *object, QEvent *event) override {
        if (event->type() == QEvent::Show) {
            auto *widget = qobject_cast<QWidget *>(object);
            if (widget && widget->isWindow())
                shown.append(QString::fromLatin1(widget->metaObject()->className()) + ":" +
                             widget->objectName());
        }
        return false;
    }
};
} // namespace orbelle::desktop
