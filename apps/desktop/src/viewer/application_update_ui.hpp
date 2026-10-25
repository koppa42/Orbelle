#pragma once
#include "application_update.hpp"
#include <QDialog>
#include <QTimer>
#include <functional>
class QAction;
class QLabel;
class QMenuBar;
class QProgressBar;
class QPushButton;
class QTextBrowser;

namespace orbelle::desktop {
class ApplicationUpdateUi final : public QDialog {
  public:
    ApplicationUpdateUi(QString preferences, QMenuBar *menus, QWidget *parent,
                        QNetworkAccessManager *network = nullptr, QString app_dir = {});
    void schedule_startup_check();
    void launch_updater(std::function<void(bool, QString)> finished);
    std::function<void()> restart_requested;

  protected:
    void reject() override;

  private:
    void refresh();
    void show_manual();
    ApplicationUpdate update_;
    QTimer prompt_;
    QTimer progress_refresh_;
    ApplicationUpdateState displayed_state_ = ApplicationUpdateState::idle;
    QString displayed_bundle_, displayed_notes_;
    QLabel *heading_, *description_;
    QTextBrowser *notes_;
    QProgressBar *progress_;
    QPushButton *primary_, *skip_, *later_, *release_page_;
    bool manual_visible_ = false, offered_ = false;
};
// A launcher handshake is acknowledged only after the normal visible UI is responsive.
void acknowledge_update_startup();
} // namespace orbelle::desktop
