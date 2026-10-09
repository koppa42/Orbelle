orbitforge_module(orbitforge_application Application src/session.cpp src/scenario.cpp)
target_link_libraries(orbitforge_application PUBLIC OrbitForge::Core OrbitForge::Results)

