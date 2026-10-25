orbelle_module(orbelle_visualization Visualization src/snapshot.cpp src/state_track.cpp src/sensor_view.cpp src/region_view.cpp)
target_link_libraries(orbelle_visualization PUBLIC Orbelle::Results)

