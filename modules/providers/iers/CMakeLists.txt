find_package(nlohmann_json 3.12.0 EXACT CONFIG REQUIRED)
find_path(PICOSHA2_INCLUDE_DIR NAMES picosha2.h REQUIRED)
orbitforge_module(orbitforge_iers Iers src/eop_snapshot.cpp)
target_link_libraries(orbitforge_iers PUBLIC OrbitForge::Core
    PRIVATE "$<BUILD_INTERFACE:nlohmann_json::nlohmann_json>")
target_include_directories(orbitforge_iers SYSTEM PRIVATE "${PICOSHA2_INCLUDE_DIR}")
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
    install(FILES "${PICOSHA2_LICENSE_FILE}" DESTINATION "${package_dir}/notices"
        RENAME PicoSHA2.txt COMPONENT ${component})
    install(FILES "${NLOHMANN_JSON_LICENSE_FILE}" DESTINATION "${package_dir}/notices"
        RENAME nlohmann-json.txt COMPONENT ${component})
    install(DIRECTORY "${PROJECT_SOURCE_DIR}/data/samples/eop/"
        DESTINATION "${package_dir}/data/eop" COMPONENT ${component})
endforeach()
if(BUILD_TESTING)
    add_subdirectory(tests)
endif()
