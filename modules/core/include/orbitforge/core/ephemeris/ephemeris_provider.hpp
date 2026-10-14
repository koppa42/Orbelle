#pragma once
#include <orbitforge/core/frames/state.hpp>
#include <orbitforge/core/time/time_provider.hpp>
#include <span>
#include <vector>

namespace orbitforge::core {
class EphemerisDataError : public std::runtime_error {
  public:
    using std::runtime_error::runtime_error;
};
class EphemerisQueryError : public std::runtime_error {
  public:
    using std::runtime_error::runtime_error;
};
class EphemerisCoverageError : public std::out_of_range {
  public:
    using std::out_of_range::out_of_range;
};

// Double precision TDB seconds past 2000-01-01 12:00:00 TDB (SPICE ET).
// This numerical query time does not preserve Epoch's nanosecond resolution.
struct TdbTime {
    double seconds_past_j2000;
};
struct EphemerisState {
    Epoch epoch;
    std::int32_t target_naif_id;
    std::int32_t observer_naif_id;
    FrameId frame;
    std::array<double, 3> position_m;
    std::array<double, 3> velocity_mps;
    TimeScale velocity_time_scale; // Derivative is per TDB second, not per TAI second.
    TdbTime tdb;
    std::string source_id;
};
struct KernelDataInfo {
    std::string file;
    std::string kind;
    std::string sha256;
    std::string source_url;
    std::string source_sha256;
    std::string producer;
};
struct EphemerisDataInfo {
    std::string id;
    std::string manifest_sha256;
    std::string model;
    std::string toolkit_version;
    Epoch first_epoch;
    Epoch last_epoch; // Closed policy window; actual SPK coverage is checked on query.
    std::vector<KernelDataInfo> kernels; // Load order is significant.
};
class IEphemerisProvider {
  public:
    virtual ~IEphemerisProvider() = default;
    [[nodiscard]] virtual TdbTime to_tdb(Epoch epoch) const = 0;
    [[nodiscard]] virtual Epoch from_tdb(TdbTime time) const = 0;
    [[nodiscard]] virtual EphemerisState geometric_state(std::int32_t target, std::int32_t observer,
                                                         Epoch epoch) const = 0;
    [[nodiscard]] virtual std::vector<EphemerisState>
    geometric_states(std::int32_t target, std::int32_t observer,
                     std::span<const Epoch> epochs) const = 0;
    [[nodiscard]] virtual const EphemerisDataInfo &data_info() const noexcept = 0;
    // Validate continuous DE-style Earth/Sun/Moon segment chains, including inertial
    // axes and centres. Providers that cannot certify this must reject HPOP use.
    virtual void validate_earth_gravity_interval(Epoch, Epoch, bool, bool) const {
        throw EphemerisDataError(
            "This ephemeris cannot validate Earth third-body gravity coverage");
    }
};
} // namespace orbitforge::core
