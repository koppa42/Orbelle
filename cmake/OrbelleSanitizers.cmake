function(orbelle_sanitizers target)
    if(ORBELLE_ENABLE_SANITIZERS)
        if(MSVC OR NOT CMAKE_CXX_COMPILER_ID MATCHES "GNU|Clang")
            message(FATAL_ERROR "This sanitizer preset currently supports GCC/Clang on Unix only")
        endif()
        # Consumers instantiate inline/template code from our public headers too.
        # Propagate instrumentation to project tests and examples as well as linking it.
        target_compile_options(${target} PUBLIC -fsanitize=address,undefined -fno-omit-frame-pointer)
        target_link_options(${target} PUBLIC -fsanitize=address,undefined)
    endif()
endfunction()

