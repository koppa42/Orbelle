#include <memory>
#include <orbitforge/core/propagation/circular_two_body.hpp>
#include <orbitforge/runtime/demo.hpp>
namespace orbitforge::runtime {
application::Session make_circular_demo_session() {
    auto provider = std::make_shared<core::CircularTwoBody>(
        core::Length{7'000'000.0}, core::GravitationalParameter{3.986004418e14}, core::Epoch{0});
    return {std::move(provider), {"demo-satellite", 1, "p0-circular-two-body-v1"}};
}
std::array<core::Epoch, 6> demo_epochs() {
    return {core::Epoch{0},
            core::Epoch{60'000'000'000},
            core::Epoch{120'000'000'000},
            core::Epoch{180'000'000'000},
            core::Epoch{240'000'000'000},
            core::Epoch{300'000'000'000}};
}
} // namespace orbitforge::runtime
