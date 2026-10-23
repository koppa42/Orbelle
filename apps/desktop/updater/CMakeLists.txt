if(NOT WIN32)
    return()
endif()
enable_language(RC)
configure_file(updater.rc.in updater.rc @ONLY)

# Embed the shared catalogue at configure time without adding a Qt or Python dependency.
set(native_catalogue "${CMAKE_CURRENT_SOURCE_DIR}/../resources/text/zh_CN.json")
set_property(DIRECTORY APPEND PROPERTY CMAKE_CONFIGURE_DEPENDS "${native_catalogue}")
file(READ "${native_catalogue}" native_catalogue_json)
set(native_text_entries "")
foreach(key IN ITEMS updates.native.title updates.native.failure
                     updates.native.rollback updates.native.close_new)
    string(JSON value GET "${native_catalogue_json}" messages "${key}")
    string(REPLACE "\\" "\\\\" value "${value}")
    string(REPLACE "\"" "\\\"" value "${value}")
    string(REPLACE "\r" "\\r" value "${value}")
    string(REPLACE "\n" "\\n" value "${value}")
    string(REPLACE "\t" "\\t" value "${value}")
    string(APPEND native_text_entries "    if (key == \"${key}\")\n        return L\"${value}\";\n")
endforeach()
configure_file(native_strings.hpp.in native_strings.hpp @ONLY)

# The root launcher and versioned helper must run without a Qt installation or DLLs.
foreach(target IN ITEMS orbelle_launcher orbelle_updater)
    add_executable(${target} WIN32 native_main.cpp "${CMAKE_CURRENT_BINARY_DIR}/updater.rc")
    target_include_directories(${target} PRIVATE "${CMAKE_CURRENT_BINARY_DIR}")
    target_compile_features(${target} PRIVATE cxx_std_20)
    target_compile_definitions(${target} PRIVATE UNICODE _UNICODE WIN32_LEAN_AND_MEAN NOMINMAX)
    set_property(TARGET ${target} PROPERTY MSVC_RUNTIME_LIBRARY "MultiThreaded$<$<CONFIG:Debug>:Debug>")
    target_link_libraries(${target} PRIVATE bcrypt shell32 user32)
    if(MSVC)
        target_compile_options(${target} PRIVATE /W4 /permissive- /utf-8)
    endif()
endforeach()
set_target_properties(orbelle_launcher PROPERTIES OUTPUT_NAME Orbelle)
set_target_properties(orbelle_updater PROPERTIES OUTPUT_NAME OrbelleUpdater)
target_compile_definitions(orbelle_updater PRIVATE ORBELLE_UPDATE_HELPER)

if(BUILD_TESTING)
    add_executable(orbelle_updater_stub tests/stub.cpp)
    target_compile_features(orbelle_updater_stub PRIVATE cxx_std_20)
    target_compile_definitions(orbelle_updater_stub PRIVATE UNICODE _UNICODE WIN32_LEAN_AND_MEAN NOMINMAX)
    set_property(TARGET orbelle_updater_stub PROPERTY MSVC_RUNTIME_LIBRARY "MultiThreaded$<$<CONFIG:Debug>:Debug>")
    find_package(Python3 COMPONENTS Interpreter REQUIRED)
    add_test(NAME desktop.NativeUpdater
        COMMAND "${Python3_EXECUTABLE}" "${CMAKE_CURRENT_SOURCE_DIR}/tests/native_updater_test.py"
            --launcher "$<TARGET_FILE:orbelle_launcher>"
            --updater "$<TARGET_FILE:orbelle_updater>"
            --stub "$<TARGET_FILE:orbelle_updater_stub>")
    set_tests_properties(desktop.NativeUpdater PROPERTIES TIMEOUT 120 RESOURCE_LOCK orbelle_desktop_tests)
endif()
