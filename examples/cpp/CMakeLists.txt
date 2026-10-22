add_executable(orbelle_example circular_orbit.cpp)
target_link_libraries(orbelle_example PRIVATE Orbelle::Runtime)
orbelle_warnings(orbelle_example)

# The project example propagates a general two-body orbit (CSPICE prop2b).
if(ORBELLE_WITH_SPICE)
    add_executable(orbelle_project_example project_analysis.cpp)
    target_link_libraries(orbelle_project_example PRIVATE Orbelle::Runtime)
    orbelle_warnings(orbelle_project_example)
    orbelle_copy_runtime_dlls(orbelle_project_example)
    if(BUILD_TESTING)
        add_test(NAME example.project_api COMMAND orbelle_project_example)
    endif()
endif()

