add_library(qtav_render_vulkan_ohos
    src/ohos_vulkan_video_renderer.cpp
)
add_library(QtAV::RenderVulkanOHOS ALIAS qtav_render_vulkan_ohos)

target_compile_features(qtav_render_vulkan_ohos PUBLIC cxx_std_17)
target_compile_definitions(
    qtav_render_vulkan_ohos
    PRIVATE
        QTAV_RENDER_VULKAN_OHOS_BUILDING_LIBRARY
        VK_USE_PLATFORM_OHOS
)
target_include_directories(qtav_render_vulkan_ohos
    PUBLIC
        $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/include>
        $<INSTALL_INTERFACE:include>
)
target_link_libraries(qtav_render_vulkan_ohos
    PUBLIC
        QtAV::RenderVulkan
    PRIVATE
        native_window
        "${QTAV_VULKAN_LIBRARY}"
)

if(NOT BUILD_SHARED_LIBS)
    target_compile_definitions(
        qtav_render_vulkan_ohos
        PUBLIC QTAV_RENDER_VULKAN_OHOS_STATIC)
endif()

set_target_properties(qtav_render_vulkan_ohos PROPERTIES
    CXX_EXTENSIONS OFF
    CXX_VISIBILITY_PRESET hidden
    VISIBILITY_INLINES_HIDDEN YES
    VERSION "${PROJECT_VERSION}"
    SOVERSION "${PROJECT_VERSION_MAJOR}"
    EXPORT_NAME RenderVulkanOHOS
)

if(MSVC)
    target_compile_options(
        qtav_render_vulkan_ohos PRIVATE /W4 /permissive-)
else()
    target_compile_options(
        qtav_render_vulkan_ohos
        PRIVATE
            -Wall
            -Wextra
            -Wpedantic
            -Wno-missing-field-initializers)
endif()

if(QTAV_CORE_INSTALL)
    include(GNUInstallDirs)
    install(
        TARGETS qtav_render_vulkan_ohos
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
