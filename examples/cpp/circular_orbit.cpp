#include <iomanip>
#include <iostream>
#include <orbitforge/runtime/demo.hpp>

int main() {
    const auto session = orbitforge::runtime::make_circular_demo_session();
    const auto result = session.sample(orbitforge::runtime::demo_epochs());
    std::cout << "tai_ns,x_m,y_m,z_m,vx_mps,vy_mps,vz_mps\n" << std::setprecision(17);
    for (const auto &state : result.states()) {
        std::cout << state.epoch.tai_nanoseconds();
        for (const auto x : state.position_m) {
            std::cout << ',' << x;
        }
        for (const auto v : state.velocity_mps) {
            std::cout << ',' << v;
        }
        std::cout << '\n';
    }
}
