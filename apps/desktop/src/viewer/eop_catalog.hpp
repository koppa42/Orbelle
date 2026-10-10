#pragma once
#include <QNetworkAccessManager>
#include <QNetworkReply>
#include <QObject>
#include <QStringList>
#include <QTemporaryFile>
#include <QTimer>
#include <optional>
#include <orbitforge/runtime/data_catalog.hpp>
#include <orbitforge/runtime/time_frames.hpp>
#include <vector>
class QWidget;

namespace orbitforge::desktop {
class EopCatalog : public QObject {
  public:
    using Provider = std::shared_ptr<const core::IEarthOrientationProvider>;
    EopCatalog(std::filesystem::path store, std::vector<std::filesystem::path> bundled,
               std::filesystem::path legacy = {}, bool automatic = true);
    ~EopCatalog() override;
    std::size_t import_snapshot(const std::filesystem::path &manifest);
    [[nodiscard]] const std::vector<Provider> &providers() const { return providers_; }
    [[nodiscard]] const QStringList &warnings() const { return warnings_; }
    [[nodiscard]] const std::filesystem::path &store() const { return store_; }
    runtime::DataCatalog &data() { return data_; }
    void set_preferred(std::size_t index);
    void set_automatic_updates(bool enabled);
    [[nodiscard]] bool automatic_updates() const;
    void start_download(QUrl url = {});
    void cancel_download();
    void check_updates_if_due(qint64 now, QUrl source = {});
    [[nodiscard]] bool has_full_download() const;
    [[nodiscard]] bool downloading() const { return reply_ != nullptr; }
    [[nodiscard]] const QString &status() const { return status_; }
    [[nodiscard]] qint64 received_bytes() const { return received_; }
    [[nodiscard]] std::optional<std::size_t> last_installed() const { return last_installed_; }
    void import_raw(const std::filesystem::path &source);
    std::size_t preferred = 0;

  private:
    std::size_t append(Provider provider);
    std::filesystem::path store_;
    runtime::DataCatalog data_;
    std::vector<Provider> providers_;
    QStringList warnings_;
    QNetworkAccessManager network_;
    QNetworkReply *reply_ = nullptr;
    std::unique_ptr<QTemporaryFile> download_;
    QTimer schedule_, deadline_;
    QString status_;
    qint64 received_ = 0;
    bool cancelled_ = false;
    QString transfer_error_;
    std::optional<std::size_t> last_installed_;
};
// Imports remain installed if cancelled; only acceptance selects a data version.
std::optional<std::size_t>
choose_eop_snapshot(QWidget *parent, EopCatalog &catalog, const EopCatalog::Provider &current,
                    std::optional<std::pair<core::Epoch, core::Epoch>> interval);
} // namespace orbitforge::desktop
