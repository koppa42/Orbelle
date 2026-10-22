find_package(vsg 1.1.15 EXACT CONFIG REQUIRED)
orbelle_module(orbelle_render_vsg RenderVsg
    src/marker_layer.cpp src/track_layer.cpp src/sensor_layer.cpp src/compile_manager.cpp src/swapchain_capture.cpp src/labels.cpp)
target_link_libraries(orbelle_render_vsg PUBLIC Orbelle::Visualization vsg::vsg)
if(BUILD_TESTING)
    add_executable(orbelle_render_vsg_tests tests/compile_manager_test.cpp tests/picking_test.cpp)
    target_link_libraries(orbelle_render_vsg_tests PRIVATE Orbelle::RenderVsg GTest::gtest_main)
    orbelle_warnings(orbelle_render_vsg_tests)
    orbelle_copy_runtime_dlls(orbelle_render_vsg_tests)
    gtest_discover_tests(orbelle_render_vsg_tests DISCOVERY_MODE PRE_TEST
        TEST_PREFIX "render_vsg." PROPERTIES TIMEOUT 10)
endif()
