#pragma once
#include <cstdint>
#include <filesystem>
#include <memory>
#include <optional>
#include <orbitforge/application/data_lock.hpp>
#include <string>
#include <vector>

namespace orbitforge::runtime {
enum class DataAcquisition { unknown, bundled, imported, downloaded };
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
    CatalogSnapshot install_eop(const std::filesystem::path &manifest);
    // Installs the entire original file. Coverage is the complete daily prefix
    // intersected with the time provider validity; incomplete predictions stay in the blob.
    CatalogSnapshot import_finals(const std::filesystem::path &source,
                                  const std::string &source_url, const std::string &retrieved_on);
    [[nodiscard]] std::vector<CatalogSnapshot> snapshots() const;
    // Local receipt metadata is separate from immutable content and Scenario locks.
    // Call only after successful acquisition; a 304 response is not a download.
    void record_acquisition(const std::string &lock_sha256, DataAcquisition acquisition);
    [[nodiscard]] std::unique_ptr<core::IEarthOrientationProvider>
    resolve_eop(const application::DataLock &lock) const;
    [[nodiscard]] std::unique_ptr<core::IEarthOrientationProvider>
    load_eop(const std::string &lock_sha256) const;
    [[nodiscard]] std::string setting(const std::string &key) const;
    void set_setting(const std::string &key, const std::string &value);

  private:
    struct Impl;
    std::unique_ptr<Impl> impl_;
};
} // namespace orbitforge::runtime
