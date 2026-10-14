orbitforge_module(orbitforge_core Core
    src/propagation/circular_two_body.cpp src/propagation/ground_station.cpp src/frames/position_rotation.cpp
    src/frames/state_transform.cpp src/propagation/tle.cpp src/propagation/access.cpp
    src/propagation/earth_gravity.cpp src/propagation/hpop.cpp src/propagation/mission.cpp src/propagation/third_body.cpp)
if(BUILD_TESTING)
    add_subdirectory(tests)
endif()

