#pragma once

#include <QNetworkProxy>
#include <QString>
#include <QVariantMap>
#include <optional>

namespace orbelle::desktop {
enum class ProxyMode { system, direct, manual };

struct ProxySettings {
    ProxyMode mode = ProxyMode::system;
    QString address;
    bool operator==(const ProxySettings &) const = default;
};

// One application policy, loaded before any desktop downloads. Qt managers using
// DefaultProxy (data acquisition and application updates) share this policy.
// Changes affect subsequent requests; existing replies retain their connection.
// This class is owned and used by the GUI thread, outside scenario/SDK settings.
class NetworkSettings final {
  public:
    explicit NetworkSettings(QString preferences_file);
    [[nodiscard]] const ProxySettings &value() const { return value_; }
    [[nodiscard]] const QString &load_error() const { return load_error_; }
    [[nodiscard]] const QString &preferences_file() const { return preferences_file_; }
    // Validate and persist before changing the live policy. Failure preserves it.
    // Other desktop preferences join the same atomic INI write, before publishing
    // the proxy policy. The network/* keys are reserved for this class.
    [[nodiscard]] bool save(ProxySettings next, QString &error,
                            const QVariantMap &other_preferences = {});
    // HTTP CONNECT and SOCKS5 endpoints, with an explicit port. No credentials.
    [[nodiscard]] static std::optional<QNetworkProxy> parse_address(const QString &address);

  private:
    void apply() const;
    QString preferences_file_;
    ProxySettings value_;
    QString load_error_;
};
} // namespace orbelle::desktop
