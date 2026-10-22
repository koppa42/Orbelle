find_package(rocky 1.1.0 EXACT CONFIG REQUIRED)
orbelle_module(orbelle_geo_rocky GeoRocky src/globe.cpp)
target_link_libraries(orbelle_geo_rocky PUBLIC Orbelle::RenderVsg PRIVATE rocky::rocky)
