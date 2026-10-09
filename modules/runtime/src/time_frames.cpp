#include <orbitforge/runtime/time_frames.hpp>
#include <stdexcept>
#ifdef ORBITFORGE_HAS_ERFA
#include <orbitforge/providers/erfa/provider.hpp>
#include <orbitforge/providers/iers/eop_snapshot.hpp>
#endif

namespace orbitforge::runtime {
bool time_frames_available() noexcept {
#ifdef ORBITFORGE_HAS_ERFA
    return true;
#else
    return false;
#endif
}
std::unique_ptr<core::ITimeProvider> make_builtin_time_provider() {
#ifdef ORBITFORGE_HAS_ERFA
    return providers::erfa::make_builtin_time_provider();
#else
    throw std::runtime_error("Time provider requires a build with ORBITFORGE_WITH_ERFA=ON");
#endif
}
std::unique_ptr<core::IFrameProvider> make_frame_provider() {
#ifdef ORBITFORGE_HAS_ERFA
    return providers::erfa::make_frame_provider();
#else
    throw std::runtime_error("Frame provider requires a build with ORBITFORGE_WITH_ERFA=ON");
#endif
}
std::unique_ptr<core::IEarthOrientationProvider>
load_eop_snapshot(const std::filesystem::path &manifest) {
#ifdef ORBITFORGE_HAS_ERFA
    const auto time = make_builtin_time_provider();
    return providers::iers::load_eop_snapshot(manifest, *time);
#else
    (void)manifest;
    throw std::runtime_error("EOP loading requires a build with ORBITFORGE_WITH_ERFA=ON");
#endif
}
} // namespace orbitforge::runtime
