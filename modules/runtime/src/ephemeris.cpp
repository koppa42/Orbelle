#include <algorithm>
#include <orbitforge/runtime/ephemeris.hpp>
#include <picosha2.h>
#ifdef ORBITFORGE_HAS_SPICE
#include <orbitforge/providers/spice/provider.hpp>
#endif

namespace orbitforge::runtime {
namespace {
class ThirdBodyEphemeris final : public core::IThirdBodyEphemeris {
    std::shared_ptr<const core::IEphemerisProvider> source_;
    core::ThirdBodyDataInfo info_;

  public:
    explicit ThirdBodyEphemeris(std::shared_ptr<const core::IEphemerisProvider> source)
        : source_(std::move(source)) {
        if (!source_)
            throw std::invalid_argument("An ephemeris provider is required");
        const auto &info = source_->data_info();
        if (!info.model.starts_with("DE440") ||
            std::none_of(info.kernels.begin(), info.kernels.end(),
                         [](const auto &k) { return k.kind == "SPK"; }) ||
            std::any_of(info.kernels.begin(), info.kernels.end(), [](const auto &k) {
                return k.kind == "SPK" && k.source_sha256 != de440s_sha256;
            }))
            throw core::EphemerisDataError(
                "HPOP currently supports ICRF-aligned DE440s ephemerides only");
        info_ = {info.model,
                 info.manifest_sha256,
                 "de440-newtonian-geocentric-icrf-none-gm440-v1;" + info.toolkit_version,
                 info.first_epoch,
                 info.last_epoch,
                 de440_sun_mu,
                 de440_moon_mu};
    }
    std::array<double, 3> position(core::ThirdBody body, core::Epoch at) const override {
        if (body != core::ThirdBody::sun && body != core::ThirdBody::moon)
            throw std::invalid_argument("Unsupported third body");
        const auto state =
            source_->geometric_state(body == core::ThirdBody::sun ? 10 : 301, 399, at);
        if (state.frame != core::FrameId::spice_j2000)
            throw core::EphemerisDataError("DE440 force ephemeris returned unexpected axes");
        return state.position_m;
    }
    void validate_interval(core::Epoch first, core::Epoch last, bool sun,
                           bool moon) const override {
        if (first.tai_nanoseconds() < info_.first.tai_nanoseconds() ||
            last.tai_nanoseconds() > info_.last.tai_nanoseconds())
            throw core::EphemerisCoverageError(
                "Sun/Moon ephemeris coverage is insufficient. Required TAI ns [" +
                std::to_string(first.tai_nanoseconds()) + ", " +
                std::to_string(last.tai_nanoseconds()) + "]; available [" +
                std::to_string(info_.first.tai_nanoseconds()) + ", " +
                std::to_string(info_.last.tai_nanoseconds()) +
                "]. Select covering ephemerides in Data Center or adjust the orbit interval; no "
                "extrapolation.");
        source_->validate_earth_gravity_interval(first, last, sun, moon);
    }
    const core::ThirdBodyDataInfo &data_info() const noexcept override { return info_; }
};
} // namespace
std::shared_ptr<const core::IThirdBodyEphemeris>
make_third_body_ephemeris(std::shared_ptr<const core::IEphemerisProvider> source) {
    return std::make_shared<const ThirdBodyEphemeris>(std::move(source));
}
application::DataReference ephemeris_reference(const std::string &selection,
                                               const core::ThirdBodyDataInfo &info) {
    return {"ephemeris/" + selection, info.model, info.content_sha256, info.interpretation_version,
            picosha2::hash256_hex_string(info.interpretation_version)};
}
void validate_ephemeris_reference(std::span<const application::DataReference> recorded,
                                  const std::string &selection,
                                  const core::ThirdBodyDataInfo &info) {
    const auto selected = ephemeris_reference(selection, info);
    const auto found =
        std::ranges::find(recorded, selected.role, &application::DataReference::role);
    if (found == recorded.end() || *found != selected)
        throw std::invalid_argument(
            "Ephemeris differs from the project's confirmed data; use a preparation plan");
}
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
