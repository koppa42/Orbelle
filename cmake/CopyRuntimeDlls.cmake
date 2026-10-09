foreach(orbit_dll IN LISTS ORBITFORGE_RUNTIME_DLLS)
    get_filename_component(orbit_dll_name "${orbit_dll}" NAME)
    file(COPY_FILE "${orbit_dll}" "${ORBITFORGE_RUNTIME_DESTINATION}/${orbit_dll_name}"
        ONLY_IF_DIFFERENT)
endforeach()
