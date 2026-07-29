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
        pkg_check_modules(PC_FFMPEG_${_component} QUIET "lib${_component}")
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
            set_target_properties(FFmpeg::${_component} PROPERTIES
                IMPORTED_LOCATION "${FFmpeg_${_component}_LIBRARY}"
                INTERFACE_INCLUDE_DIRECTORIES "${FFmpeg_${_component}_INCLUDE_DIR}"
            )
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
