find_package(Threads REQUIRED)
find_package(FFmpeg REQUIRED COMPONENTS avformat avcodec avutil)
if(NOT DEFINED FFmpeg_avcodec_VERSION_MAJOR
   OR FFmpeg_avcodec_VERSION_MAJOR STREQUAL "")
    message(FATAL_ERROR
        "QtAVCore could not determine the libavcodec major version")
endif()
if(FFmpeg_avcodec_VERSION_MAJOR
   LESS QTAV_MINIMUM_FFMPEG_AVCODEC_MAJOR)
    message(FATAL_ERROR
        "QtAVCore requires FFmpeg 8 or newer "
        "(libavcodec major ${QTAV_MINIMUM_FFMPEG_AVCODEC_MAJOR}+)")
endif()

configure_file(
    "${CMAKE_CURRENT_SOURCE_DIR}/version.h.in"
    "${CMAKE_CURRENT_BINARY_DIR}/generated/include/qtav/version.h"
    @ONLY
)

add_library(qtav_core
    src/backend.cpp
    src/frame.cpp
    src/hardware_decoder.cpp
    src/player.cpp
)
add_library(QtAV::Core ALIAS qtav_core)

target_compile_features(qtav_core PUBLIC cxx_std_17)
target_compile_definitions(qtav_core PRIVATE QTAV_CORE_BUILDING_LIBRARY)
target_include_directories(qtav_core
    PUBLIC
        $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/include>
        $<BUILD_INTERFACE:${CMAKE_CURRENT_BINARY_DIR}/generated/include>
        $<INSTALL_INTERFACE:include>
    PRIVATE
        ${CMAKE_CURRENT_SOURCE_DIR}/src
)
target_link_libraries(qtav_core
    PRIVATE
        FFmpeg::avformat
        FFmpeg::avcodec
        FFmpeg::avutil
        Threads::Threads
)
if(ANDROID)
    # A statically linked FFmpeg build may contain its MediaCodec/JNI wrapper
    # objects. Shared QtAVCore must resolve those NDK references itself, and a
    # static installed target must propagate the logical system libraries.
    target_link_libraries(qtav_core PRIVATE android mediandk)
endif()

if(NOT BUILD_SHARED_LIBS)
    target_compile_definitions(qtav_core PUBLIC QTAV_CORE_STATIC)
endif()

set_target_properties(qtav_core PROPERTIES
    CXX_EXTENSIONS OFF
    CXX_VISIBILITY_PRESET hidden
    VISIBILITY_INLINES_HIDDEN YES
    VERSION "${PROJECT_VERSION}"
    SOVERSION "${PROJECT_VERSION_MAJOR}"
    EXPORT_NAME Core
)

if(MSVC)
    target_compile_options(qtav_core PRIVATE /W4 /permissive-)
else()
    target_compile_options(qtav_core PRIVATE -Wall -Wextra -Wpedantic)
endif()

if(QTAV_CORE_INSTALL)
    include(GNUInstallDirs)

    install(
        TARGETS qtav_core
        EXPORT QtAVCoreTargets
        RUNTIME DESTINATION "${CMAKE_INSTALL_BINDIR}"
        LIBRARY DESTINATION "${CMAKE_INSTALL_LIBDIR}"
        ARCHIVE DESTINATION "${CMAKE_INSTALL_LIBDIR}"
        INCLUDES DESTINATION "${CMAKE_INSTALL_INCLUDEDIR}"
    )
    install(
        DIRECTORY include/
        DESTINATION "${CMAKE_INSTALL_INCLUDEDIR}"
    )
    install(
        FILES
            "${CMAKE_CURRENT_BINARY_DIR}/generated/include/qtav/version.h"
        DESTINATION "${CMAKE_INSTALL_INCLUDEDIR}/qtav"
    )
endif()
