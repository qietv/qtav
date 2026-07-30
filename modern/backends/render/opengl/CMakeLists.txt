find_path(QTAV_GLES3_INCLUDE_DIR GLES3/gl3.h)
find_library(QTAV_GLES3_LIBRARY_PATH NAMES GLESv3)
if(NOT QTAV_GLES3_INCLUDE_DIR OR NOT QTAV_GLES3_LIBRARY_PATH)
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
    PRIVATE "${QTAV_GLES3_INCLUDE_DIR}"
)
target_link_libraries(qtav_render_opengl
    PUBLIC QtAV::Core
    PRIVATE "${QTAV_GLES3_LIBRARY}"
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
        PRIVATE -Wall -Wextra -Wpedantic)
endif()

if(ANDROID)
    add_subdirectory(android)
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
