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
std::filesystem::path import_eop_snapshot(const std::filesystem::path &manifest,
                                          const std::filesystem::path &store) {
#ifdef ORBITFORGE_HAS_ERFA
    const auto time = make_builtin_time_provider();
    return providers::iers::import_eop_snapshot(manifest, store, *time);
#else
    (void)manifest;
    (void)store;
    throw std::runtime_error("EOP import requires a build with ORBITFORGE_WITH_ERFA=ON");
#endif
}
std::array<double, 3> display_sun_direction_gcrf(core::Epoch epoch) {
#ifdef ORBITFORGE_HAS_ERFA
    return providers::erfa::display_sun_direction_gcrf(epoch);
#else
    (void)epoch;
    throw std::runtime_error("Display sunlight requires ORBITFORGE_WITH_ERFA=ON");
#endif
}
} // namespace orbitforge::runtime
