#include "eop_catalog.hpp"
#include <QCryptographicHash>
#include <QDateTime>
#include <QNetworkRequest>
#include <algorithm>
#include <orbitforge/application/project.hpp>

namespace orbitforge::desktop {
namespace {
constexpr auto source_url = "https://datacenter.iers.org/data/9/finals2000A.all";
constexpr qint64 max_download = 16 * 1024 * 1024;
} // namespace
EopCatalog::EopCatalog(std::filesystem::path store, std::vector<std::filesystem::path> bundled,
                       std::filesystem::path legacy, bool automatic)
    : store_(std::move(store)), data_(store_) {
    for (const auto &manifest : bundled) {
        try {
            import_snapshot(manifest);
        } catch (const std::exception &error) {
            warnings_.push_back(QString::fromUtf8(error.what()));
            append(runtime::load_eop_snapshot(manifest));
        }
    }
    if (!legacy.empty() && std::filesystem::exists(legacy)) {
        for (const auto &entry : std::filesystem::directory_iterator(legacy)) {
            const auto name = entry.path().filename().string();
            if (!entry.is_directory() || name.size() != 64 ||
                !std::all_of(name.begin(), name.end(), [](char c) {
                    return (c >= '0' && c <= '9') || (c >= 'a' && c <= 'f');
                }))
                continue;
            try {
                import_snapshot(entry.path() / "manifest.json");
            } catch (const std::exception &error) {
                warnings_.push_back(QString::fromUtf8(error.what()));
            }
        }
    }
    for (const auto &entry : data_.snapshots()) {
        try {
            append(data_.load_eop(entry.lock_sha256));
        } catch (const std::exception &error) {
            warnings_.push_back(QString::fromStdString(entry.snapshot_id) + ": " +
                                QString::fromUtf8(error.what()));
        }
    }
    const auto default_key = data_.setting("default-eop");
    for (std::size_t i = 0; i < providers_.size(); ++i)
        if (application::eop_data_lock(providers_[i]->data_info()).sha256() == default_key)
            preferred = i;
    status_ = QString::fromStdString(data_.setting("finals-status"));
    deadline_.setSingleShot(true);
    connect(&deadline_, &QTimer::timeout, this, [this] {
        transfer_error_ = QStringLiteral("下载超时，请重试。");
        if (reply_)
            reply_->abort();
    });
    connect(&schedule_, &QTimer::timeout, this,
            [this] { check_updates_if_due(QDateTime::currentSecsSinceEpoch()); });
    if (automatic)
        schedule_.start(60000);
}
EopCatalog::~EopCatalog() {
    if (reply_) {
        disconnect(reply_, nullptr, this, nullptr);
        reply_->abort();
    }
}
bool EopCatalog::has_full_download() const {
    return std::any_of(providers_.begin(), providers_.end(), [](const auto &provider) {
        return provider->data_info().id.starts_with("iers-finals2000a-all-");
    });
}
void EopCatalog::check_updates_if_due(qint64 now, QUrl source) {
    try {
        if (downloading() || !automatic_updates() || !has_full_download())
            return;
        const auto last = std::max<qint64>(
            0, QString::fromStdString(data_.setting("finals-last-attempt")).toLongLong());
        if (now < last || now - last < 24 * 60 * 60)
            return;
        start_download(std::move(source));
    } catch (const std::exception &error) {
        status_ = QString::fromUtf8(error.what());
    }
}
std::size_t EopCatalog::append(Provider provider) {
    for (std::size_t i = 0; i < providers_.size(); ++i)
        if (application::matching_project_data(providers_[i]->data_info(), provider->data_info()))
            return i;
    providers_.push_back(std::move(provider));
    return providers_.size() - 1;
}
std::size_t EopCatalog::import_snapshot(const std::filesystem::path &manifest) {
    const auto entry = data_.install_eop(manifest);
    return append(data_.load_eop(entry.lock_sha256));
}
void EopCatalog::set_preferred(std::size_t index) {
    const auto key = application::eop_data_lock(providers_.at(index)->data_info()).sha256();
    data_.set_setting("default-eop", key);
    preferred = index;
}
bool EopCatalog::automatic_updates() const {
    // Updates begin after the user installs the first complete file.
    return data_.setting("finals-auto-update") != "0";
}
void EopCatalog::set_automatic_updates(bool enabled) {
    data_.set_setting("finals-auto-update", enabled ? "1" : "0");
}
void EopCatalog::import_raw(const std::filesystem::path &source) {
    const auto entry = data_.import_finals(
        source, source_url,
        QDateTime::currentDateTimeUtc().date().toString(Qt::ISODate).toStdString());
    last_installed_ = append(data_.load_eop(entry.lock_sha256));
    status_ = QStringLiteral("已安装：%1；场景绑定保持不变。")
                  .arg(QString::fromStdString(entry.snapshot_id));
}
void EopCatalog::cancel_download() {
    if (reply_) {
        cancelled_ = true;
        reply_->abort();
    }
}
void EopCatalog::start_download(QUrl url) {
    if (downloading())
        return;
    try {
        if (url.isEmpty())
            url = QUrl(source_url);
        if (url.scheme() != "https" && !(url.scheme() == "http" && url.host() == "127.0.0.1")) {
            status_ = QStringLiteral("数据源必须使用 HTTPS。");
            return;
        }
        download_ = std::make_unique<QTemporaryFile>(
            QString::fromStdWString((store_ / "staging/download-XXXXXX").wstring()));
        if (!download_->open()) {
            status_ = QStringLiteral("无法创建临时下载文件。");
            return;
        }
        received_ = 0;
        cancelled_ = false;
        transfer_error_.clear();
        last_installed_.reset();
        QNetworkRequest request(url);
        request.setAttribute(QNetworkRequest::RedirectPolicyAttribute,
                             QNetworkRequest::NoLessSafeRedirectPolicy);
        request.setMaximumRedirectsAllowed(3);
        request.setTransferTimeout(30000);
        request.setRawHeader("User-Agent", "OrbitForge/0.1 data-center");
        request.setAttribute(QNetworkRequest::CacheLoadControlAttribute,
                             QNetworkRequest::AlwaysNetwork);
        const auto installed_hash = data_.setting("finals-installed-source");
        const bool cached = data_.setting("finals-source-url") == url.toString().toStdString() &&
                            std::any_of(providers_.begin(), providers_.end(), [&](const auto &p) {
                                return p->data_info().sha256 == installed_hash &&
                                       p->data_info().id.starts_with("iers-finals2000a-all-");
                            });
        bool conditional = false;
        if (cached) {
            for (const auto &[setting, header] :
                 {std::pair{"finals-etag", "If-None-Match"},
                  std::pair{"finals-last-modified", "If-Modified-Since"}}) {
                const auto value = data_.setting(setting);
                if (!value.empty()) {
                    request.setRawHeader(header, QByteArray::fromStdString(value));
                    conditional = true;
                }
            }
        }
        data_.set_setting("finals-last-attempt",
                          std::to_string(QDateTime::currentSecsSinceEpoch()));
        data_.set_setting("finals-status", "上次更新尚未完成；可重新检查或下载。");
        status_ = QStringLiteral("正在检查并获取完整 IERS finals2000A 数据…");
        reply_ = network_.get(request);
        reply_->setReadBufferSize(64 * 1024);
        deadline_.start(120000);
        connect(reply_, &QNetworkReply::readyRead, this, [this] {
            const auto bytes = reply_->readAll();
            received_ += bytes.size();
            if (received_ > max_download || download_->write(bytes) != bytes.size()) {
                transfer_error_ = QStringLiteral("下载超过 16 MiB 限制，或临时文件写入失败。");
                reply_->abort();
            }
        });
        connect(reply_, &QNetworkReply::finished, this, [this, url, conditional] {
            deadline_.stop();
            auto *finished = reply_;
            reply_ = nullptr;
            try {
                if (cancelled_)
                    throw std::runtime_error("下载已取消，已有数据和场景保持不变。");
                if (!transfer_error_.isEmpty())
                    throw std::runtime_error(transfer_error_.toStdString());
                const auto http =
                    finished->attribute(QNetworkRequest::HttpStatusCodeAttribute).toInt();
                if (finished->error() != QNetworkReply::NoError ||
                    (http != 200 && !(http == 304 && conditional)))
                    throw std::runtime_error(
                        (QStringLiteral("下载失败：") + finished->errorString()).toStdString());
                if (!download_->flush() || !download_->seek(0))
                    throw std::runtime_error("Cannot flush download");
                const auto hash =
                    QCryptographicHash::hash(download_->readAll(), QCryptographicHash::Sha256)
                        .toHex()
                        .toStdString();
                if (http == 304) {
                    status_ = QStringLiteral("数据已是最新，未重复下载。");
                } else {
                    const auto entry = data_.import_finals(
                        std::filesystem::path(download_->fileName().toStdWString()),
                        url.toString().toStdString(),
                        QDateTime::currentDateTimeUtc().date().toString(Qt::ISODate).toStdString());
                    last_installed_ = append(data_.load_eop(entry.lock_sha256));
                    data_.set_setting("finals-installed-source", hash);
                    data_.set_setting("finals-source-url", url.toString().toStdString());
                    data_.set_setting("finals-etag", finished->rawHeader("ETag").toStdString());
                    data_.set_setting("finals-last-modified",
                                      finished->rawHeader("Last-Modified").toStdString());
                    status_ = QStringLiteral("完整数据已就绪，已有场景继续使用固定版本。");
                }
            } catch (const std::exception &error) {
                status_ = QString::fromUtf8(error.what());
            }
            try {
                data_.set_setting("finals-status", status_.toStdString());
            } catch (const std::exception &) {
            }
            download_.reset();
            finished->deleteLater();
        });
    } catch (const std::exception &error) {
        if (!reply_)
            download_.reset();
        status_ = QString::fromUtf8(error.what());
    }
}
} // namespace orbitforge::desktop
