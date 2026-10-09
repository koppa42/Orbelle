#pragma once
#include <memory>
#include <orbitforge/core/frames/frame_provider.hpp>
#include <orbitforge/core/time/time_provider.hpp>

namespace orbitforge::providers::erfa {
[[nodiscard]] std::unique_ptr<core::ITimeProvider> make_builtin_time_provider();
[[nodiscard]] std::unique_ptr<core::IFrameProvider> make_frame_provider();
} // namespace orbitforge::providers::erfa
