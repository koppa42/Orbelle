orbitforge_module(orbitforge_visualization Visualization src/snapshot.cpp src/state_track.cpp src/sensor_view.cpp)
target_link_libraries(orbitforge_visualization PUBLIC OrbitForge::Results)

