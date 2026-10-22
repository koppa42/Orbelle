orbelle_module(orbelle_sgp4 Sgp4 src/provider.cpp third_party/SGP4.cpp)
target_link_libraries(orbelle_sgp4 PUBLIC Orbelle::Core)
target_include_directories(orbelle_sgp4 SYSTEM PRIVATE third_party)
# Keep the upstream private symbols separate from another SGP4 linked by an SDK consumer.
target_compile_definitions(orbelle_sgp4 PRIVATE SGP4Funcs=OrbelleSgp4Reference
    help=orbelle_sgp4_debug_help dbgfile=orbelle_sgp4_debug_file)
# Preserve the upstream source verbatim; its legacy helpers have compiler warnings.
if(MSVC)
    set_source_files_properties(third_party/SGP4.cpp PROPERTIES COMPILE_OPTIONS "/w"
        COMPILE_DEFINITIONS "_CRT_SECURE_NO_WARNINGS")
else()
    set_source_files_properties(third_party/SGP4.cpp PROPERTIES COMPILE_OPTIONS "-w")
endif()
foreach(component SDK Python)
    if(component STREQUAL "SDK")
        set(package_dir "${CMAKE_INSTALL_DATADIR}/Orbelle")
    else()
        set(package_dir "orbelle")
    endif()
    install(FILES third_party/LICENSE DESTINATION "${package_dir}/notices"
        RENAME SGP4.txt COMPONENT ${component})
endforeach()
if(BUILD_TESTING)
    add_executable(orbelle_sgp4_tests tests/provider_test.cpp)
    target_link_libraries(orbelle_sgp4_tests PRIVATE Orbelle::Sgp4 GTest::gtest_main)
    target_compile_definitions(orbelle_sgp4_tests PRIVATE
        ORBELLE_SGP4_REFERENCE="${PROJECT_SOURCE_DIR}/tests/reference/propagators")
    orbelle_warnings(orbelle_sgp4_tests)
    orbelle_copy_runtime_dlls(orbelle_sgp4_tests)
    gtest_discover_tests(orbelle_sgp4_tests DISCOVERY_MODE PRE_TEST TEST_PREFIX "sgp4.")
endif()
