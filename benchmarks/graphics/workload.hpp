#pragma once
#include <orbitforge/core/propagation/circular_two_body.hpp>
#include <orbitforge/visualization/snapshot.hpp>

namespace orbitforge::benchmarks {
// Rendering fixture, not a catalog or the general P1 propagator. Positions use
// Core's circular solution, rotated into deterministic GCRF orbital planes.
class Constellation {
  public:
    static constexpr std::size_t planes = 100, satellites_per_plane = 100;
    static constexpr std::size_t count = planes * satellites_per_plane;
    Constellation();
    [[nodiscard]] visualization::RenderSnapshot at(core::Epoch epoch) const;

  private:
    struct Plane {
        core::CircularTwoBody orbit;
        double cos_raan, sin_raan, cos_inclination, sin_inclination;
    };
    std::vector<Plane> planes_;
};

class DisplayCache {
  public:
    static constexpr std::int64_t duration_seconds = 600;
    // Precompute one-second ITRF samples. Storage contains positions, not a second
    // implementation of propagation. This cost is measured separately from display.
    DisplayCache(const core::IFrameProvider &frames, const core::IEarthOrientationProvider &eop);
    [[nodiscard]] visualization::RenderSnapshot sample(core::Epoch epoch) const;
    [[nodiscard]] std::vector<visualization::RenderSnapshot> track(std::size_t object_index) const;
    [[nodiscard]] std::size_t position_bytes() const noexcept;

  private:
    std::vector<std::string> ids_;
    std::vector<std::array<double, 3>> positions_;
};
} // namespace orbitforge::benchmarks
