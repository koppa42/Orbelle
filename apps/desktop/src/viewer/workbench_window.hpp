#pragma once
#include <QMainWindow>
#include <QPointer>

class QHBoxLayout;
class QToolButton;

namespace orbelle::desktop {
// Client-drawn Windows chrome; all move/resize operations remain owned by the OS.
// Other platforms keep their native window manager decorations.
class WorkbenchWindow : public QMainWindow {
  public:
    void install_title_bar(QWidget *bar, QHBoxLayout *layout);
    [[nodiscard]] bool title_drag_target(const QPoint &position) const;
    [[nodiscard]] Qt::Edges resize_edges(const QPoint &position) const;

  protected:
    bool nativeEvent(const QByteArray &type, void *message, qintptr *result) override;
    void changeEvent(QEvent *event) override;
    void showEvent(QShowEvent *event) override;

  private:
    void update_window_buttons();
    void maximize_window();
    void restore_window();
    void minimize_window();
    QPointer<QWidget> title_bar_;
    QToolButton *maximize_ = nullptr;
    QRect restore_geometry_;
};
} // namespace orbelle::desktop
