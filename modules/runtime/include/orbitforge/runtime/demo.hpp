#pragma once
#include <array>
#include <orbitforge/application/session.hpp>

namespace orbitforge::runtime {
// Shared composition entry point for the P0 C++, Python and desktop examples.
[[nodiscard]] application::Session make_circular_demo_session();
[[nodiscard]] std::array<core::Epoch, 6> demo_epochs();
} // namespace orbitforge::runtime
