cmake_minimum_required(VERSION 3.20)

project(
    QtAVCore
    VERSION 2.0.0
    DESCRIPTION "A modern, Qt-free media playback core based on FFmpeg"
    LANGUAGES CXX
)

# Keep project DLLs and their consumers together in multi-config Windows
# builds. This makes the shared-library tests and examples runnable without
# modifying PATH while leaving the install layout unchanged.
if(WIN32)
    set(CMAKE_RUNTIME_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}/bin")
endif()

option(QTAV_CORE_BUILD_EXAMPLES "Build QtAVCore examples" ON)
option(QTAV_CORE_BUILD_TESTS "Build QtAVCore tests" ON)
option(QTAV_CORE_INSTALL "Generate install rules" ON)

set(QTAV_MINIMUM_FFMPEG_AVCODEC_MAJOR 62)

list(APPEND CMAKE_MODULE_PATH "${CMAKE_CURRENT_SOURCE_DIR}/cmake")

include(QtAVBackend)

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
    if(TARGET qtav_render_cpu)
        list(APPEND QTAV_CORE_PACKAGE_FFMPEG_COMPONENTS swscale)
    endif()
    if(TARGET qtav_audio_resample)
        list(APPEND QTAV_CORE_PACKAGE_FFMPEG_COMPONENTS swresample)
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
        DESTINATION "${CMAKE_INSTALL_LIBDIR}/cmake/QtAVCore"
    )
endif()
