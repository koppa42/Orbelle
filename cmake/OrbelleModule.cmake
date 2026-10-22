function(orbelle_module target export_name)
    add_library(${target} STATIC ${ARGN})
    add_library(Orbelle::${export_name} ALIAS ${target})
    set_target_properties(${target} PROPERTIES
        EXPORT_NAME ${export_name} POSITION_INDEPENDENT_CODE ON)
    target_compile_features(${target} PUBLIC cxx_std_20)
    target_include_directories(${target} PUBLIC
        "$<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/include>"
        "$<INSTALL_INTERFACE:${CMAKE_INSTALL_INCLUDEDIR}>")
    orbelle_warnings(${target})
    orbelle_sanitizers(${target})
    if(ORBELLE_INSTALL_SDK)
        install(TARGETS ${target} EXPORT OrbelleTargets
            ARCHIVE DESTINATION "${CMAKE_INSTALL_LIBDIR}" COMPONENT SDK)
        install(DIRECTORY include/ DESTINATION "${CMAKE_INSTALL_INCLUDEDIR}" COMPONENT SDK)
    endif()
endfunction()

# Make test executables runnable with shared imported libraries on Windows.
# The script also accepts an empty list (for a static dependency build).
function(orbelle_copy_runtime_dlls target)
    if(WIN32)
        add_custom_command(TARGET ${target} POST_BUILD
            COMMAND ${CMAKE_COMMAND}
                "-DORBELLE_RUNTIME_DLLS=$<TARGET_RUNTIME_DLLS:${target}>"
                "-DORBELLE_RUNTIME_DESTINATION=$<TARGET_FILE_DIR:${target}>"
                -P "${PROJECT_SOURCE_DIR}/cmake/CopyRuntimeDlls.cmake"
            VERBATIM)
    endif()
endfunction()

