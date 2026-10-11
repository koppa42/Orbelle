# CI and release artifacts use only Release libraries and the VS 2022 toolset.
set(VCPKG_TARGET_ARCHITECTURE x64)
set(VCPKG_CRT_LINKAGE dynamic)
set(VCPKG_LIBRARY_LINKAGE dynamic)
set(VCPKG_PLATFORM_TOOLSET v143)
set(VCPKG_BUILD_TYPE release)
