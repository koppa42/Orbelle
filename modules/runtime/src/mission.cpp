#include <orbitforge/core/propagation/central_body.hpp>
#include <orbitforge/runtime/hpop.hpp>
#include <orbitforge/runtime/mission.hpp>
#include <orbitforge/runtime/two_body.hpp>

namespace orbitforge::runtime {
namespace {
// Keep the provider boundary behind Session: optional provider dependencies and
// their algorithm identities remain owned by the existing runtime factories.
class CoastSession final : public core::IStateProvider {
  public:
    explicit CoastSession(application::Session session) : session_(std::move(session)) {}
    core::CartesianState state_at(core::Epoch epoch) const override { return state_at(epoch, {}); }
    core::CartesianState state_at(core::Epoch epoch,
                                  const std::function<void()> &checkpoint) const override {
        const std::array epochs{epoch};
        return session_.sample(epochs, checkpoint).states().front();
    }

  private:
    application::Session session_;
};
} // namespace
application::Session make_mission_session(core::CartesianState initial, core::MissionPlan plan,
                                          core::HpopOptions options,
                                          core::HpopEnvironment environment, std::string object_id,
                                          std::uint64_t revision) {
    (void)core::validate_mission(initial.epoch, plan);
    const auto hpop = plan.coast_model == core::MissionCoastModel::hpop;
    const auto make_coast = [hpop, options, environment](core::CartesianState state) {
        return hpop ? make_hpop_session(state, options, environment)
                    : make_two_body_session(state, core::central_body(state.origin).mu, "coast", 1);
    };
    const auto algorithm =
        "mission-impulsive-gcrf-tai-v1;coast=" + make_coast(initial).provenance().algorithm;
    core::CoastFactory factory = [make_coast](core::CartesianState state) {
        return std::make_shared<CoastSession>(make_coast(state));
    };
    return {core::make_mission(initial, std::move(plan), std::move(factory)),
            {std::move(object_id), revision, algorithm}};
}
} // namespace orbitforge::runtime
