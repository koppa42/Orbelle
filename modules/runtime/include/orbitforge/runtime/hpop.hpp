#pragma once
#include <orbitforge/application/session.hpp>
#include <orbitforge/core/propagation/hpop.hpp>

namespace orbitforge::runtime {
[[nodiscard]] application::Session make_hpop_session(core::CartesianState initial,
                                                     core::HpopOptions options = {},
                                                     core::HpopEnvironment environment = {},
                                                     std::string object_id = "satellite-1",
                                                     std::uint64_t input_revision = 1);
} // namespace orbitforge::runtime
