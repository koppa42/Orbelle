#pragma once
#include <orbitforge/core/frames/eop_provider.hpp>
#include <span>
#include <vector>

namespace orbitforge::application {
// A path-independent reference. The content hash identifies raw file bytes;
// configuration covers their interpretation and transitive data dependencies.
struct DataReference {
    std::string role;
    std::string snapshot_id;
    std::string content_sha256;
    std::string interpretation_version;
    std::string configuration_sha256;
    bool operator==(const DataReference &) const = default;
};
class DataLock {
  public:
    explicit DataLock(std::vector<DataReference> references = {});
    [[nodiscard]] std::span<const DataReference> references() const noexcept { return references_; }
    [[nodiscard]] const std::string &sha256() const noexcept { return sha256_; }
    // Stable v1 encoding, also used for independent digest verification.
    [[nodiscard]] std::vector<std::uint8_t> canonical_bytes() const;
    bool operator==(const DataLock &) const = default;

  private:
    std::vector<DataReference> references_;
    std::string sha256_;
};
[[nodiscard]] DataLock eop_data_lock(const core::EopDataInfo &info);
[[nodiscard]] DataLock time_data_lock(const core::TimeDataInfo &info);
} // namespace orbitforge::application
