#pragma once
#include <map>
#include <orbitforge/core/frames/frame_provider.hpp>

namespace orbitforge::runtime::detail {
// Owned by one sampled report, whose time/EOP providers are immutable snapshots.
// Reuse exact state transforms across objects, never across queries or providers.
class ReportFrameCache final : public core::IFrameProvider {
  public:
    explicit ReportFrameCache(const core::IFrameProvider &source) : source_(source) {}
    core::PositionRotation gcrf_to_itrf(core::Epoch epoch,
                                        const core::EarthOrientationSample &eop) const override {
        return source_.gcrf_to_itrf(epoch, eop);
    }
    core::StateTransform gcrf_to_itrf_state(core::Epoch epoch,
                                            const core::EarthOrientationState &eop) const override {
        return cached(core::FrameId::gcrf, epoch,
                      [&] { return source_.gcrf_to_itrf_state(epoch, eop); });
    }
    core::PositionRotation teme_to_itrf(core::Epoch epoch,
                                        const core::EarthOrientationSample &eop) const override {
        return source_.teme_to_itrf(epoch, eop);
    }
    core::StateTransform teme_to_itrf_state(core::Epoch epoch,
                                            const core::EarthOrientationState &eop) const override {
        return cached(core::FrameId::teme, epoch,
                      [&] { return source_.teme_to_itrf_state(epoch, eop); });
    }
    core::PositionRotation gcrf_to_celestial(core::Epoch epoch,
                                             const core::EarthOrientationSample &eop,
                                             core::FrameId frame) const override {
        return source_.gcrf_to_celestial(epoch, eop, frame);
    }
    core::StateTransform gcrf_to_celestial_state(core::Epoch epoch,
                                                 const core::EarthOrientationState &eop,
                                                 core::FrameId frame) const override {
        return source_.gcrf_to_celestial_state(epoch, eop, frame);
    }

  private:
    const core::IFrameProvider &source_;
    // Keep the first transforms rather than evicting entries during each object's
    // sequential scan. Longer reports still compute every uncached epoch exactly.
    static constexpr std::size_t capacity = 8192;
    mutable std::map<std::pair<core::FrameId, std::int64_t>, core::StateTransform> transforms_;
    template <class Factory>
    core::StateTransform cached(core::FrameId from, core::Epoch epoch, Factory factory) const {
        const auto key = std::pair{from, epoch.tai_nanoseconds()};
        if (const auto it = transforms_.find(key); it != transforms_.end())
            return it->second;
        auto transform = factory();
        if (transforms_.size() < capacity)
            transforms_.emplace(key, transform);
        return transform;
    }
};
} // namespace orbitforge::runtime::detail
