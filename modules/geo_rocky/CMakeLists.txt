find_package(rocky 1.1.0 EXACT CONFIG REQUIRED)
orbitforge_module(orbitforge_geo_rocky GeoRocky src/globe.cpp)
target_link_libraries(orbitforge_geo_rocky PUBLIC OrbitForge::RenderVsg PRIVATE rocky::rocky)
