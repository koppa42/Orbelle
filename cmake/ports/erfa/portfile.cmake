# ERFA's C API is linked statically into the standalone Python extension.
vcpkg_check_linkage(ONLY_STATIC_LIBRARY)
vcpkg_from_github(
    OUT_SOURCE_PATH SOURCE_PATH
    REPO liberfa/erfa
    REF v2.0.1
    SHA512 c05fc4a769172b60fe39cbd96294974577279724513cc91af9f1134b3a8f34ab201beab91fc4e0c06d89e5493f930dd8fe0a3f9adcd02c115922cd0447f8f1ce)
file(COPY "${CMAKE_CURRENT_LIST_DIR}/CMakeLists.txt" "${CMAKE_CURRENT_LIST_DIR}/erfaConfig.cmake.in"
    DESTINATION "${SOURCE_PATH}")
vcpkg_cmake_configure(SOURCE_PATH "${SOURCE_PATH}" OPTIONS -DBUILD_TESTING=OFF)
vcpkg_cmake_install()
vcpkg_cmake_config_fixup(CONFIG_PATH lib/cmake/erfa)
vcpkg_copy_pdbs()
file(REMOVE_RECURSE "${CURRENT_PACKAGES_DIR}/debug/include")
vcpkg_install_copyright(FILE_LIST "${SOURCE_PATH}/LICENSE")
