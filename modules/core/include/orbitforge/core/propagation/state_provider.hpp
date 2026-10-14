#pragma once
#include <functional>
#include <orbitforge/core/frames/state.hpp>
#include <stdexcept>

namespace orbitforge::core {
class PropagationError : public std::runtime_error {
  public:
    using std::runtime_error::runtime_error;
};

class IStateProvider {
  public:
    virtual ~IStateProvider() = default;
    [[nodiscard]] virtual CartesianState state_at(Epoch epoch) const = 0;
    // Numerical providers also call checkpoint during integration. A throwing
    // checkpoint cancels the query; analytical providers need only one check.
    [[nodiscard]] virtual CartesianState state_at(Epoch epoch,
                                                  const std::function<void()> &checkpoint) const {
        if (checkpoint)
            checkpoint();
        return state_at(epoch);
    }
};
} // namespace orbitforge::core
