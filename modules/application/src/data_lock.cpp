#include <algorithm>
#include <bit>
#include <orbitforge/application/data_lock.hpp>
#include <picosha2.h>
#include <stdexcept>

namespace orbitforge::application {
namespace {
void check(bool valid, const char *message) {
    if (!valid)
        throw std::invalid_argument(message);
}
bool valid_hash(const std::string &hash) {
    return hash.size() == 64 && std::all_of(hash.begin(), hash.end(), [](char c) {
               return (c >= '0' && c <= '9') || (c >= 'a' && c <= 'f');
           });
}
struct Canonical {
    std::vector<std::uint8_t> bytes;
    void integer(std::uint64_t value, unsigned width = 8) {
        for (unsigned i = 0; i < width; ++i)
            bytes.push_back(static_cast<std::uint8_t>(value >> (8 * i)));
    }
    void text(const std::string &value) {
        check(!value.empty() && value.size() <= 16384 && value.find('\0') == std::string::npos,
              "Invalid data-reference text");
        integer(value.size(), 4);
        bytes.insert(bytes.end(), value.begin(), value.end());
    }
    void epoch(core::Epoch value) {
        integer(std::bit_cast<std::uint64_t>(value.tai_nanoseconds()));
    }
    [[nodiscard]] std::string digest() const { return picosha2::hash256_hex_string(bytes); }
};
} // namespace
DataLock::DataLock(std::vector<DataReference> references) : references_(std::move(references)) {
    check(references_.size() <= 64, "Data lock exceeds 64 references");
    std::sort(references_.begin(), references_.end(),
              [](const auto &a, const auto &b) { return a.role < b.role; });
    std::string previous;
    for (const auto &reference : references_) {
        check(!reference.role.empty() && reference.role.size() <= 128 &&
                  std::all_of(reference.role.begin(), reference.role.end(),
                              [](char c) {
                                  return (c >= 'a' && c <= 'z') || (c >= '0' && c <= '9') ||
                                         c == '-' || c == '/' || c == '.';
                              }),
              "Invalid data-reference role");
        check(reference.role != previous, "Duplicate data-reference role");
        check(valid_hash(reference.content_sha256) && valid_hash(reference.configuration_sha256),
              "Invalid data-reference SHA256");
        previous = reference.role;
    }
    sha256_ = picosha2::hash256_hex_string(canonical_bytes());
}
std::vector<std::uint8_t> DataLock::canonical_bytes() const {
    Canonical result;
    result.text("orbitforge-data-lock-v1");
    result.integer(references_.size(), 4);
    for (const auto &reference : references_) {
        result.text(reference.role);
        result.text(reference.snapshot_id);
        result.text(reference.content_sha256);
        result.text(reference.interpretation_version);
        result.text(reference.configuration_sha256);
    }
    return std::move(result.bytes);
}
DataLock eop_data_lock(const core::EopDataInfo &info) {
    const auto &time = info.time_data;
    check(valid_hash(time.sha256) && valid_hash(info.sha256), "Invalid data content SHA256");
    check(time.utc_valid_from_mjd >= 0 && time.utc_valid_from_mjd < time.utc_valid_until_mjd &&
              info.first_epoch.tai_nanoseconds() < info.last_epoch.tai_nanoseconds(),
          "Invalid data-reference coverage");
    constexpr auto time_version = "orbitforge-utc-tai-v1";
    Canonical time_config;
    time_config.text(time_version);
    time_config.integer(static_cast<std::uint32_t>(time.utc_valid_from_mjd), 4);
    time_config.integer(static_cast<std::uint32_t>(time.utc_valid_until_mjd), 4);
    DataReference time_ref{"utc-time", time.id, time.sha256, time_version, time_config.digest()};

    constexpr auto eop_version = "orbitforge-eop-reader-v1";
    Canonical eop_config;
    eop_config.text(eop_version);
    eop_config.text(info.reference_frame);
    eop_config.text(info.interpolation);
    eop_config.epoch(info.first_epoch);
    eop_config.epoch(info.last_epoch);
    // Include the complete time dependency identity, not just its display ID.
    eop_config.text(DataLock({time_ref}).sha256());
    DataReference eop_ref{"earth-orientation", info.id, info.sha256, eop_version,
                          eop_config.digest()};
    return DataLock({std::move(eop_ref), std::move(time_ref)});
}
} // namespace orbitforge::application
