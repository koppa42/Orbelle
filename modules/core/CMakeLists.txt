orbitforge_module(orbitforge_core Core
    src/propagation/circular_two_body.cpp src/propagation/ground_station.cpp src/frames/position_rotation.cpp
    src/frames/state_transform.cpp src/propagation/tle.cpp)
if(BUILD_TESTING)
    add_subdirectory(tests)
endif()

