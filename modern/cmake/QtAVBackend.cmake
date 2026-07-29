include_guard(GLOBAL)

function(qtav_backend_option variable description)
    set("${variable}" "AUTO" CACHE STRING "${description} (AUTO, ON, or OFF)")
    set_property(CACHE "${variable}" PROPERTY STRINGS AUTO ON OFF)

    string(TOUPPER "${${variable}}" value)
    if(NOT value STREQUAL "AUTO"
       AND NOT value STREQUAL "ON"
       AND NOT value STREQUAL "OFF")
        message(FATAL_ERROR
            "${variable} must be AUTO, ON, or OFF; got '${${variable}}'")
    endif()
    set("${variable}" "${value}" CACHE STRING
        "${description} (AUTO, ON, or OFF)" FORCE)
endfunction()

function(qtav_add_optional_backend variable target relative_directory)
    qtav_backend_option("${variable}" "Build the ${target} backend")

    if("${${variable}}" STREQUAL "OFF")
        return()
    endif()

    set(source_directory
        "${CMAKE_CURRENT_SOURCE_DIR}/${relative_directory}")
    if(NOT EXISTS "${source_directory}/CMakeLists.txt")
        if("${${variable}}" STREQUAL "ON")
            message(FATAL_ERROR
                "${variable}=ON requested ${target}, but that backend is not "
                "implemented for this source tree or host platform")
        endif()
        return()
    endif()

    add_subdirectory("${relative_directory}")
    if(NOT TARGET "${target}")
        if("${${variable}}" STREQUAL "ON")
            message(FATAL_ERROR
                "${variable}=ON requested ${target}, but its required "
                "dependencies are not available")
        endif()
        message(STATUS
            "${target} is disabled because its required dependencies are "
            "not available")
    endif()
endfunction()
