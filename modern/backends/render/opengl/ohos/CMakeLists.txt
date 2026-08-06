add_library(qtav_render_opengl_ohos
    src/ohos_opengl_video_renderer.cpp
)
add_library(QtAV::RenderOpenGLOHOS ALIAS qtav_render_opengl_ohos)

target_compile_features(qtav_render_opengl_ohos PUBLIC cxx_std_17)
target_compile_definitions(
    qtav_render_opengl_ohos
    PRIVATE QTAV_RENDER_OPENGL_OHOS_BUILDING_LIBRARY
)
target_include_directories(qtav_render_opengl_ohos
    PUBLIC
        $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/include>
        $<INSTALL_INTERFACE:include>
)
target_link_libraries(qtav_render_opengl_ohos
    PUBLIC QtAV::RenderOpenGL
    PRIVATE
        native_window
        EGL
        "${QTAV_GLES3_LIBRARY}"
)

if(NOT BUILD_SHARED_LIBS)
    target_compile_definitions(
        qtav_render_opengl_ohos
        PUBLIC QTAV_RENDER_OPENGL_OHOS_STATIC)
endif()

set_target_properties(qtav_render_opengl_ohos PROPERTIES
    CXX_EXTENSIONS OFF
    CXX_VISIBILITY_PRESET hidden
    VISIBILITY_INLINES_HIDDEN YES
    VERSION "${PROJECT_VERSION}"
    SOVERSION "${PROJECT_VERSION_MAJOR}"
    EXPORT_NAME RenderOpenGLOHOS
)
target_compile_options(
    qtav_render_opengl_ohos
    PRIVATE
        -Wall
        -Wextra
        -Wpedantic)

if(QTAV_CORE_INSTALL)
    include(GNUInstallDirs)
    install(
        TARGETS qtav_render_opengl_ohos
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
