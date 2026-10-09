find_package(vsg 1.1.15 EXACT CONFIG REQUIRED)
orbitforge_module(orbitforge_render_vsg RenderVsg
    src/marker_layer.cpp src/compile_manager.cpp src/swapchain_capture.cpp src/labels.cpp)
target_link_libraries(orbitforge_render_vsg PUBLIC OrbitForge::Visualization vsg::vsg)
if(BUILD_TESTING)
    add_executable(orbitforge_render_vsg_tests tests/compile_manager_test.cpp tests/picking_test.cpp)
    target_link_libraries(orbitforge_render_vsg_tests PRIVATE OrbitForge::RenderVsg GTest::gtest_main)
    orbitforge_warnings(orbitforge_render_vsg_tests)
    orbitforge_copy_runtime_dlls(orbitforge_render_vsg_tests)
    gtest_discover_tests(orbitforge_render_vsg_tests DISCOVERY_MODE PRE_TEST
        TEST_PREFIX "render_vsg." PROPERTIES TIMEOUT 10)
endif()
