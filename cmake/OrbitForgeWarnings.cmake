function(orbitforge_warnings target)
    if(MSVC)
        target_compile_options(${target} PRIVATE /W4 /permissive- /utf-8)
    else()
        target_compile_options(${target} PRIVATE -Wall -Wextra -Wpedantic)
    endif()
    set_property(TARGET ${target} PROPERTY COMPILE_WARNING_AS_ERROR
        ${ORBITFORGE_WARNINGS_AS_ERRORS})
endfunction()

