#pragma once
#include <array>
#include <orbelle/application/session.hpp>

namespace orbelle::runtime {
// Shared composition entry point for the P0 C++, Python and desktop examples.
[[nodiscard]] application::Session make_circular_demo_session();
[[nodiscard]] std::array<core::Epoch, 6> demo_epochs();
} // namespace orbelle::runtime
