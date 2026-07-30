if(NOT ANDROID)
    return()
endif()

add_library(qtav_render_vulkan_android
    src/android_vulkan_video_renderer.cpp
)
add_library(
    QtAV::RenderVulkanAndroid ALIAS qtav_render_vulkan_android)

target_compile_features(
    qtav_render_vulkan_android PUBLIC cxx_std_17)
target_compile_definitions(
    qtav_render_vulkan_android
    PRIVATE
        QTAV_RENDER_VULKAN_ANDROID_BUILDING_LIBRARY
        VK_USE_PLATFORM_ANDROID_KHR
)
target_include_directories(qtav_render_vulkan_android
    PUBLIC
        $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/include>
        $<INSTALL_INTERFACE:include>
)
target_link_libraries(qtav_render_vulkan_android
    PUBLIC QtAV::RenderVulkan
    PRIVATE android "${QTAV_VULKAN_LIBRARY}"
)

if(NOT BUILD_SHARED_LIBS)
    target_compile_definitions(
        qtav_render_vulkan_android
        PUBLIC QTAV_RENDER_VULKAN_ANDROID_STATIC)
endif()

set_target_properties(qtav_render_vulkan_android PROPERTIES
    CXX_EXTENSIONS OFF
    CXX_VISIBILITY_PRESET hidden
    VISIBILITY_INLINES_HIDDEN YES
    VERSION "${PROJECT_VERSION}"
    SOVERSION "${PROJECT_VERSION_MAJOR}"
    EXPORT_NAME RenderVulkanAndroid
)
target_compile_options(
    qtav_render_vulkan_android
    PRIVATE
        -Wall
        -Wextra
        -Wpedantic
        -Wno-missing-field-initializers
)

if(QTAV_CORE_INSTALL)
    include(GNUInstallDirs)
    install(
        TARGETS qtav_render_vulkan_android
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
