if(WIN32)
    return()
endif()

find_path(QTAV_GLES3_INCLUDE_DIR GLES3/gl3.h)
find_library(QTAV_GLES3_LIBRARY_PATH NAMES GLESv3)
find_library(QTAV_OPENGL_EGL_LIBRARY_PATH NAMES EGL)
find_package(PkgConfig QUIET)
if(PkgConfig_FOUND)
    pkg_check_modules(
        QTAV_LIBPLACEBO
        QUIET
        IMPORTED_TARGET
        GLOBAL
        "libplacebo>=7.351.0"
    )
endif()
find_package(FFmpeg REQUIRED COMPONENTS avutil)

if(NOT QTAV_GLES3_INCLUDE_DIR
   OR NOT QTAV_GLES3_LIBRARY_PATH
   OR NOT QTAV_OPENGL_EGL_LIBRARY_PATH
   OR NOT TARGET PkgConfig::QTAV_LIBPLACEBO
   OR NOT TARGET qtav_render_libplacebo_common)
    return()
endif()
set(QTAV_GLES3_LIBRARY GLESv3)

add_library(qtav_render_opengl
    src/opengl_video_renderer.cpp
)
add_library(QtAV::RenderOpenGL ALIAS qtav_render_opengl)

target_compile_features(qtav_render_opengl PUBLIC cxx_std_17)
target_compile_definitions(
    qtav_render_opengl
    PRIVATE QTAV_RENDER_OPENGL_BUILDING_LIBRARY
)
target_include_directories(qtav_render_opengl
    PUBLIC
        $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/include>
        $<INSTALL_INTERFACE:include>
    PRIVATE
        "${PROJECT_SOURCE_DIR}/core/src"
        "${QTAV_GLES3_INCLUDE_DIR}"
)
target_link_libraries(qtav_render_opengl
    PUBLIC QtAV::Core
    PRIVATE
        "${QTAV_GLES3_LIBRARY}"
        "${QTAV_OPENGL_EGL_LIBRARY_PATH}"
        PkgConfig::QTAV_LIBPLACEBO
        FFmpeg::avutil
        QtAV::RenderLibplaceboCommon
)

if(NOT BUILD_SHARED_LIBS)
    target_compile_definitions(
        qtav_render_opengl PUBLIC QTAV_RENDER_OPENGL_STATIC)
endif()

set_target_properties(qtav_render_opengl PROPERTIES
    CXX_EXTENSIONS OFF
    CXX_VISIBILITY_PRESET hidden
    VISIBILITY_INLINES_HIDDEN YES
    VERSION "${PROJECT_VERSION}"
    SOVERSION "${PROJECT_VERSION_MAJOR}"
    EXPORT_NAME RenderOpenGL
)

if(MSVC)
    target_compile_options(qtav_render_opengl PRIVATE /W4 /permissive-)
else()
    target_compile_options(
        qtav_render_opengl
        PRIVATE
            -Wall
            -Wextra
            -Wpedantic
            -Wno-missing-field-initializers)
endif()

if(ANDROID)
    add_subdirectory(android)
elseif(CMAKE_SYSTEM_NAME MATCHES "^(OHOS|OpenHarmony)$")
    add_subdirectory(ohos)
endif()

if(QTAV_CORE_INSTALL)
    include(GNUInstallDirs)
    install(
        TARGETS qtav_render_opengl
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
endif()
