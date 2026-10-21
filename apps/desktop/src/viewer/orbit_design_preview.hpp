#pragma once
#include "ground_map.hpp"
#include <future>
#include <orbitforge/core/propagation/orbit_design.hpp>

class QComboBox;
class QLabel;
namespace orbitforge::desktop {
// Providers prepare exact, reusable time grids off-thread. Bounded interactive
// projection uses the latest design on Qt's thread; detailed sampling is asynchronous.
class OrbitDesignPreview final : public QWidget {
  public:
    explicit OrbitDesignPreview(QWidget *parent = nullptr);
    ~OrbitDesignPreview() override;
    void set_context(QImage imagery, std::shared_ptr<const core::IFrameProvider> frames,
                     std::shared_ptr<const core::IEarthOrientationProvider> eop);
    void set_design(std::optional<core::EarthOrbitDesign> design);
    [[nodiscard]] bool ready() const {
        // Data readiness is independent of whether a scrolled-out map can paint.
        return published_revision_ == revision_ && published_full_ && design_.has_value();
    }

  private:
    struct RotationGrid {
        core::Epoch epoch{0};
        double duration = 0;
        int segments = 0;
        std::vector<core::Epoch> epochs;
        std::vector<core::Matrix3> rotations;
        bool matches(core::Epoch start, double seconds, int samples) const {
            return epoch == start && duration == seconds && segments == samples;
        }
        bool covers(double seconds) const {
            return !epochs.empty() &&
                   epochs.back().tai_nanoseconds() - epoch.tai_nanoseconds() >= seconds * 1e9;
        }
    };
    struct Result {
        GroundMap::Track track;
        visualization::RenderSnapshot marker{
            core::Epoch{0}, core::FrameId::itrf, core::OriginId::earth, {}};
        bool cancelled = false;
        bool full = false;
        std::shared_ptr<const RotationGrid> grid;
    };
    void request_update();
    void start();
    void finish();
    static Result sample(const core::EarthOrbitDesign &design,
                         std::shared_ptr<const RotationGrid> grid, double duration, bool full,
                         const std::function<void()> &checkpoint);
    void publish(Result result);
    GroundMap *map_ = nullptr;
    QLabel *status_;
    QComboBox *revolutions_;
    QTimer update_timer_, refine_timer_;
    std::future<Result> result_;
    std::optional<core::EarthOrbitDesign> design_;
    std::shared_ptr<const core::IFrameProvider> frames_;
    std::shared_ptr<const core::IEarthOrientationProvider> eop_;
    std::shared_ptr<std::atomic_bool> cancel_;
    std::future<void> pending_;
    // Two adjacent interactive time spans plus the settled, exact-duration grid.
    std::array<std::shared_ptr<const RotationGrid>, 3> rotation_cache_;
    std::uint64_t revision_ = 1, running_revision_ = 0, published_revision_ = 0;
    std::uint64_t context_revision_ = 0, running_context_ = 0;
    bool dirty_ = false, full_requested_ = false, published_full_ = false, running_full_ = false;
};
} // namespace orbitforge::desktop
