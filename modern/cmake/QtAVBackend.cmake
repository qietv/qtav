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

function(_qtav_collect_directory_targets directory output_variable)
    get_property(
        directory_targets
        DIRECTORY "${directory}"
        PROPERTY BUILDSYSTEM_TARGETS
    )
    get_property(
        subdirectories
        DIRECTORY "${directory}"
        PROPERTY SUBDIRECTORIES
    )
    foreach(subdirectory IN LISTS subdirectories)
        _qtav_collect_directory_targets(
            "${subdirectory}"
            subdirectory_targets
        )
        list(APPEND directory_targets ${subdirectory_targets})
    endforeach()
    set("${output_variable}" "${directory_targets}" PARENT_SCOPE)
endfunction()

function(qtav_verify_shared_library_versions directory)
    _qtav_collect_directory_targets("${directory}" qtav_targets)
    foreach(target IN LISTS qtav_targets)
        get_target_property(target_type "${target}" TYPE)
        if(NOT target_type STREQUAL "SHARED_LIBRARY")
            continue()
        endif()

        get_target_property(target_version "${target}" VERSION)
        get_target_property(target_soversion "${target}" SOVERSION)
        if(NOT target_version STREQUAL PROJECT_VERSION
           OR NOT target_soversion STREQUAL PROJECT_VERSION_MAJOR)
            message(FATAL_ERROR
                "Shared target ${target} must use QtAVCore VERSION "
                "${PROJECT_VERSION} and SOVERSION ${PROJECT_VERSION_MAJOR}; "
                "got VERSION='${target_version}' and "
                "SOVERSION='${target_soversion}'")
        endif()
    endforeach()
endfunction()
