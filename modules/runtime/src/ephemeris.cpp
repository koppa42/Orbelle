#include <orbitforge/runtime/ephemeris.hpp>
#ifdef ORBITFORGE_HAS_SPICE
#include <orbitforge/providers/spice/provider.hpp>
#endif

namespace orbitforge::runtime {
bool ephemeris_available() noexcept {
#ifdef ORBITFORGE_HAS_SPICE
    return true;
#else
    return false;
#endif
}
std::unique_ptr<core::IEphemerisProvider>
load_spice_snapshot(const std::filesystem::path &manifest, const std::filesystem::path &workspace) {
#ifdef ORBITFORGE_HAS_SPICE
    return providers::spice::load_snapshot(manifest, workspace);
#else
    (void)manifest;
    (void)workspace;
    throw std::runtime_error("This build does not include the CSPICE ephemeris provider");
#endif
}
} // namespace orbitforge::runtime
