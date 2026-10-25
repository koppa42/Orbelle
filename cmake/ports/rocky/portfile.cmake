vcpkg_from_github(
    OUT_SOURCE_PATH SOURCE_PATH
    REPO pelicanmapping/rocky
    REF 59d953669485b07c31e8207780a89a90973044bf
    SHA512 122dfb9716f4bf8148dc4e4d83aa3f80a8f435a941771d1dbd2b50cebcb3e0b1da07bf787b6582d00cde2404a0c9ca647f5726d458a697aee426e731cb202aed
    HEAD_REF main
    PATCHES optional-imgui.patch update-synchronization.patch polar-tangent-frame.patch
)

# Upstream unconditionally adds demos/tests. They are not part of the dependency SDK.
vcpkg_replace_string("${SOURCE_PATH}/src/CMakeLists.txt"
    "add_subdirectory(apps)" "# Orbelle port: do not build upstream demos")
vcpkg_replace_string("${SOURCE_PATH}/src/CMakeLists.txt"
    "add_subdirectory(tests)" "# Orbelle port: do not build upstream tests")

vcpkg_check_features(OUT_FEATURE_OPTIONS feature_options
    FEATURES gdal ROCKY_SUPPORTS_GDAL)
if(VCPKG_LIBRARY_LINKAGE STREQUAL "dynamic")
    set(rocky_shared ON)
else()
    set(rocky_shared OFF)
endif()

vcpkg_cmake_configure(
    SOURCE_PATH "${SOURCE_PATH}"
    OPTIONS
        ${feature_options}
        -DROCKY_BUILD_SHARED_LIBS=${rocky_shared}
        -DROCKY_RENDERER_VSG=ON
        -DROCKY_SUPPORTS_HTTPLIB=OFF
        -DROCKY_SUPPORTS_CURL=OFF
        -DROCKY_SUPPORTS_HTTPS=OFF
        -DROCKY_SUPPORTS_MBTILES=OFF
        -DROCKY_SUPPORTS_AZURE=OFF
        -DROCKY_SUPPORTS_BING=OFF
        -DROCKY_SUPPORTS_IMGUI=OFF
        -DROCKY_SUPPORTS_QT=OFF
        -DROCKY_INSTALL_VCPKG_DEPENDENCY_DLLS=OFF
)
vcpkg_cmake_install()
vcpkg_cmake_config_fixup(CONFIG_PATH lib/cmake/rocky)
vcpkg_copy_pdbs()
file(REMOVE_RECURSE "${CURRENT_PACKAGES_DIR}/debug/include"
    "${CURRENT_PACKAGES_DIR}/debug/share")
vcpkg_install_copyright(FILE_LIST "${SOURCE_PATH}/LICENSE.txt")
