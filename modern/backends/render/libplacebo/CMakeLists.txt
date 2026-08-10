find_package(PkgConfig QUIET)
if(PkgConfig_FOUND)
    pkg_check_modules(
        QTAV_LIBPLACEBO
        QUIET
        IMPORTED_TARGET
        GLOBAL
        "libplacebo>=7.351.0"
    )
    qtav_sanitize_android_pkg_config_target(PkgConfig::QTAV_LIBPLACEBO)
endif()
find_package(FFmpeg REQUIRED COMPONENTS avcodec avutil)

if(NOT TARGET PkgConfig::QTAV_LIBPLACEBO)
    return()
endif()

add_library(qtav_render_libplacebo_common STATIC
    src/libplacebo_ffmpeg_bridge.c
)
add_library(
    QtAV::RenderLibplaceboCommon
    ALIAS qtav_render_libplacebo_common)

target_include_directories(qtav_render_libplacebo_common
    PUBLIC $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/include>
)
target_link_libraries(qtav_render_libplacebo_common
    PUBLIC
        PkgConfig::QTAV_LIBPLACEBO
        FFmpeg::avcodec
        FFmpeg::avutil
)
set_target_properties(qtav_render_libplacebo_common PROPERTIES
    POSITION_INDEPENDENT_CODE ON
    C_VISIBILITY_PRESET hidden
    EXPORT_NAME RenderLibplaceboCommon
)

if(QTAV_CORE_INSTALL)
    include(GNUInstallDirs)
    install(
        TARGETS qtav_render_libplacebo_common
        EXPORT QtAVCoreTargets
        ARCHIVE DESTINATION "${CMAKE_INSTALL_LIBDIR}"
    )
endif()
