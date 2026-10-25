#include "application_update.hpp"
#include <QCoreApplication>
#include <QDateTime>
#include <QDir>
#include <QElapsedTimer>
#include <QFileInfo>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QNetworkReply>
#include <QProcess>
#include <QRegularExpression>
#include <QStorageInfo>
#include <QUuid>
#include <QVersionNumber>
#include <algorithm>
#include <chrono>
#include <cmath>
#ifdef Q_OS_WIN
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#endif

namespace orbelle::desktop {
namespace {
constexpr qint64 metadata_limit = 8 * 1024 * 1024;
constexpr qint64 manifest_limit = 16 * 1024 * 1024;
const QUrl release_endpoint("https://api.github.com/repos/koppa42/Orbelle/releases/latest");
bool version_valid(const QString &value) {
    static const QRegularExpression pattern(
        R"((0|[1-9][0-9]{0,8})\.(0|[1-9][0-9]{0,8})\.(0|[1-9][0-9]{0,8}))");
    const auto match = pattern.match(value);
    return match.hasMatch() && match.capturedLength() == value.size();
}
QByteArray digest(const QString &value) {
    static const QRegularExpression pattern("^[0-9a-f]{64}$");
    if (!pattern.match(value).hasMatch())
        throw std::runtime_error("Invalid SHA-256 digest in release metadata");
    return QByteArray::fromHex(value.toLatin1());
}
bool trusted_redirect(const QUrl &url) {
    return url.scheme() == "https" && url.userInfo().isEmpty() &&
           (url.port(-1) == -1 || url.port() == 443) &&
           (url.host() == "github.com" || url.host() == "api.github.com" ||
            url.host() == "release-assets.githubusercontent.com" ||
            url.host() == "objects.githubusercontent.com");
}
} // namespace

struct ApplicationUpdate::Handover {
#ifdef Q_OS_WIN
    HANDLE ready = nullptr, cancel = nullptr;
    ~Handover() {
        if (ready)
            CloseHandle(ready);
        if (cancel)
            CloseHandle(cancel);
    }
#endif
    QElapsedTimer elapsed;
    std::function<void(bool, QString)> finished;
};

bool newer_application_version(const QString &candidate, const QString &current) {
    return version_valid(candidate) && version_valid(current) &&
           QVersionNumber::fromString(candidate) > QVersionNumber::fromString(current);
}

std::optional<ApplicationRelease> parse_application_release(const QByteArray &bytes) {
    if (bytes.size() > metadata_limit)
        throw std::runtime_error("Release metadata is too large");
    QJsonParseError error;
    const auto document = QJsonDocument::fromJson(bytes, &error);
    if (error.error != QJsonParseError::NoError || !document.isObject())
        throw std::runtime_error("Invalid release metadata");
    const auto object = document.object();
    if (!object.value("draft").isBool() || !object.value("prerelease").isBool())
        throw std::runtime_error("Missing release channel");
    if (object.value("draft").toBool() || object.value("prerelease").toBool())
        return {};
    const auto tag = object.value("tag_name").toString();
    if (!tag.startsWith('v') || !version_valid(tag.mid(1)))
        throw std::runtime_error("Unsupported release version");
    ApplicationRelease release;
    release.version = tag.mid(1);
    release.notes = object.value("body").toString().left(100000);
    // Construct the trusted destination rather than rendering a server-provided HTML URL.
    release.page = QUrl("https://github.com/koppa42/Orbelle/releases/tag/" + tag);
    if (!object.value("assets").isArray())
        throw std::runtime_error("Missing release assets");
    const auto prefix = "Orbelle-" + release.version + "-win-x64";
    const QStringList wanted{prefix + "-update.json", prefix + "-app.zip", prefix + "-runtime.zip",
                             prefix + "-resources.zip"};
    for (const auto &value : object.value("assets").toArray()) {
        const auto item = value.toObject();
        const auto name = item.value("name").toString();
        if (!wanted.contains(name))
            continue;
        const auto url = QUrl(item.value("browser_download_url").toString());
        const auto expected =
            QUrl("https://github.com/koppa42/Orbelle/releases/download/" + tag + "/" + name);
        const auto size = item.value("size").toDouble(-1);
        if (release.assets.contains(name) || url != expected || !std::isfinite(size) || size <= 0 ||
            size > 8.0 * 1024 * 1024 * 1024 || size != std::floor(size))
            throw std::runtime_error("Invalid update asset");
        const auto checksum = item.value("digest").toString();
        QByteArray sha;
        if (!checksum.isEmpty()) {
            if (!checksum.startsWith("sha256:"))
                throw std::runtime_error("Unsupported release asset digest");
            sha = digest(checksum.mid(7));
        }
        release.assets.insert(name, {name, url, static_cast<qint64>(size), sha});
    }
    return release;
}

ApplicationUpdate::ApplicationUpdate(QString current_version, QString preferences, QString app_dir,
                                     QObject *parent, QNetworkAccessManager *network)
    : QObject(parent), current_version_(std::move(current_version)), app_dir_(std::move(app_dir)),
      install_root_(determine_install_root(app_dir_)), settings_(preferences, QSettings::IniFormat),
      network_(network ? network : &owned_network_) {
    poll_.setInterval(50);
    connect(&poll_, &QTimer::timeout, this, [this] { poll_work(); });
    deadline_.setSingleShot(true);
    connect(&deadline_, &QTimer::timeout, this, [this] {
        if (reply_) {
            transfer_error_ = QStringLiteral("The update request timed out");
            reply_->abort();
        }
    });
}
ApplicationUpdate::~ApplicationUpdate() {
    changed = {};
#ifdef Q_OS_WIN
    if (handover_)
        SetEvent(handover_->cancel);
#endif
    if (cancellation_)
        cancellation_->store(true);
    if (reply_) {
        disconnect(reply_, nullptr, this, nullptr);
        reply_->abort();
    }
    if (worker_.valid())
        worker_.wait();
    output_.close();
}
bool ApplicationUpdate::startup_enabled() const {
    return settings_.value("updates/startup", true).toBool();
}
void ApplicationUpdate::set_startup_enabled(bool enabled) {
    settings_.setValue("updates/startup", enabled);
    settings_.sync();
}
bool ApplicationUpdate::busy() const {
    return state_ == ApplicationUpdateState::checking ||
           state_ == ApplicationUpdateState::downloading ||
           state_ == ApplicationUpdateState::preparing;
}
void ApplicationUpdate::notify() {
    if (changed)
        changed();
}
void ApplicationUpdate::fail(const QString &message) {
    deadline_.stop();
    output_.close();
    temporary_.reset();
    error_ = message;
    state_ = ApplicationUpdateState::error;
    notify();
}
void ApplicationUpdate::check(bool manual) {
    if (busy() || state_ == ApplicationUpdateState::ready) {
        manual_ = manual_ || manual;
        notify();
        return;
    }
    if (!manual && (!startup_enabled() || settings_.value("updates/retryAfter").toLongLong() >
                                              QDateTime::currentSecsSinceEpoch()))
        return;
    manual_ = manual;
    cancelled_ = false;
    cache_retry_ = false;
    error_.clear();
    release_.reset();
    state_ = ApplicationUpdateState::checking;
    request(release_endpoint, Request::release, metadata_limit);
}
void ApplicationUpdate::request(const QUrl &url, Request kind, qint64 size_limit,
                                const QByteArray &expected_hash, bool use_cache) {
    request_ = kind;
    limit_ = size_limit;
    expected_hash_ = expected_hash;
    received_ = 0;
    total_ = kind == Request::release ? -1 : size_limit;
    body_.clear();
    transfer_error_.clear();
    hash_.reset();
    QNetworkRequest request(url);
    request.setRawHeader("User-Agent", "Orbelle/" + current_version_.toLatin1());
    request.setRawHeader("Accept", kind == Request::release ? "application/vnd.github+json"
                                                            : "application/octet-stream");
    request.setAttribute(QNetworkRequest::RedirectPolicyAttribute,
                         QNetworkRequest::UserVerifiedRedirectPolicy);
    request.setMaximumRedirectsAllowed(4);
    request.setTransferTimeout(kind == Request::bundle ? 60000 : 15000);
    if (kind == Request::release && use_cache) {
        const auto etag = settings_.value("updates/etag").toByteArray();
        if (!etag.isEmpty() && settings_.contains("updates/release"))
            request.setRawHeader("If-None-Match", etag);
    }
    auto *reply = network_->get(request);
    reply_ = reply;
    reply->setReadBufferSize(1024 * 1024);
    connect(reply, &QNetworkReply::redirected, this, [this, reply](const QUrl &redirect) {
        if (reply_ != reply)
            return;
        if (trusted_redirect(redirect))
            reply->redirectAllowed();
        else {
            transfer_error_ = QStringLiteral("Update redirected outside the trusted HTTPS hosts");
            reply->abort();
        }
    });
    connect(reply, &QNetworkReply::readyRead, this, [this, reply] {
        if (reply_ != reply)
            return;
        const auto bytes = reply->readAll();
        if (bytes.size() > limit_ - received_) {
            transfer_error_ = QStringLiteral("Update response exceeds its declared size");
            reply->abort();
            return;
        }
        received_ += bytes.size();
        hash_.addData(bytes);
        if (request_ == Request::bundle) {
            if (output_.write(bytes) != bytes.size()) {
                transfer_error_ = QStringLiteral("Cannot write update download");
                reply->abort();
                return;
            }
        } else
            body_.append(bytes);
        notify();
    });
    connect(reply, &QNetworkReply::finished, this, [this, reply] { finish_reply(reply); });
    deadline_.start(kind == Request::bundle ? 30 * 60 * 1000 : 20000);
    notify();
}
void ApplicationUpdate::finish_reply(QNetworkReply *reply) {
    reply->deleteLater();
    if (reply_ != reply)
        return;
    reply_ = nullptr;
    deadline_.stop();
    output_.close();
    if (cancelled_) {
        finish_cancel();
        return;
    }
    const auto status = reply->attribute(QNetworkRequest::HttpStatusCodeAttribute).toInt();
    if (request_ == Request::release && status == 304) {
        try {
            const auto cached = settings_.value("updates/release").toByteArray();
            release_ = parse_application_release(cached);
            if (cached.isEmpty())
                throw std::runtime_error("Missing update cache");
        } catch (const std::exception &) {
            if (!cache_retry_) {
                cache_retry_ = true;
                request(release_endpoint, Request::release, metadata_limit, {}, false);
                return;
            }
            fail(QStringLiteral("Invalid update cache"));
            return;
        }
    } else {
        if (!transfer_error_.isEmpty() || reply->error() != QNetworkReply::NoError ||
            status != 200) {
            if (status == 403 || status == 429) {
                const auto now = QDateTime::currentSecsSinceEpoch();
                const auto delay =
                    std::clamp(reply->rawHeader("Retry-After").toLongLong(), 60LL, 86400LL);
                settings_.setValue(
                    "updates/retryAfter",
                    std::max(
                        now + delay,
                        std::min(now + 86400, reply->rawHeader("X-RateLimit-Reset").toLongLong())));
            }
            fail(transfer_error_.isEmpty()
                     ? QStringLiteral("HTTP %1: %2").arg(status).arg(reply->errorString())
                     : transfer_error_);
            return;
        }
        if ((!expected_hash_.isEmpty() && hash_.result() != expected_hash_) ||
            (request_ != Request::release && received_ != limit_)) {
            fail(QStringLiteral("Update size or SHA-256 verification failed"));
            return;
        }
        try {
            if (request_ == Request::release) {
                release_ = parse_application_release(body_);
                settings_.setValue("updates/release", body_);
                settings_.setValue("updates/etag", reply->rawHeader("ETag"));
                settings_.remove("updates/retryAfter");
                settings_.sync();
            } else if (request_ == Request::manifest) {
                manifest_ = parse_update_manifest(body_, release_->version);
                for (const auto &bundle : manifest_->bundles) {
                    if (!release_->assets.contains(bundle.name))
                        throw std::runtime_error("Incomplete release update bundles");
                    const auto &asset = release_->assets[bundle.name];
                    if (asset.size != static_cast<qint64>(bundle.size) ||
                        (!asset.sha256.isEmpty() && asset.sha256 != bundle.sha256))
                        throw std::runtime_error("Release bundle differs from update manifest");
                }
                begin_plan();
                return;
            } else {
                downloads_.insert(active_bundle_, output_.fileName());
                next_bundle();
                return;
            }
        } catch (const std::exception &error) {
            fail(QString::fromUtf8(error.what()));
            return;
        }
    }
    const bool newer = release_ && newer_application_version(release_->version, current_version_);
    const bool skipped =
        release_ && settings_.value("updates/skipped").toString() == release_->version;
    state_ = newer && (manual_ || !skipped) ? ApplicationUpdateState::available
                                            : ApplicationUpdateState::idle;
    notify();
}
void ApplicationUpdate::skip_version() {
    if (release_ && state_ == ApplicationUpdateState::available) {
        settings_.setValue("updates/skipped", release_->version);
        settings_.sync();
        state_ = ApplicationUpdateState::idle;
        notify();
    }
}
void ApplicationUpdate::download() {
    if (busy() || !release_ || !portable())
        return;
    const auto name = "Orbelle-" + release_->version + "-win-x64-update.json";
    if (!release_->assets.contains(name)) {
        fail(QStringLiteral("This release has no automatic update package"));
        return;
    }
    const auto &asset = release_->assets[name];
    if (asset.size > manifest_limit) {
        fail(QStringLiteral("Update manifest is too large"));
        return;
    }
    temporary_ = std::make_unique<QTemporaryDir>(install_root_ + "/.update-download-XXXXXX");
    if (!temporary_->isValid()) {
        fail(QStringLiteral("The portable installation directory is not writable"));
        return;
    }
    cancelled_ = false;
    cancellation_ = std::make_shared<std::atomic_bool>(false);
    downloads_.clear();
    prepared_.clear();
    state_ = ApplicationUpdateState::downloading;
    active_bundle_ = QStringLiteral("manifest");
    request(asset.url, Request::manifest, asset.size, asset.sha256);
}
void ApplicationUpdate::begin_plan() {
    state_ = ApplicationUpdateState::preparing;
    work_ = Work::plan;
    const auto manifest = *manifest_;
    const auto current = app_dir_;
    const auto root = install_root_;
    const auto cancel = cancellation_;
    worker_ = std::async(std::launch::async, [manifest, current, root, cancel] {
        if (QFileInfo::exists(root + "/versions/" + manifest.version))
            return WorkResult{{}, stage_update(manifest, current, root, {}, *cancel)};
        return WorkResult{plan_update(manifest, current, *cancel), {}};
    });
    poll_.start();
    notify();
}
void ApplicationUpdate::next_bundle() {
    if (needed_.isEmpty()) {
        begin_stage();
        return;
    }
    active_bundle_ = needed_.takeFirst();
    const auto found =
        std::find_if(manifest_->bundles.begin(), manifest_->bundles.end(),
                     [this](const auto &bundle) { return bundle.id == active_bundle_; });
    if (found == manifest_->bundles.end()) {
        fail(QStringLiteral("Invalid update download plan"));
        return;
    }
    output_.setFileName(temporary_->path() + "/" + found->name);
    if (!output_.open(QIODevice::WriteOnly | QIODevice::NewOnly)) {
        fail(QStringLiteral("Cannot create update download"));
        return;
    }
    state_ = ApplicationUpdateState::downloading;
    request(release_->assets[found->name].url, Request::bundle, static_cast<qint64>(found->size),
            found->sha256);
}
void ApplicationUpdate::begin_stage() {
    state_ = ApplicationUpdateState::preparing;
    work_ = Work::stage;
    const auto manifest = *manifest_;
    const auto current = app_dir_;
    const auto root = install_root_;
    const auto downloads = downloads_;
    const auto cancel = cancellation_;
    worker_ = std::async(std::launch::async, [manifest, current, root, downloads, cancel] {
        return WorkResult{{}, stage_update(manifest, current, root, downloads, *cancel)};
    });
    poll_.start();
    notify();
}
void ApplicationUpdate::poll_work() {
    if (!worker_.valid() || worker_.wait_for(std::chrono::seconds(0)) != std::future_status::ready)
        return;
    poll_.stop();
    try {
        auto result = worker_.get();
        if (cancelled_) {
            finish_cancel();
            return;
        }
        if (work_ == Work::plan && result.prepared.isEmpty()) {
            needed_ = std::move(result.needed);
            quint64 bytes = 0;
            for (const auto &file : manifest_->files)
                bytes += file.size;
            for (const auto &bundle : manifest_->bundles)
                if (needed_.contains(bundle.id))
                    bytes += bundle.size;
            const QStorageInfo storage(install_root_);
            if (storage.isValid() && storage.bytesAvailable() >= 0 &&
                bytes + 64 * 1024 * 1024 > static_cast<quint64>(storage.bytesAvailable()))
                throw std::runtime_error("Not enough free space to prepare the update");
            next_bundle();
        } else {
            prepared_ = result.prepared;
            temporary_.reset();
            state_ = ApplicationUpdateState::ready;
            notify();
        }
    } catch (const UpdateCancelled &) {
        finish_cancel();
    } catch (const std::exception &error) {
        if (cancelled_)
            finish_cancel();
        else
            fail(QString::fromUtf8(error.what()));
    }
}
void ApplicationUpdate::cancel() {
    if (!busy())
        return;
    cancelled_ = true;
    if (cancellation_)
        cancellation_->store(true);
    if (reply_)
        reply_->abort();
    else if (!worker_.valid())
        finish_cancel();
    notify();
}
void ApplicationUpdate::finish_cancel() {
    output_.close();
    temporary_.reset();
    state_ = release_ ? ApplicationUpdateState::available : ApplicationUpdateState::idle;
    notify();
}
void ApplicationUpdate::launch_updater(std::function<void(bool, QString)> finished) {
#ifdef Q_OS_WIN
    if (handover_) {
        finished(false, QStringLiteral("An update handover is already in progress"));
        return;
    }
    if (state_ != ApplicationUpdateState::ready || prepared_.isEmpty()) {
        finished(false, QStringLiteral("The update is not ready"));
        return;
    }
    const auto helper = prepared_ + "/OrbelleUpdater.exe";
    const auto name = QStringLiteral("Local\\Orbelle.UpdateParent.%1.%2")
                          .arg(QCoreApplication::applicationPid())
                          .arg(QUuid::createUuid().toString(QUuid::WithoutBraces));
    const auto cancel_name = name + ".cancel";
    const auto ready_event =
        CreateEventW(nullptr, TRUE, FALSE, reinterpret_cast<LPCWSTR>(name.utf16()));
    const auto cancel_event =
        CreateEventW(nullptr, TRUE, FALSE, reinterpret_cast<LPCWSTR>(cancel_name.utf16()));
    if (!ready_event || !cancel_event) {
        if (ready_event)
            CloseHandle(ready_event);
        if (cancel_event)
            CloseHandle(cancel_event);
        finished(false, QStringLiteral("Cannot create the update handover events"));
        return;
    }
    QProcess process;
    process.setProgram(helper);
    process.setArguments({"--apply-update", install_root_, release_->version,
                          QString::number(QCoreApplication::applicationPid()),
                          "--parent-ready-event", name});
    process.setWorkingDirectory(install_root_);
    if (!process.startDetached()) {
        SetEvent(cancel_event);
        CloseHandle(ready_event);
        CloseHandle(cancel_event);
        finished(false, process.errorString());
        return;
    }
    handover_ = std::make_unique<Handover>();
    handover_->ready = ready_event;
    handover_->cancel = cancel_event;
    handover_->finished = std::move(finished);
    handover_->elapsed.start();
    handover_poll_.setInterval(30);
    connect(&handover_poll_, &QTimer::timeout, this, [this] {
        if (!handover_)
            return;
        const bool ready = WaitForSingleObject(handover_->ready, 0) == WAIT_OBJECT_0;
        if (!ready && handover_->elapsed.elapsed() < 5000)
            return;
        handover_poll_.stop();
        disconnect(&handover_poll_, nullptr, this, nullptr);
        if (!ready)
            SetEvent(handover_->cancel);
        auto callback = std::move(handover_->finished);
        handover_.reset();
        callback(ready,
                 ready ? QString{}
                       : QStringLiteral("The updater did not accept the handover; please retry"));
    });
    handover_poll_.start();
#else
    finished(false, QStringLiteral("Automatic installation currently requires Windows x64"));
#endif
}
void ApplicationUpdate::cancel_handover() {
    if (!handover_)
        return;
    handover_poll_.stop();
    disconnect(&handover_poll_, nullptr, this, nullptr);
#ifdef Q_OS_WIN
    SetEvent(handover_->cancel);
#endif
    auto finished = std::move(handover_->finished);
    handover_.reset();
    finished(false, {});
}
} // namespace orbelle::desktop
