find_package(Qt6 6.8 CONFIG REQUIRED COMPONENTS Widgets)
add_executable(orbitforge_desktop WIN32 src/main.cpp)
target_link_libraries(orbitforge_desktop PRIVATE
    OrbitForge::Runtime OrbitForge::Visualization Qt6::Widgets)
orbitforge_warnings(orbitforge_desktop)
if(ORBITFORGE_WITH_VSG)
    find_package(vsgQt 0.4.0 EXACT CONFIG REQUIRED)
    find_package(rocky 1.1.0 EXACT CONFIG REQUIRED)
    target_sources(orbitforge_desktop PRIVATE src/viewer/graphics_workbench.cpp)
    target_link_libraries(orbitforge_desktop PRIVATE OrbitForge::GeoRocky vsgQt::vsgQt)
    target_compile_definitions(orbitforge_desktop PRIVATE
        ORBITFORGE_DESKTOP_GRAPHICS
        ORBITFORGE_ROCKY_VERSION="${rocky_VERSION}"
        ORBITFORGE_VSGQT_VERSION="${vsgQt_VERSION}")
    # Copy fixed runtime resources alongside the build output, independent of source paths.
    add_custom_command(TARGET orbitforge_desktop POST_BUILD
        COMMAND ${CMAKE_COMMAND} -E copy_directory
            "${ROCKY_SHARE_DIR}/shaders" "$<TARGET_FILE_DIR:orbitforge_desktop>/resources/rocky/shaders"
        COMMAND ${CMAKE_COMMAND} -E copy_directory
            "${PROJECT_SOURCE_DIR}/data/samples/eop/iers_c04_2000_01_01_2000_01_02"
            "$<TARGET_FILE_DIR:orbitforge_desktop>/resources/eop"
        VERBATIM)
    orbitforge_copy_runtime_dlls(orbitforge_desktop)
    if(ORBITFORGE_BUILD_BENCHMARKS)
        find_package(vsgXchange 1.1.13 EXACT CONFIG REQUIRED)
        target_sources(orbitforge_desktop PRIVATE src/viewer/benchmark_scene.cpp src/viewer/benchmark_run.cpp
            src/viewer/benchmark_presentation.cpp src/viewer/benchmark_gpu.cpp
            src/viewer/benchmark_memory.cpp)
        if(WIN32)
            target_link_libraries(orbitforge_desktop PRIVATE Psapi dxgi)
        endif()
        target_link_libraries(orbitforge_desktop PRIVATE orbitforge_graphics_workload vsgXchange::vsgXchange)
        target_compile_definitions(orbitforge_desktop PRIVATE ORBITFORGE_DESKTOP_BENCHMARK)
        add_custom_command(TARGET orbitforge_desktop POST_BUILD
            COMMAND ${CMAKE_COMMAND} -E copy_directory
                "${PROJECT_SOURCE_DIR}/data/samples/fonts/roboto_mono_3_001"
                "$<TARGET_FILE_DIR:orbitforge_desktop>/resources/font"
            COMMAND ${CMAKE_COMMAND} -E copy_if_different
                "${PROJECT_SOURCE_DIR}/benchmarks/scenarios/windows_10k_two_views.json"
                "$<TARGET_FILE_DIR:orbitforge_desktop>/resources/windows_10k_two_views.json"
            VERBATIM)
    endif()
endif()
# Deployment/installer rules follow after the VSG/vsgQt integration PoC.

