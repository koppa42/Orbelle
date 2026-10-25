#pragma once
#include "data_inventory.hpp"
#include <QNetworkAccessManager>
#include <QNetworkReply>
#include <QObject>
#include <QStringList>
#include <QTemporaryFile>
#include <QTimer>
#include <array>
#include <atomic>
#include <functional>
#include <future>
#include <optional>
#include <orbelle/runtime/data_catalog.hpp>
#include <orbelle/runtime/time_frames.hpp>
#include <vector>
class QWidget;

namespace orbelle::desktop {
struct DataUpdatePreference {
    bool automatic = false;
    int hours = 168;
    bool operator==(const DataUpdatePreference &) const = default;
};
struct DataPreferences {
    DataProduct preferred = DataProduct::c04;
    int finals_extrapolation_days = 0, c04_extrapolation_days = 0;
    std::map<DataProduct, DataUpdatePreference> updates;
    bool operator==(const DataPreferences &) const = default;
};
class EopCatalog : public QObject {
  public:
    // The desktop supplies its shared preferences entry. Nested editors use the
    // same route without owning the application settings or opening a second inspector.
    std::function<void(QWidget *, std::optional<DataProduct>)> open_data_center;
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
    [[nodiscard]] DataUpdateStatus
    update_status(DataProduct product = DataProduct::finals2000a) const;
    [[nodiscard]] std::shared_ptr<const core::ITimeProvider> time_provider() const { return time_; }
    [[nodiscard]] DataProduct preferred_product() const;
    [[nodiscard]] DataPreferences preferences() const;
    // Validate every field and publish the complete preference batch in one transaction.
    void set_preferences(const DataPreferences &preferences);
    void set_preferred_product(DataProduct product);
    void set_automatic_updates(bool enabled, DataProduct product = DataProduct::finals2000a);
    [[nodiscard]] bool automatic_updates(DataProduct product = DataProduct::finals2000a) const;
    void set_update_interval_hours(int hours, DataProduct product = DataProduct::finals2000a);
    [[nodiscard]] int update_interval_hours(DataProduct product = DataProduct::finals2000a) const;
    void set_extrapolation_days(std::size_t index, int days);
    [[nodiscard]] int extrapolation_days(std::size_t index) const;
    [[nodiscard]] Provider for_interval(core::Epoch first, core::Epoch last) const;
    void start_download(QUrl url = {}, DataProduct product = DataProduct::finals2000a);
    void start_download_from_sources(const QList<QUrl> &sources,
                                     DataProduct product = DataProduct::finals2000a);
    void cancel_download();
    // Verify installed fixed-model bytes locally, without changing acquisition or model identity.
    void verify_files(DataProduct product);
    [[nodiscard]] bool verifying_files() const { return verifying_ && resource_job_.valid(); }
    void check_updates_if_due(qint64 now, QUrl source = {});
    void set_update_schedule_enabled(bool enabled);
    [[nodiscard]] bool has_full_download(DataProduct product = DataProduct::finals2000a) const;
    [[nodiscard]] bool downloading() const {
        return reply_ != nullptr || retry_.isActive() || resource_job_.valid();
    }
    [[nodiscard]] DataProduct active_product() const { return active_product_; }
    [[nodiscard]] const QString &status() const { return status_; }
    [[nodiscard]] qint64 received_bytes() const { return received_; }
    [[nodiscard]] qint64 total_bytes() const { return total_; }
    [[nodiscard]] std::optional<std::size_t> last_installed() const { return last_installed_; }
    void import_raw(const std::filesystem::path &source,
                    DataProduct product = DataProduct::finals2000a);

    void import_ephemeris_snapshot(const std::filesystem::path &source);
    void import_planetary_centres(const std::filesystem::path &source);

  private:
    enum class EphemerisImport { base, snapshot, planetary_centres };
    std::size_t append(Provider provider);
    void refresh_current(std::optional<std::size_t> acquired = std::nullopt);
    [[nodiscard]] std::optional<std::size_t> current_index(DataProduct product) const;
    [[nodiscard]] std::string current_hash(DataProduct product) const;
    void install_source(const std::filesystem::path &source, DataProduct product,
                        const std::string &url, runtime::DataAcquisition acquisition);
    DataUpdateStatus &update(DataProduct product) {
        return updates_[static_cast<std::size_t>(product)];
    }
    void save_update_status();
    void start_download_attempt();
    void finish_download_failure(const QString &message);
    bool recover_download(QNetworkReply &reply, int http, bool conditional);
    void record_download_failure(const QNetworkReply &reply, int http, const QString &message);
    void start_resource_install(const std::filesystem::path &file, DataProduct product,
                                const std::string &url, runtime::DataAcquisition acquisition,
                                EphemerisImport ephemeris_import = EphemerisImport::base);
    std::filesystem::path store_;
    runtime::DataCatalog data_;
    std::vector<Provider> providers_;
    std::vector<std::size_t> current_;
    std::uint64_t revision_ = 0;
    QStringList warnings_;
    QNetworkAccessManager network_;
    QNetworkReply *reply_ = nullptr;
    std::unique_ptr<QTemporaryFile> download_;
    QTimer schedule_, deadline_, retry_;
    QList<QUrl> download_sources_;
    QUrl request_url_;
    qsizetype source_index_ = 0;
    int source_attempt_ = 0;
    int redirects_ = 0;
    bool unconditional_ = false;
    QByteArray error_response_;
    QString status_;
    qint64 received_ = 0;
    qint64 total_ = -1;
    std::array<DataUpdateStatus, all_data_products.size()> updates_;
    struct InstalledResource {
        std::string sha256;
        runtime::DataAcquisition acquisition;
    };
    std::future<InstalledResource> resource_job_;
    std::shared_ptr<std::atomic_bool> resource_cancel_;
    QTimer resource_poll_;
    std::map<std::string, std::string> resource_response_;
    bool verifying_ = false;
    DataProduct active_product_ = DataProduct::finals2000a;
    std::shared_ptr<const core::ITimeProvider> time_;
    std::filesystem::path time_file_;
    std::vector<std::pair<std::string, std::filesystem::path>> bundled_;
    bool cancelled_ = false;
    QString transfer_error_;
    std::optional<std::size_t> last_installed_;
};
void show_data_center(QWidget *parent, EopCatalog &catalog,
                      std::optional<DataProduct> initial = {});
} // namespace orbelle::desktop
