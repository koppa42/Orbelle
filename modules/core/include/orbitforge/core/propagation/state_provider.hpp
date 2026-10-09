#pragma once
#include <orbitforge/core/frames/state.hpp>

namespace orbitforge::core {
class IStateProvider {
  public:
    virtual ~IStateProvider() = default;
    [[nodiscard]] virtual CartesianState state_at(Epoch epoch) const = 0;
};
} // namespace orbitforge::core
