if(NOT CMAKE_SYSTEM_NAME MATCHES "^(OHOS|OpenHarmony)$"
   OR NOT TARGET qtav_render_opengl
   OR NOT TARGET qtav_hw_ohcodec)
    return()
endif()

find_library(QTAV_OHOS_NATIVE_IMAGE_LIBRARY native_image)
find_library(QTAV_OHOS_NATIVE_WINDOW_LIBRARY native_window)
find_library(QTAV_OHOS_EGL_LIBRARY EGL)
find_library(QTAV_OHOS_GLESV3_LIBRARY GLESv3)
if(NOT QTAV_OHOS_NATIVE_IMAGE_LIBRARY
   OR NOT QTAV_OHOS_NATIVE_WINDOW_LIBRARY
   OR NOT QTAV_OHOS_EGL_LIBRARY
   OR NOT QTAV_OHOS_GLESV3_LIBRARY)
    return()
endif()

add_library(qtav_interop_ohcodec_opengl
    src/ohcodec_opengl_interop.cpp
)
add_library(
    QtAV::InteropOHCodecOpenGL
    ALIAS qtav_interop_ohcodec_opengl
)

target_compile_features(qtav_interop_ohcodec_opengl PUBLIC cxx_std_17)
target_compile_definitions(
    qtav_interop_ohcodec_opengl
    PRIVATE QTAV_INTEROP_OHCODEC_OPENGL_BUILDING_LIBRARY
)
target_include_directories(qtav_interop_ohcodec_opengl
    PUBLIC
        $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/include>
        $<INSTALL_INTERFACE:include>
)
target_link_libraries(qtav_interop_ohcodec_opengl
    PUBLIC
        QtAV::HWOHCodec
        QtAV::RenderOpenGL
    PRIVATE
        ${QTAV_OHOS_NATIVE_IMAGE_LIBRARY}
        ${QTAV_OHOS_NATIVE_WINDOW_LIBRARY}
        ${QTAV_OHOS_EGL_LIBRARY}
        ${QTAV_OHOS_GLESV3_LIBRARY}
)

if(NOT BUILD_SHARED_LIBS)
    target_compile_definitions(
        qtav_interop_ohcodec_opengl
        PUBLIC QTAV_INTEROP_OHCODEC_OPENGL_STATIC
    )
endif()

set_target_properties(qtav_interop_ohcodec_opengl PROPERTIES
    CXX_EXTENSIONS OFF
    CXX_VISIBILITY_PRESET hidden
    VISIBILITY_INLINES_HIDDEN YES
    VERSION "${PROJECT_VERSION}"
    SOVERSION "${PROJECT_VERSION_MAJOR}"
    EXPORT_NAME InteropOHCodecOpenGL
)
target_compile_options(
    qtav_interop_ohcodec_opengl
    PRIVATE
        -Wall
        -Wextra
        -Wpedantic
)

if(QTAV_CORE_INSTALL)
    include(GNUInstallDirs)
    install(
        TARGETS qtav_interop_ohcodec_opengl
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
