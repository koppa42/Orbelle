orbitforge_module(orbitforge_runtime Runtime src/demo.cpp src/time_frames.cpp src/ephemeris.cpp
    src/two_body.cpp src/j2.cpp)
target_link_libraries(orbitforge_runtime PUBLIC OrbitForge::Application)
if(ORBITFORGE_WITH_ERFA)
    target_link_libraries(orbitforge_runtime PRIVATE OrbitForge::Erfa OrbitForge::Iers)
    target_compile_definitions(orbitforge_runtime PRIVATE ORBITFORGE_HAS_ERFA)
endif()
if(ORBITFORGE_WITH_SPICE)
    target_link_libraries(orbitforge_runtime PRIVATE OrbitForge::Spice)
    target_compile_definitions(orbitforge_runtime PRIVATE ORBITFORGE_HAS_SPICE)
endif()

