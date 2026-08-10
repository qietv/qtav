cmake_minimum_required(VERSION 3.20)

project(
    QtAVCore
    VERSION 2.0.0
    DESCRIPTION "A modern, Qt-free media playback core based on FFmpeg"
    LANGUAGES C CXX
)

if(NOT WIN32
   AND NOT ANDROID
   AND NOT CMAKE_SYSTEM_NAME MATCHES "^(OHOS|OpenHarmony)$")
    message(FATAL_ERROR
        "QtAVCore supports Windows, Android, and OHOS targets only. "
        "The former macOS/iOS implementation is archived under "
        "archived_apple/ and is no longer maintained.")
endif()

# Keep project DLLs and their consumers together in multi-config Windows
# builds. This makes the shared-library tests and examples runnable without
# modifying PATH while leaving the install layout unchanged.
if(WIN32)
    set(CMAKE_RUNTIME_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}/bin")
endif()

# The supported OHOS package links the repository's PIC static FFmpeg closure
# into QtAVCore shared libraries. Bind definitions inside each shared object so
# AArch64 assembler references do not become non-PIC interposable relocations.
if(CMAKE_SYSTEM_NAME MATCHES "^(OHOS|OpenHarmony)$" AND BUILD_SHARED_LIBS)
    add_link_options(
        "$<$<STREQUAL:$<TARGET_PROPERTY:TYPE>,SHARED_LIBRARY>:LINKER:-Bsymbolic>"
    )
endif()

option(QTAV_CORE_BUILD_EXAMPLES "Build QtAVCore examples" ON)
option(QTAV_CORE_BUILD_TESTS "Build QtAVCore tests" ON)
option(QTAV_CORE_INSTALL "Generate install rules" ON)

set(QTAV_MINIMUM_FFMPEG_AVCODEC_MAJOR 62)

# The repository module understands the packaged static FFmpeg dependency
# closure. Put it ahead of any dependency-provided FindFFMPEG.cmake whose
# imported targets may contain only the primary Windows archives.
list(PREPEND CMAKE_MODULE_PATH "${CMAKE_CURRENT_SOURCE_DIR}/cmake")

include(QtAVBackend)
include(QtAVPkgConfig)

add_subdirectory(core)
add_subdirectory(platform)
add_subdirectory(backends)

if(QTAV_CORE_BUILD_EXAMPLES)
    add_subdirectory(examples)
endif()

if(QTAV_CORE_BUILD_TESTS)
    include(CTest)
    if(BUILD_TESTING)
        add_subdirectory(tests)
    endif()
endif()

if(QTAV_CORE_INSTALL)
    include(CMakePackageConfigHelpers)
    include(GNUInstallDirs)

    set(QTAV_CORE_PACKAGE_FFMPEG_COMPONENTS avformat avcodec avutil)
    set(QTAV_CORE_PACKAGE_NEEDS_VULKAN FALSE)
    set(QTAV_CORE_PACKAGE_NEEDS_LIBPLACEBO FALSE)
    set(QTAV_CORE_PACKAGE_NEEDS_LIBASS FALSE)
    if(TARGET qtav_render_cpu)
        list(APPEND QTAV_CORE_PACKAGE_FFMPEG_COMPONENTS swscale)
    endif()
    if(TARGET qtav_audio_resample)
        list(APPEND QTAV_CORE_PACKAGE_FFMPEG_COMPONENTS swresample)
    endif()
    if(TARGET qtav_audio_timestretch)
        list(APPEND QTAV_CORE_PACKAGE_FFMPEG_COMPONENTS avfilter)
    endif()
    if(TARGET qtav_render_vulkan)
        set(QTAV_CORE_PACKAGE_NEEDS_VULKAN TRUE)
    endif()
    if(TARGET qtav_render_vulkan
       OR TARGET qtav_render_opengl
       OR TARGET qtav_render_d3d11)
        set(QTAV_CORE_PACKAGE_NEEDS_LIBPLACEBO TRUE)
    endif()
    if(TARGET qtav_subtitle_libass)
        set(QTAV_CORE_PACKAGE_NEEDS_LIBASS TRUE)
    endif()

    install(
        EXPORT QtAVCoreTargets
        FILE QtAVCoreTargets.cmake
        NAMESPACE QtAV::
        DESTINATION "${CMAKE_INSTALL_LIBDIR}/cmake/QtAVCore"
    )

    configure_package_config_file(
        "${CMAKE_CURRENT_SOURCE_DIR}/cmake/QtAVCoreConfig.cmake.in"
        "${CMAKE_CURRENT_BINARY_DIR}/QtAVCoreConfig.cmake"
        INSTALL_DESTINATION "${CMAKE_INSTALL_LIBDIR}/cmake/QtAVCore"
    )
    write_basic_package_version_file(
        "${CMAKE_CURRENT_BINARY_DIR}/QtAVCoreConfigVersion.cmake"
        VERSION "${PROJECT_VERSION}"
        COMPATIBILITY SameMajorVersion
    )
    install(
        FILES
            "${CMAKE_CURRENT_BINARY_DIR}/QtAVCoreConfig.cmake"
            "${CMAKE_CURRENT_BINARY_DIR}/QtAVCoreConfigVersion.cmake"
            "${CMAKE_CURRENT_SOURCE_DIR}/cmake/FindFFmpeg.cmake"
            "${CMAKE_CURRENT_SOURCE_DIR}/cmake/QtAVPkgConfig.cmake"
        DESTINATION "${CMAKE_INSTALL_LIBDIR}/cmake/QtAVCore"
    )
endif()
