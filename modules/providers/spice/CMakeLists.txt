find_package(unofficial-cspice CONFIG REQUIRED)
find_package(nlohmann_json 3.12.0 EXACT CONFIG REQUIRED)
find_path(PICOSHA2_INCLUDE_DIR NAMES picosha2.h REQUIRED)
orbitforge_module(orbitforge_spice Spice src/provider.cpp)
target_link_libraries(orbitforge_spice PUBLIC OrbitForge::Core
    PRIVATE unofficial::cspice::cspice "$<BUILD_INTERFACE:nlohmann_json::nlohmann_json>")
target_include_directories(orbitforge_spice SYSTEM PRIVATE "${PICOSHA2_INCLUDE_DIR}")
find_file(CSPICE_LICENSE_FILE NAMES copyright PATHS "${unofficial-cspice_DIR}/../cspice"
    NO_DEFAULT_PATH REQUIRED)
find_file(PICOSHA2_LICENSE_FILE NAMES copyright PATHS "${PICOSHA2_INCLUDE_DIR}/../share/picosha2"
    NO_DEFAULT_PATH REQUIRED)
find_file(NLOHMANN_JSON_LICENSE_FILE NAMES copyright PATHS "${nlohmann_json_DIR}/../nlohmann-json"
    NO_DEFAULT_PATH REQUIRED)
foreach(component SDK Python)
    if(component STREQUAL "SDK")
        set(package_dir "${CMAKE_INSTALL_DATADIR}/OrbitForge")
    else()
        set(package_dir "orbitforge")
    endif()
    install(FILES "${CSPICE_LICENSE_FILE}" DESTINATION "${package_dir}/notices"
        RENAME CSPICE.txt COMPONENT ${component})
    install(FILES "${PICOSHA2_LICENSE_FILE}" DESTINATION "${package_dir}/notices"
        RENAME PicoSHA2.txt COMPONENT ${component})
    install(FILES "${NLOHMANN_JSON_LICENSE_FILE}" DESTINATION "${package_dir}/notices"
        RENAME nlohmann-json.txt COMPONENT ${component})
    install(FILES third_party/SatelliteToolbox-LICENSE.txt DESTINATION "${package_dir}/notices"
        RENAME SatelliteToolbox.txt COMPONENT ${component})
    install(DIRECTORY "${PROJECT_SOURCE_DIR}/data/samples/spice/"
        DESTINATION "${package_dir}/data/spice" COMPONENT ${component})
endforeach()
if(ORBITFORGE_BUILD_DATA_TOOLS)
    add_executable(orbitforge_spice_extract "${PROJECT_SOURCE_DIR}/tools/data/spice_extract.cpp")
    target_compile_features(orbitforge_spice_extract PRIVATE cxx_std_20)
    target_link_libraries(orbitforge_spice_extract PRIVATE unofficial::cspice::cspice)
    target_include_directories(orbitforge_spice_extract SYSTEM PRIVATE "${PICOSHA2_INCLUDE_DIR}")
    orbitforge_warnings(orbitforge_spice_extract)
    orbitforge_sanitizers(orbitforge_spice_extract)
endif()
if(BUILD_TESTING)
    add_subdirectory(tests)
endif()
