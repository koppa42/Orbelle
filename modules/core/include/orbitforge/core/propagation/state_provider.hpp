#pragma once
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
};
} // namespace orbitforge::core
