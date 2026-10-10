#pragma once
#include "data_inventory.hpp"
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
    [[nodiscard]] const std::vector<std::size_t> &current_indices() const { return current_; }
    [[nodiscard]] std::size_t preferred_index() const {
        return current_.empty() ? 0 : current_.front();
    }
    [[nodiscard]] std::uint64_t revision() const { return revision_; }
    [[nodiscard]] std::vector<DataInventoryItem> inventory() const;
    [[nodiscard]] DataUpdateStatus update_status() const;
    void set_automatic_updates(bool enabled);
    [[nodiscard]] bool automatic_updates() const;
    void set_update_interval_hours(int hours);
    [[nodiscard]] int update_interval_hours() const;
    void set_extrapolation_days(std::size_t index, int days);
    [[nodiscard]] int extrapolation_days(std::size_t index) const;
    [[nodiscard]] Provider for_interval(core::Epoch first, core::Epoch last) const;
    void start_download(QUrl url = {});
    void cancel_download();
    void check_updates_if_due(qint64 now, QUrl source = {});
    [[nodiscard]] bool has_full_download() const;
    [[nodiscard]] bool downloading() const { return reply_ != nullptr; }
    [[nodiscard]] const QString &status() const { return status_; }
    [[nodiscard]] qint64 received_bytes() const { return received_; }
    [[nodiscard]] qint64 total_bytes() const { return total_; }
    [[nodiscard]] std::optional<std::size_t> last_installed() const { return last_installed_; }
    void import_raw(const std::filesystem::path &source);

  private:
    std::size_t append(Provider provider);
    void refresh_current(std::optional<std::size_t> acquired = std::nullopt);
    [[nodiscard]] std::optional<std::size_t> current_finals() const;
    void save_update_status();
    std::filesystem::path store_;
    runtime::DataCatalog data_;
    std::vector<Provider> providers_;
    std::vector<std::size_t> current_;
    std::uint64_t revision_ = 0;
    QStringList warnings_;
    QNetworkAccessManager network_;
    QNetworkReply *reply_ = nullptr;
    std::unique_ptr<QTemporaryFile> download_;
    QTimer schedule_, deadline_;
    QString status_;
    qint64 received_ = 0;
    qint64 total_ = -1;
    DataUpdateStatus update_;
    std::vector<std::pair<std::string, std::filesystem::path>> bundled_;
    bool cancelled_ = false;
    QString transfer_error_;
    std::optional<std::size_t> last_installed_;
};
void show_data_center(QWidget *parent, EopCatalog &catalog);
} // namespace orbitforge::desktop
