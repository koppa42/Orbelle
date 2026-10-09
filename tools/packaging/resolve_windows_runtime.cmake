cmake_minimum_required(VERSION 3.28)
if(POLICY CMP0207)
    cmake_policy(SET CMP0207 NEW)
endif()
foreach(required IN ITEMS EXECUTABLE PLUGINS SEARCH_DIRECTORIES DUMPBIN REPORT)
    if(NOT DEFINED ${required})
        message(FATAL_ERROR "Missing ${required}")
    endif()
endforeach()
set(CMAKE_GET_RUNTIME_DEPENDENCIES_PLATFORM "windows+pe")
set(CMAKE_GET_RUNTIME_DEPENDENCIES_TOOL "dumpbin")
set(CMAKE_GET_RUNTIME_DEPENDENCIES_COMMAND "${DUMPBIN}")
file(GET_RUNTIME_DEPENDENCIES
    EXECUTABLES "${EXECUTABLE}"
    LIBRARIES ${PLUGINS}
    DIRECTORIES ${SEARCH_DIRECTORIES}
    RESOLVED_DEPENDENCIES_VAR resolved
    UNRESOLVED_DEPENDENCIES_VAR unresolved
    CONFLICTING_DEPENDENCIES_PREFIX conflicts
    PRE_EXCLUDE_REGEXES "^[aA][pP][iI]-[mM][sS]-" "^[eE][xX][tT]-[mM][sS]-"
    POST_EXCLUDE_REGEXES ".*[/\\][Ww][Ii][Nn][Dd][Oo][Ww][Ss][/\\][Ss][Yy][Ss][Tt][Ee][Mm]32[/\\].*")
if(unresolved)
    message(FATAL_ERROR "Unresolved DLLs: ${unresolved}")
endif()
# Dependency inspection can find the same DLL in the staging directory and
# original prefix. Identical copies are harmless; different non-system builds are not.
get_filename_component(executable_directory "${EXECUTABLE}" DIRECTORY)
foreach(name IN LISTS conflicts_FILENAMES)
    set(preferred "${executable_directory}/${name}")
    if(NOT EXISTS "${preferred}")
        list(GET conflicts_${name} 0 preferred)
    endif()
    file(SHA256 "${preferred}" expected)
    foreach(candidate IN LISTS conflicts_${name})
        file(TO_CMAKE_PATH "${candidate}" normalized)
        if(normalized MATCHES ".*[/][Ww][Ii][Nn][Dd][Oo][Ww][Ss][/][Ss][Yy][Ss][Tt][Ee][Mm]32[/].*")
            continue()
        endif()
        file(SHA256 "${candidate}" actual)
        if(NOT actual STREQUAL expected)
            message(FATAL_ERROR "Conflicting runtime builds for ${name}: ${conflicts_${name}}")
        endif()
    endforeach()
    list(APPEND resolved "${preferred}")
endforeach()
list(REMOVE_DUPLICATES resolved)
list(SORT resolved)
string(REPLACE ";" "\n" lines "${resolved}")
file(WRITE "${REPORT}" "${lines}\n")
