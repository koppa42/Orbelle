#pragma once
#include "update_package.hpp"
#include <QCryptographicHash>
#include <QFile>
#include <QNetworkAccessManager>
#include <QPointer>
#include <QSettings>
#include <QTemporaryDir>
#include <QTimer>
#include <QUrl>
#include <future>
#include <memory>
#include <optional>

class QNetworkReply;
namespace orbelle::desktop {
struct ReleaseAsset {
    QString name;
    QUrl url;
    qint64 size = 0;
    QByteArray sha256;
};
struct ApplicationRelease {
    QString version, notes;
    QUrl page;
    QMap<QString, ReleaseAsset> assets;
};
// Only stable, canonical release versions and this repository's HTTPS assets are accepted.
std::optional<ApplicationRelease> parse_application_release(const QByteArray &json);
bool newer_application_version(const QString &candidate, const QString &current);

enum class ApplicationUpdateState {
    idle,
    checking,
    available,
    downloading,
    preparing,
    ready,
    error
};
class ApplicationUpdate final : public QObject {
  public:
    ApplicationUpdate(QString current_version, QString preferences, QString app_dir,
                      QObject *parent = nullptr, QNetworkAccessManager *network = nullptr);
    ~ApplicationUpdate() override;
    void check(bool manual);
    void download();
    void cancel();
    void skip_version();
    void launch_updater(std::function<void(bool, QString)> finished);
    void cancel_handover();
    bool startup_enabled() const;
    void set_startup_enabled(bool enabled);
    bool portable() const { return !install_root_.isEmpty(); }
    bool busy() const;
    bool manual() const { return manual_; }
    bool cancelled() const { return cancelled_; }
    ApplicationUpdateState state() const { return state_; }
    const std::optional<ApplicationRelease> &release() const { return release_; }
    const QString &current_version() const { return current_version_; }
    const QString &error() const { return error_; }
    const QString &bundle() const { return active_bundle_; }
    qint64 received() const { return received_; }
    qint64 total() const { return total_; }
    std::function<void()> changed;

  private:
    enum class Request { release, manifest, bundle };
    enum class Work { plan, stage };
    struct WorkResult {
        QStringList needed;
        QString prepared;
    };
    void notify();
    void fail(const QString &error);
    void request(const QUrl &url, Request kind, qint64 size_limit,
                 const QByteArray &expected_hash = {}, bool use_cache = true);
    void finish_reply(QNetworkReply *reply);
    void begin_plan();
    void next_bundle();
    void begin_stage();
    void poll_work();
    void finish_cancel();
    struct Handover;
    std::unique_ptr<Handover> handover_;
    QTimer handover_poll_;
    QString current_version_, app_dir_, install_root_, error_, prepared_;
    mutable QSettings settings_;
    QNetworkAccessManager owned_network_;
    QNetworkAccessManager *network_;
    QPointer<QNetworkReply> reply_;
    QTimer poll_, deadline_;
    ApplicationUpdateState state_ = ApplicationUpdateState::idle;
    std::optional<ApplicationRelease> release_;
    std::optional<UpdateManifest> manifest_;
    std::unique_ptr<QTemporaryDir> temporary_;
    QFile output_;
    QMap<QString, QString> downloads_;
    QStringList needed_;
    QString active_bundle_, transfer_error_;
    QByteArray body_, expected_hash_;
    QCryptographicHash hash_{QCryptographicHash::Sha256};
    qint64 received_ = 0, total_ = -1, limit_ = 0;
    Request request_ = Request::release;
    Work work_ = Work::plan;
    std::future<WorkResult> worker_;
    std::shared_ptr<std::atomic_bool> cancellation_;
    bool manual_ = false, cancelled_ = false, cache_retry_ = false;
};
} // namespace orbelle::desktop
