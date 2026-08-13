if(NOT CMAKE_SYSTEM_NAME MATCHES "^(OHOS|OpenHarmony)$"
   OR NOT TARGET qtav_render_vulkan
   OR NOT TARGET qtav_hw_ohcodec)
    return()
endif()

find_library(QTAV_OHOS_NATIVE_BUFFER_LIBRARY native_buffer)
find_library(QTAV_OHOS_NATIVE_IMAGE_LIBRARY native_image)
find_library(QTAV_OHOS_NATIVE_WINDOW_LIBRARY native_window)
find_library(QTAV_OHOS_VULKAN_LIBRARY vulkan)
if(NOT QTAV_OHOS_NATIVE_BUFFER_LIBRARY
   OR NOT QTAV_OHOS_NATIVE_IMAGE_LIBRARY
   OR NOT QTAV_OHOS_NATIVE_WINDOW_LIBRARY
   OR NOT QTAV_OHOS_VULKAN_LIBRARY)
    return()
endif()

add_library(qtav_interop_ohcodec_vulkan
    src/ohcodec_vulkan_interop.cpp
)
add_library(
    QtAV::InteropOHCodecVulkan
    ALIAS qtav_interop_ohcodec_vulkan
)

target_compile_features(qtav_interop_ohcodec_vulkan PUBLIC cxx_std_17)
target_compile_definitions(
    qtav_interop_ohcodec_vulkan
    PRIVATE
        QTAV_INTEROP_OHCODEC_VULKAN_BUILDING_LIBRARY
        VK_USE_PLATFORM_OHOS
)
target_include_directories(qtav_interop_ohcodec_vulkan
    PUBLIC
        $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/include>
        $<INSTALL_INTERFACE:include>
    PRIVATE
        "${PROJECT_SOURCE_DIR}/backends/interop"
)
target_link_libraries(qtav_interop_ohcodec_vulkan
    PUBLIC
        QtAV::HWOHCodec
        QtAV::RenderVulkan
    PRIVATE
        "$<BUILD_INTERFACE:${QTAV_OHOS_NATIVE_BUFFER_LIBRARY}>$<INSTALL_INTERFACE:native_buffer>"
        "$<BUILD_INTERFACE:${QTAV_OHOS_NATIVE_IMAGE_LIBRARY}>$<INSTALL_INTERFACE:native_image>"
        "$<BUILD_INTERFACE:${QTAV_OHOS_NATIVE_WINDOW_LIBRARY}>$<INSTALL_INTERFACE:native_window>"
        "$<BUILD_INTERFACE:${QTAV_OHOS_VULKAN_LIBRARY}>$<INSTALL_INTERFACE:vulkan>"
)

if(NOT BUILD_SHARED_LIBS)
    target_compile_definitions(
        qtav_interop_ohcodec_vulkan
        PUBLIC QTAV_INTEROP_OHCODEC_VULKAN_STATIC
    )
endif()

set_target_properties(qtav_interop_ohcodec_vulkan PROPERTIES
    CXX_EXTENSIONS OFF
    CXX_VISIBILITY_PRESET hidden
    VISIBILITY_INLINES_HIDDEN YES
    VERSION "${PROJECT_VERSION}"
    SOVERSION "${PROJECT_VERSION_MAJOR}"
    EXPORT_NAME InteropOHCodecVulkan
)
target_compile_options(
    qtav_interop_ohcodec_vulkan
    PRIVATE
        -Wall
        -Wextra
        -Wpedantic
        -Wno-missing-field-initializers
)

if(QTAV_CORE_INSTALL)
    include(GNUInstallDirs)
    install(
        TARGETS qtav_interop_ohcodec_vulkan
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
