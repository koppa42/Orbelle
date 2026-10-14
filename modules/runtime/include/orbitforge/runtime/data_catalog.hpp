#pragma once
#include <cstdint>
#include <filesystem>
#include <map>
#include <memory>
#include <optional>
#include <orbitforge/application/data_lock.hpp>
#include <orbitforge/runtime/analysis.hpp>
#include <string>
#include <vector>

namespace orbitforge::runtime {
enum class DataAcquisition { unknown, bundled, imported, downloaded };
enum class EopProduct { c04, finals2000a };
struct CatalogGravity {
    std::string selection;
    core::GravityInfo info;
    std::filesystem::path file;
    DataAcquisition acquisition = DataAcquisition::unknown;
    std::int64_t acquired_at_unix_seconds = 0;
};
struct CatalogSnapshot {
    std::string lock_sha256;
    std::string snapshot_id;
    std::string source_url;
    core::Epoch first{0}, last{0};
    std::filesystem::path manifest;
    DataAcquisition acquisition = DataAcquisition::unknown;
    std::optional<std::int64_t> acquired_at_unix_seconds = std::nullopt;
};
[[nodiscard]] bool data_catalog_available() noexcept;
// Shared on-disk catalog. Methods are serialized per instance and SQLite serializes
// publication across processes. No method changes a running Scenario or downloads.
class DataCatalog {
  public:
    explicit DataCatalog(std::filesystem::path root);
    ~DataCatalog();
    DataCatalog(const DataCatalog &) = delete;
    DataCatalog &operator=(const DataCatalog &) = delete;
    [[nodiscard]] const std::filesystem::path &root() const noexcept;
    // Validate the complete file before atomically replacing this model's current receipt.
    // Static fields have no expiry. Failed/cancelled installs retain the previous selection.
    CatalogGravity install_gravity(const std::filesystem::path &source,
                                   const std::string &selection, const std::string &source_url = {},
                                   DataAcquisition acquisition = DataAcquisition::imported,
                                   const std::function<void()> &checkpoint = {});
    [[nodiscard]] std::vector<CatalogGravity> gravity_models() const;
    [[nodiscard]] std::shared_ptr<const core::GravityModel>
    load_gravity(const std::string &selection) const;
    // Reads a coherent local catalog view; never downloads or changes current pointers.
    // Full coverage wins over extrapolation, then preferred_eop() orders the sources.
    [[nodiscard]] DataSnapshot snapshot(core::Epoch first, core::Epoch last,
                                        bool earth_orientation = true) const;
    [[nodiscard]] EopProduct preferred_eop() const;
    void set_preferred_eop(EopProduct product);
    [[nodiscard]] int extrapolation_days(EopProduct product) const;
    void set_extrapolation_days(EopProduct product, int days);
    // Explicit activation is independent of installation and preserves receipt metadata.
    // Rejects an EOP resource whose UTC dependency differs from the catalog's current UTC.
    void activate_eop(const std::string &lock_sha256);
    CatalogSnapshot install_eop(const std::filesystem::path &manifest,
                                const std::filesystem::path &time_file = {});
    // Installs the entire original file. Coverage is the complete daily prefix
    // intersected with the time provider validity; incomplete predictions stay in the blob.
    CatalogSnapshot import_finals(const std::filesystem::path &source,
                                  const std::string &source_url, const std::string &retrieved_on,
                                  const std::filesystem::path &time_file = {});
    CatalogSnapshot import_c04(const std::filesystem::path &source, const std::string &source_url,
                               const std::string &retrieved_on,
                               const std::filesystem::path &time_file = {});
    // Publish a validated immutable UTC file; activation is a separate settings transaction.
    [[nodiscard]] std::filesystem::path install_time(const std::filesystem::path &source);
    [[nodiscard]] std::filesystem::path time_file(const std::string &sha256) const;
    [[nodiscard]] std::vector<CatalogSnapshot> snapshots() const;
    // Local receipt metadata is separate from immutable content and Scenario locks.
    // Call only after successful acquisition; a 304 response is not a download.
    void record_acquisition(const std::string &lock_sha256, DataAcquisition acquisition,
                            std::optional<std::int64_t> acquired_at = std::nullopt);
    [[nodiscard]] std::unique_ptr<core::IEarthOrientationProvider>
    resolve_eop(const application::DataLock &lock) const;
    [[nodiscard]] std::unique_ptr<core::IEarthOrientationProvider>
    load_eop(const std::string &lock_sha256) const;
    [[nodiscard]] std::string setting(const std::string &key) const;
    void set_setting(const std::string &key, const std::string &value);
    void set_settings(const std::map<std::string, std::string> &values);

  private:
    struct Impl;
    std::unique_ptr<Impl> impl_;
};
} // namespace orbitforge::runtime
