#include "application_update_ui.hpp"
#include "../ui_text.hpp"
#include <QAction>
#include <QApplication>
#include <QCheckBox>
#include <QDesktopServices>
#include <QDialogButtonBox>
#include <QHBoxLayout>
#include <QLabel>
#include <QMenu>
#include <QMenuBar>
#include <QProgressBar>
#include <QProgressDialog>
#include <QPushButton>
#include <QScreen>
#include <QSignalBlocker>
#include <QTextBrowser>
#include <QVBoxLayout>
#ifdef Q_OS_WIN
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#endif

namespace orbelle::desktop {
ApplicationUpdateUi::ApplicationUpdateUi(QString preferences, QMenuBar *menus, QWidget *parent,
                                         QNetworkAccessManager *network, QString app_dir)
    : QDialog(parent),
      update_(QCoreApplication::applicationVersion(), std::move(preferences),
              app_dir.isEmpty() ? QCoreApplication::applicationDirPath() : app_dir, this, network) {
    setObjectName("applicationUpdateDialog");
    setWindowTitle(ui::text("updates.title"));
    setModal(false);
    setAttribute(Qt::WA_ShowWithoutActivating);
    auto *layout = new QVBoxLayout(this);
    layout->setContentsMargins(20, 18, 20, 18);
    layout->setSpacing(12);
    heading_ = new QLabel;
    heading_->setWordWrap(true);
    auto font = heading_->font();
    font.setPointSizeF(font.pointSizeF() + 2);
    font.setBold(true);
    heading_->setFont(font);
    layout->addWidget(heading_);
    description_ = new QLabel;
    description_->setWordWrap(true);
    layout->addWidget(description_);
    notes_ = new QTextBrowser;
    notes_->setOpenLinks(false);
    notes_->setMinimumHeight(100);
    layout->addWidget(notes_, 1);
    release_page_ = new QPushButton(ui::text("updates.release_page"));
    layout->addWidget(release_page_, 0, Qt::AlignLeft);
    connect(release_page_, &QPushButton::clicked, this, [this] {
        if (update_.release())
            QDesktopServices::openUrl(update_.release()->page);
    });
    progress_ = new QProgressBar;
    progress_->setTextVisible(false);
    layout->addWidget(progress_);
    startup_ = new QCheckBox(ui::text("updates.startup"));
    startup_->setChecked(update_.startup_enabled());
    layout->addWidget(startup_);
    auto *buttons = new QDialogButtonBox;
    primary_ = buttons->addButton(ui::text("updates.download"), QDialogButtonBox::ActionRole);
    primary_->setObjectName("downloadApplicationUpdate");
    skip_ = buttons->addButton(ui::text("updates.skip"), QDialogButtonBox::ActionRole);
    later_ = buttons->addButton(ui::text("updates.later"), QDialogButtonBox::RejectRole);
    later_->setDefault(true);
    layout->addWidget(buttons);
    connect(later_, &QPushButton::clicked, this, &ApplicationUpdateUi::reject);
    connect(skip_, &QPushButton::clicked, this, [this] {
        manual_visible_ = false;
        update_.skip_version();
        hide();
    });
    connect(primary_, &QPushButton::clicked, this, [this] {
        if (update_.state() == ApplicationUpdateState::ready) {
            if (restart_requested)
                restart_requested();
        } else if (update_.state() == ApplicationUpdateState::error && !update_.release())
            update_.check(true);
        else
            update_.download();
    });
    auto *help = menus->addMenu(ui::text("updates.help"));
    auto *check = help->addAction(ui::text("updates.check"));
    check->setObjectName("checkApplicationUpdates");
    connect(check, &QAction::triggered, this, [this] { show_manual(); });
    startup_action_ = help->addAction(ui::text("updates.startup"));
    startup_action_->setCheckable(true);
    startup_action_->setChecked(update_.startup_enabled());
    connect(startup_, &QCheckBox::toggled, this, [this](bool enabled) {
        update_.set_startup_enabled(enabled);
        const QSignalBlocker blocker(startup_action_);
        startup_action_->setChecked(enabled);
    });
    connect(startup_action_, &QAction::toggled, this, [this](bool enabled) {
        update_.set_startup_enabled(enabled);
        const QSignalBlocker blocker(startup_);
        startup_->setChecked(enabled);
    });
    prompt_.setInterval(500);
    connect(&prompt_, &QTimer::timeout, this, [this] {
        if (update_.state() != ApplicationUpdateState::available || offered_) {
            prompt_.stop();
            return;
        }
        if (!parentWidget()->isActiveWindow() || QApplication::activeModalWidget())
            return;
        offered_ = true;
        prompt_.stop();
        show();
    });
    update_.changed = [this] { refresh(); };
    resize(QSize(600, 460).boundedTo(parent->screen()->availableGeometry().size() - QSize(40, 60)));
    refresh();
}
void ApplicationUpdateUi::schedule_startup_check() {
    QTimer::singleShot(1500, this, [this] { update_.check(false); });
}
void ApplicationUpdateUi::launch_updater(std::function<void(bool, QString)> finished) {
    auto *progress = new QProgressDialog(ui::text("updates.handoff"), ui::text("updates.cancel"), 0,
                                         0, parentWidget());
    progress->setWindowTitle(ui::text("updates.title"));
    progress->setWindowModality(Qt::ApplicationModal);
    progress->setMinimumDuration(0);
    progress->show();
    connect(progress, &QProgressDialog::canceled, this, [this] { update_.cancel_handover(); });
    update_.launch_updater([progress, finished = std::move(finished)](bool ready, QString error) {
        progress->close();
        progress->deleteLater();
        finished(ready, std::move(error));
    });
}
void ApplicationUpdateUi::show_manual() {
    manual_visible_ = true;
    offered_ = true;
    prompt_.stop();
    update_.check(true);
    show();
    raise();
    activateWindow();
}
void ApplicationUpdateUi::reject() {
    manual_visible_ = false;
    offered_ = true;
    update_.cancel();
    QDialog::reject();
}
void ApplicationUpdateUi::refresh() {
    const auto state = update_.state();
    const bool available = state == ApplicationUpdateState::available;
    const bool ready = state == ApplicationUpdateState::ready;
    const bool error = state == ApplicationUpdateState::error;
    const bool busy = update_.busy();
    const auto &release = update_.release();
    const auto manifest =
        release ? "Orbelle-" + release->version + "-win-x64-update.json" : QString{};
    const bool installable = update_.portable() && release && release->assets.contains(manifest);
    release_page_->setVisible(release.has_value());
    notes_->setVisible(release.has_value() || error);
    if (error)
        notes_->setPlainText(update_.error());
    else if (release)
        notes_->setPlainText(release->notes);
    skip_->setVisible(available);
    primary_->setVisible(ready || ((available || error) && installable) || (error && !release));
    primary_->setText(ready               ? ui::text("updates.restart")
                      : error && !release ? ui::text("updates.retry")
                                          : ui::text("updates.download"));
    later_->setText(busy                 ? ui::text("updates.cancel")
                    : available || ready ? ui::text("updates.later")
                                         : ui::text("updates.close"));
    progress_->setVisible(busy);
    progress_->setRange(0, 0);
    if (release)
        description_->setText(
            ui::text("updates.versions").arg(update_.current_version(), release->version));
    else
        description_->setText(ui::text("updates.current").arg(update_.current_version()));
    if (state == ApplicationUpdateState::checking)
        heading_->setText(ui::text("updates.checking"));
    else if (available) {
        heading_->setText(ui::text("updates.available").arg(release->version));
        if (!installable)
            description_->setText(ui::text("updates.manual_install"));
    } else if (ready) {
        heading_->setText(ui::text("updates.ready"));
        description_->setText(ui::text("updates.restart_detail"));
    } else if (state == ApplicationUpdateState::preparing) {
        heading_->setText(update_.cancelled() ? ui::text("updates.cancelling")
                                              : ui::text("updates.preparing"));
        description_->setText(ui::text("updates.preparing_detail"));
    } else if (state == ApplicationUpdateState::downloading) {
        QString part = ui::text("updates.part_manifest");
        if (update_.bundle() == "app")
            part = ui::text("updates.part_app");
        else if (update_.bundle() == "runtime")
            part = ui::text("updates.part_runtime");
        else if (update_.bundle() == "resources")
            part = ui::text("updates.part_resources");
        heading_->setText(ui::text("updates.downloading").arg(part));
        if (update_.total() > 0) {
            progress_->setRange(0, 1000);
            progress_->setValue(static_cast<int>(1000 * update_.received() / update_.total()));
            description_->setText(ui::text("updates.progress")
                                      .arg(QString::number(update_.received() / 1048576.0, 'f', 1),
                                           QString::number(update_.total() / 1048576.0, 'f', 1)));
        }
    } else if (error)
        heading_->setText(ui::text("updates.failed"));
    else
        heading_->setText(ui::text("updates.latest"));
    if (available && !manual_visible_ && !offered_ && !prompt_.isActive())
        prompt_.start();
}
void acknowledge_update_startup() {
#ifdef Q_OS_WIN
    const auto arguments = QCoreApplication::arguments();
    const auto index = arguments.indexOf("--update-ready-event");
    if (index < 0 || index + 1 >= arguments.size())
        return;
    const auto name = arguments[index + 1];
    if (!name.startsWith("Local\\Orbelle.Ready.") || name.size() > 200)
        return;
    if (const auto event =
            OpenEventW(EVENT_MODIFY_STATE, FALSE, reinterpret_cast<LPCWSTR>(name.utf16()))) {
        SetEvent(event);
        CloseHandle(event);
    }
#endif
}
} // namespace orbelle::desktop
