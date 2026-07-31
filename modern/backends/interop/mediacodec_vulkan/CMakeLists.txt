if(NOT ANDROID
   OR NOT TARGET qtav_render_vulkan
   OR NOT TARGET qtav_hw_mediacodec)
    return()
endif()

set(qtav_mediacodec_vulkan_android_api "${CMAKE_SYSTEM_VERSION}")
if(ANDROID_PLATFORM MATCHES "^android-([0-9]+)$")
    set(qtav_mediacodec_vulkan_android_api "${CMAKE_MATCH_1}")
endif()
if(qtav_mediacodec_vulkan_android_api
   AND qtav_mediacodec_vulkan_android_api VERSION_LESS 26)
    return()
endif()

add_library(qtav_interop_mediacodec_vulkan
    src/mediacodec_vulkan_interop.cpp
)
add_library(
    QtAV::InteropMediaCodecVulkan
    ALIAS qtav_interop_mediacodec_vulkan
)

target_compile_features(
    qtav_interop_mediacodec_vulkan PUBLIC cxx_std_17)
target_compile_definitions(
    qtav_interop_mediacodec_vulkan
    PRIVATE
        QTAV_INTEROP_MEDIACODEC_VULKAN_BUILDING_LIBRARY
        VK_USE_PLATFORM_ANDROID_KHR
)
target_include_directories(qtav_interop_mediacodec_vulkan
    PUBLIC
        $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/include>
        $<INSTALL_INTERFACE:include>
)
target_link_libraries(qtav_interop_mediacodec_vulkan
    PUBLIC
        QtAV::HWMediaCodec
        QtAV::RenderVulkan
    PRIVATE
        android
        mediandk
        vulkan
)

if(NOT BUILD_SHARED_LIBS)
    target_compile_definitions(
        qtav_interop_mediacodec_vulkan
        PUBLIC QTAV_INTEROP_MEDIACODEC_VULKAN_STATIC
    )
endif()

set_target_properties(qtav_interop_mediacodec_vulkan PROPERTIES
    CXX_EXTENSIONS OFF
    CXX_VISIBILITY_PRESET hidden
    VISIBILITY_INLINES_HIDDEN YES
    VERSION "${PROJECT_VERSION}"
    SOVERSION "${PROJECT_VERSION_MAJOR}"
    EXPORT_NAME InteropMediaCodecVulkan
)
target_compile_options(
    qtav_interop_mediacodec_vulkan
    PRIVATE
        -Wall
        -Wextra
        -Wpedantic
        -Wno-missing-field-initializers
)

if(QTAV_CORE_INSTALL)
    include(GNUInstallDirs)
    install(
        TARGETS qtav_interop_mediacodec_vulkan
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
