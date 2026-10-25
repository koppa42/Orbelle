#include "network_settings.hpp"
#include "../ui_text.hpp"
#include <QNetworkProxyFactory>
#include <QSettings>
#include <QUrl>
#include <algorithm>

namespace orbelle::desktop {
namespace {
QString mode_key(ProxyMode mode) {
    switch (mode) {
    case ProxyMode::system:
        return QStringLiteral("system");
    case ProxyMode::direct:
        return QStringLiteral("direct");
    case ProxyMode::manual:
        return QStringLiteral("manual");
    }
    return {};
}
QString normalized_address(const QNetworkProxy &proxy) {
    QUrl url;
    url.setScheme(proxy.type() == QNetworkProxy::Socks5Proxy ? "socks5" : "http");
    url.setHost(proxy.hostName());
    url.setPort(proxy.port());
    return url.toString(QUrl::FullyEncoded);
}
} // namespace

std::optional<QNetworkProxy> NetworkSettings::parse_address(const QString &address) {
    auto text = address.trimmed();
    if (text.isEmpty() || std::any_of(text.begin(), text.end(), [](QChar character) {
            return character.isSpace() || character.category() == QChar::Other_Control;
        }))
        return std::nullopt;
    if (!text.contains("://"))
        text.prepend("http://");
    const QUrl url(text, QUrl::StrictMode);
    if (!url.isValid() || url.host().isEmpty() || url.port() < 1 || url.port() > 65535 ||
        (url.scheme() != "http" && url.scheme() != "socks5") || url.authority().contains('@') ||
        (!url.path().isEmpty() && url.path() != "/") || url.hasQuery() || url.hasFragment())
        return std::nullopt;
    return QNetworkProxy(url.scheme() == "socks5" ? QNetworkProxy::Socks5Proxy
                                                  : QNetworkProxy::HttpProxy,
                         url.host(), static_cast<quint16>(url.port()));
}

NetworkSettings::NetworkSettings(QString preferences_file)
    : preferences_file_(std::move(preferences_file)) {
    QSettings settings(preferences_file_, QSettings::IniFormat);
    const auto mode = settings.value("network/proxy-mode", "system").toString();
    if (mode == "direct")
        value_.mode = ProxyMode::direct;
    else if (mode == "manual")
        value_.mode = ProxyMode::manual;
    const auto address = settings.value("network/proxy-address").toString();
    if (const auto proxy = parse_address(address))
        value_.address = normalized_address(*proxy);
    if ((mode != "system" && mode != "direct" && mode != "manual") ||
        (value_.mode == ProxyMode::manual && value_.address.isEmpty()) ||
        settings.status() != QSettings::NoError) {
        value_ = {};
        load_error_ = ui::text("network_settings.load_failed");
    }
    apply();
}

bool NetworkSettings::save(ProxySettings next, QString &error,
                           const QVariantMap &other_preferences) {
    error.clear();
    if (mode_key(next.mode).isEmpty()) {
        error = ui::text("network_settings.invalid_mode");
        return false;
    }
    if (next.mode == ProxyMode::manual) {
        const auto proxy = parse_address(next.address);
        if (!proxy) {
            error = ui::text("network_settings.invalid_address");
            return false;
        }
        next.address = normalized_address(*proxy);
    } else {
        // Keep the last valid endpoint; a disabled, unapplied draft is not a preference.
        next.address = value_.address;
    }
    QSettings settings(preferences_file_, QSettings::IniFormat);
    auto changes = other_preferences;
    changes.insert("network/proxy-mode", mode_key(next.mode));
    changes.insert("network/proxy-address", next.address);
    QVariantMap previous;
    for (auto it = changes.cbegin(); it != changes.cend(); ++it) {
        previous.insert(it.key(), settings.value(it.key()));
        settings.setValue(it.key(), it.value());
    }
    settings.sync();
    if (settings.status() != QSettings::NoError) {
        // QSettings shares pending values between instances. Restore those too, so
        // a later write by the updater cannot accidentally publish a failed draft.
        for (auto it = previous.cbegin(); it != previous.cend(); ++it) {
            if (it.value().isValid())
                settings.setValue(it.key(), it.value());
            else
                settings.remove(it.key());
        }
        settings.sync();
        error = ui::text("network_settings.save_failed");
        return false;
    }
    value_ = std::move(next);
    load_error_.clear();
    apply();
    return true;
}

void NetworkSettings::apply() const {
    if (value_.mode == ProxyMode::system)
        QNetworkProxyFactory::setUseSystemConfiguration(true);
    else
        QNetworkProxy::setApplicationProxy(value_.mode == ProxyMode::direct
                                               ? QNetworkProxy(QNetworkProxy::NoProxy)
                                               : *parse_address(value_.address));
}
} // namespace orbelle::desktop
