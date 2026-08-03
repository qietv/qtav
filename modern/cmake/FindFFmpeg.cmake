# SPDX-License-Identifier: LGPL-2.1-or-later

include(FindPackageHandleStandardArgs)

set(_ffmpeg_known_components avformat avcodec avutil swresample swscale)
set(_ffmpeg_requested_components ${FFmpeg_FIND_COMPONENTS})
if(NOT _ffmpeg_requested_components)
    set(_ffmpeg_requested_components ${_ffmpeg_known_components})
endif()

find_package(PkgConfig QUIET)

if("avcodec" IN_LIST _ffmpeg_requested_components)
    unset(FFmpeg_avcodec_VERSION_MAJOR)
endif()

foreach(_component IN LISTS _ffmpeg_requested_components)
    if(NOT _component IN_LIST _ffmpeg_known_components)
        message(FATAL_ERROR "Unknown FFmpeg component: ${_component}")
    endif()

    if(PkgConfig_FOUND)
        pkg_check_modules(
            PC_FFMPEG_${_component}
            QUIET
            IMPORTED_TARGET
            GLOBAL
            "lib${_component}"
        )
    endif()

    find_path(
        FFmpeg_${_component}_INCLUDE_DIR
        NAMES "lib${_component}/${_component}.h"
        HINTS ${PC_FFMPEG_${_component}_INCLUDE_DIRS}
    )
    find_library(
        FFmpeg_${_component}_LIBRARY
        NAMES ${_component}
        HINTS ${PC_FFMPEG_${_component}_LIBRARY_DIRS}
    )

    if(FFmpeg_${_component}_INCLUDE_DIR AND FFmpeg_${_component}_LIBRARY)
        set(FFmpeg_${_component}_FOUND TRUE)
        if(_component STREQUAL "avcodec")
            foreach(_version_header
                    version_major.h
                    version.h)
                set(_version_path
                    "${FFmpeg_${_component}_INCLUDE_DIR}/libavcodec/${_version_header}")
                if(EXISTS "${_version_path}")
                    file(
                        STRINGS "${_version_path}"
                        _version_lines
                        REGEX
                            "^#define LIBAVCODEC_VERSION_MAJOR[ \t]+[0-9]+"
                    )
                    if(_version_lines)
                        list(GET _version_lines 0 _version_line)
                        string(
                            REGEX REPLACE
                            ".*LIBAVCODEC_VERSION_MAJOR[ \t]+([0-9]+).*"
                            "\\1"
                            FFmpeg_avcodec_VERSION_MAJOR
                            "${_version_line}"
                        )
                        break()
                    endif()
                endif()
            endforeach()
        endif()
        if(NOT TARGET FFmpeg::${_component})
            add_library(FFmpeg::${_component} UNKNOWN IMPORTED)
            set(_ffmpeg_component_library
                "${FFmpeg_${_component}_LIBRARY}")
            file(TO_CMAKE_PATH
                "${_ffmpeg_component_library}"
                _ffmpeg_component_library_normalized)
            set(_ffmpeg_component_release_library "")
            set(_ffmpeg_component_debug_library "")

            if(_ffmpeg_component_library_normalized
               MATCHES "/debug/lib/[^/]+$")
                set(_ffmpeg_component_debug_library
                    "${_ffmpeg_component_library}")
                string(
                    REGEX REPLACE
                    "/debug/lib/([^/]+)$"
                    "/lib/\\1"
                    _ffmpeg_component_release_candidate
                    "${_ffmpeg_component_library_normalized}"
                )
                if(EXISTS "${_ffmpeg_component_release_candidate}")
                    set(_ffmpeg_component_release_library
                        "${_ffmpeg_component_release_candidate}")
                endif()
            elseif(_ffmpeg_component_library_normalized
                   MATCHES "/lib/[^/]+$")
                set(_ffmpeg_component_release_library
                    "${_ffmpeg_component_library}")
                string(
                    REGEX REPLACE
                    "/lib/([^/]+)$"
                    "/debug/lib/\\1"
                    _ffmpeg_component_debug_candidate
                    "${_ffmpeg_component_library_normalized}"
                )
                if(EXISTS "${_ffmpeg_component_debug_candidate}")
                    set(_ffmpeg_component_debug_library
                        "${_ffmpeg_component_debug_candidate}")
                endif()
            endif()

            set_target_properties(FFmpeg::${_component} PROPERTIES
                INTERFACE_INCLUDE_DIRECTORIES
                    "${FFmpeg_${_component}_INCLUDE_DIR}"
            )
            if(_ffmpeg_component_release_library
               AND _ffmpeg_component_debug_library)
                set_target_properties(FFmpeg::${_component} PROPERTIES
                    IMPORTED_CONFIGURATIONS
                        "DEBUG;RELEASE;RELWITHDEBINFO;MINSIZEREL"
                    IMPORTED_LOCATION_DEBUG
                        "${_ffmpeg_component_debug_library}"
                    IMPORTED_LOCATION_RELEASE
                        "${_ffmpeg_component_release_library}"
                    IMPORTED_LOCATION_RELWITHDEBINFO
                        "${_ffmpeg_component_release_library}"
                    IMPORTED_LOCATION_MINSIZEREL
                        "${_ffmpeg_component_release_library}"
                )
            else()
                set_target_properties(FFmpeg::${_component} PROPERTIES
                    IMPORTED_LOCATION
                        "${FFmpeg_${_component}_LIBRARY}"
                )
            endif()

            # Repository FFmpeg packages carry their third-party dependency
            # closure in pkg-config metadata. Keep the primary archive as the
            # imported location, then propagate that closure for both Unix
            # .a archives and Windows static .lib archives.
            if(TARGET PkgConfig::PC_FFMPEG_${_component})
                target_link_libraries(
                    FFmpeg::${_component}
                    INTERFACE
                        "$<LINK_ONLY:PkgConfig::PC_FFMPEG_${_component}>"
                )
            endif()
        endif()
    else()
        set(FFmpeg_${_component}_FOUND FALSE)
    endif()
endforeach()

set(_ffmpeg_version "")
if(PC_FFMPEG_avcodec_VERSION)
    set(_ffmpeg_version "${PC_FFMPEG_avcodec_VERSION}")
endif()

list(GET _ffmpeg_requested_components 0 _ffmpeg_primary_component)
set(_ffmpeg_primary_variable
    "FFmpeg_${_ffmpeg_primary_component}_INCLUDE_DIR")
find_package_handle_standard_args(
    FFmpeg
    REQUIRED_VARS ${_ffmpeg_primary_variable}
    VERSION_VAR _ffmpeg_version
    HANDLE_COMPONENTS
)

unset(_ffmpeg_known_components)
unset(_ffmpeg_requested_components)
unset(_ffmpeg_primary_component)
unset(_ffmpeg_primary_variable)
unset(_ffmpeg_version)
unset(_version_header)
unset(_version_path)
unset(_version_lines)
unset(_version_line)
unset(_ffmpeg_component_library)
unset(_ffmpeg_component_library_normalized)
unset(_ffmpeg_component_release_library)
unset(_ffmpeg_component_debug_library)
unset(_ffmpeg_component_release_candidate)
unset(_ffmpeg_component_debug_candidate)
