if(NOT ANDROID
   OR NOT TARGET qtav_render_opengl
   OR NOT TARGET qtav_hw_mediacodec)
    return()
endif()

set(qtav_mediacodec_opengl_android_api "${CMAKE_SYSTEM_VERSION}")
if(ANDROID_PLATFORM MATCHES "^android-([0-9]+)$")
    set(qtav_mediacodec_opengl_android_api "${CMAKE_MATCH_1}")
endif()
if(qtav_mediacodec_opengl_android_api
   AND qtav_mediacodec_opengl_android_api VERSION_LESS 28)
    return()
endif()

add_library(qtav_interop_mediacodec_opengl
    src/mediacodec_opengl_interop.cpp
)
add_library(
    QtAV::InteropMediaCodecOpenGL
    ALIAS qtav_interop_mediacodec_opengl
)

target_compile_features(
    qtav_interop_mediacodec_opengl PUBLIC cxx_std_17)
target_compile_definitions(
    qtav_interop_mediacodec_opengl
    PRIVATE QTAV_INTEROP_MEDIACODEC_OPENGL_BUILDING_LIBRARY
)
target_include_directories(qtav_interop_mediacodec_opengl
    PUBLIC
        $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/include>
        $<INSTALL_INTERFACE:include>
    PRIVATE
        "${PROJECT_SOURCE_DIR}/backends/interop"
)
target_link_libraries(qtav_interop_mediacodec_opengl
    PUBLIC
        QtAV::HWMediaCodec
        QtAV::RenderOpenGL
    PRIVATE
        android
        EGL
        GLESv3
        mediandk
        nativewindow
)

if(NOT BUILD_SHARED_LIBS)
    target_compile_definitions(
        qtav_interop_mediacodec_opengl
        PUBLIC QTAV_INTEROP_MEDIACODEC_OPENGL_STATIC
    )
endif()

set_target_properties(qtav_interop_mediacodec_opengl PROPERTIES
    CXX_EXTENSIONS OFF
    CXX_VISIBILITY_PRESET hidden
    VISIBILITY_INLINES_HIDDEN YES
    VERSION "${PROJECT_VERSION}"
    SOVERSION "${PROJECT_VERSION_MAJOR}"
    EXPORT_NAME InteropMediaCodecOpenGL
)
target_compile_options(
    qtav_interop_mediacodec_opengl
    PRIVATE
        -Wall
        -Wextra
        -Wpedantic
)

if(QTAV_CORE_INSTALL)
    include(GNUInstallDirs)
    install(
        TARGETS qtav_interop_mediacodec_opengl
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
