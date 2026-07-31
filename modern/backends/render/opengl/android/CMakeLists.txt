if(NOT ANDROID)
    return()
endif()

find_library(QTAV_EGL_LIBRARY_PATH NAMES EGL)
if(NOT QTAV_EGL_LIBRARY_PATH)
    return()
endif()
set(QTAV_EGL_LIBRARY EGL)

add_library(qtav_render_opengl_android
    src/android_opengl_video_renderer.cpp
)
add_library(
    QtAV::RenderOpenGLAndroid ALIAS qtav_render_opengl_android)

target_compile_features(
    qtav_render_opengl_android PUBLIC cxx_std_17)
target_compile_definitions(
    qtav_render_opengl_android
    PRIVATE QTAV_RENDER_OPENGL_ANDROID_BUILDING_LIBRARY
)
target_include_directories(qtav_render_opengl_android
    PUBLIC
        $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/include>
        $<INSTALL_INTERFACE:include>
)
target_link_libraries(qtav_render_opengl_android
    PUBLIC QtAV::RenderOpenGL
    PRIVATE
        android
        nativewindow
        "${QTAV_EGL_LIBRARY}"
        "${QTAV_GLES3_LIBRARY}"
)

if(NOT BUILD_SHARED_LIBS)
    target_compile_definitions(
        qtav_render_opengl_android
        PUBLIC QTAV_RENDER_OPENGL_ANDROID_STATIC)
endif()

set_target_properties(qtav_render_opengl_android PROPERTIES
    CXX_EXTENSIONS OFF
    CXX_VISIBILITY_PRESET hidden
    VISIBILITY_INLINES_HIDDEN YES
    VERSION "${PROJECT_VERSION}"
    SOVERSION "${PROJECT_VERSION_MAJOR}"
    EXPORT_NAME RenderOpenGLAndroid
)
target_compile_options(
    qtav_render_opengl_android
    PRIVATE -Wall -Wextra -Wpedantic)

if(QTAV_CORE_INSTALL)
    include(GNUInstallDirs)
    install(
        TARGETS qtav_render_opengl_android
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
