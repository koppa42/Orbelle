#pragma once
#include <filesystem>
#include <functional>
#include <orbitforge/application/data_lock.hpp>
#include <orbitforge/core/propagation/gravity.hpp>

namespace orbitforge::runtime {
// Static ICGEM .gfc and STK .grv subset. Reads and hashes the complete file, retains
// coefficients through core::gravity_max_degree, rejects time-dependent records.
// The returned resource owns all computation data; it never reopens its source.
[[nodiscard]] std::shared_ptr<const core::GravityModel>
load_gravity(const std::filesystem::path &file, const std::string &source_url = {},
             const std::function<void()> &checkpoint = {});
[[nodiscard]] application::DataReference gravity_reference(const std::string &selection,
                                                           const core::GravityInfo &info);
// Low-level callers must confirm the same bytes and interpretation as the project.
void validate_gravity_reference(std::span<const application::DataReference> recorded,
                                const std::string &selection, const core::GravityInfo &info);
[[nodiscard]] std::string gravity_download_url(const std::string &model);
} // namespace orbitforge::runtime
