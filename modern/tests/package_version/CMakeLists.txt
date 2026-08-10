cmake_minimum_required(VERSION 3.20)

project(QtAVCorePackageVersionProbe NONE)

if(NOT DEFINED QTAV_CORE_VERSION_FILE
   OR NOT EXISTS "${QTAV_CORE_VERSION_FILE}")
    message(FATAL_ERROR
        "QTAV_CORE_VERSION_FILE must name the generated package version file")
endif()
if(NOT DEFINED QTAV_CORE_PROBE_MODE)
    message(FATAL_ERROR "QTAV_CORE_PROBE_MODE is required")
endif()

set(probe_package_directory
    "${CMAKE_CURRENT_BINARY_DIR}/probe-package")
file(REMOVE_RECURSE "${probe_package_directory}")
file(MAKE_DIRECTORY "${probe_package_directory}")
configure_file(
    "${QTAV_CORE_VERSION_FILE}"
    "${probe_package_directory}/QtAVCoreConfigVersion.cmake"
    COPYONLY
)
file(WRITE
    "${probe_package_directory}/QtAVCoreConfig.cmake"
    "set(QtAVCore_PROBE_CONFIG_LOADED TRUE)\n"
)

if(QTAV_CORE_PROBE_MODE STREQUAL "EXACT")
    find_package(
        QtAVCore 2.0.0 EXACT CONFIG REQUIRED
        PATHS "${probe_package_directory}"
        NO_DEFAULT_PATH
    )
elseif(QTAV_CORE_PROBE_MODE STREQUAL "COMPATIBLE")
    find_package(
        QtAVCore 2.0 CONFIG REQUIRED
        PATHS "${probe_package_directory}"
        NO_DEFAULT_PATH
    )
elseif(QTAV_CORE_PROBE_MODE STREQUAL "REJECT_NEWER")
    find_package(
        QtAVCore 2.1 CONFIG QUIET
        PATHS "${probe_package_directory}"
        NO_DEFAULT_PATH
    )
    if(QtAVCore_FOUND)
        message(FATAL_ERROR
            "QtAVCore 2.0.0 must reject a request for newer 2.1")
    endif()
    return()
elseif(QTAV_CORE_PROBE_MODE STREQUAL "REJECT_OTHER_MAJOR")
    find_package(
        QtAVCore 1.0 CONFIG QUIET
        PATHS "${probe_package_directory}"
        NO_DEFAULT_PATH
    )
    if(QtAVCore_FOUND)
        message(FATAL_ERROR
            "QtAVCore 2.0.0 must reject a request for major 1")
    endif()
    return()
else()
    message(FATAL_ERROR
        "Unknown QTAV_CORE_PROBE_MODE: ${QTAV_CORE_PROBE_MODE}")
endif()

if(NOT QtAVCore_PROBE_CONFIG_LOADED)
    message(FATAL_ERROR "The compatible QtAVCore config was not loaded")
endif()
if(NOT QtAVCore_VERSION STREQUAL "2.0.0")
    message(FATAL_ERROR
        "find_package reported QtAVCore_VERSION='${QtAVCore_VERSION}'")
endif()
