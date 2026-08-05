find_library(QTAV_OHOS_ACE_NAPI_LIBRARY ace_napi.z)
find_library(QTAV_OHOS_HILOG_LIBRARY hilog_ndk.z)
if(NOT QTAV_OHOS_ACE_NAPI_LIBRARY OR NOT QTAV_OHOS_HILOG_LIBRARY)
    message(STATUS
        "The OHOS XComponent example is disabled because N-API or HiLog "
        "is unavailable")
    return()
endif()

add_library(qtav_ohos_xcomponent_example SHARED
    native/ohos_vulkan_context.cpp
    native/qtav_ohos_napi.cpp
)
target_compile_features(
    qtav_ohos_xcomponent_example PRIVATE cxx_std_17)
target_compile_definitions(
    qtav_ohos_xcomponent_example PRIVATE VK_USE_PLATFORM_OHOS)
target_include_directories(
    qtav_ohos_xcomponent_example PRIVATE native)
target_link_libraries(qtav_ohos_xcomponent_example
    PRIVATE
        "${QTAV_OHOS_ACE_NAPI_LIBRARY}"
        "${QTAV_OHOS_HILOG_LIBRARY}"
        ace_ndk.z
        native_window
        QtAV::Core
        QtAV::RenderMobile
        QtAV::RenderOpenGLOHOS
        QtAV::RenderVulkanOHOS
)
set_target_properties(qtav_ohos_xcomponent_example PROPERTIES
    CXX_EXTENSIONS OFF
    OUTPUT_NAME entry
)
target_compile_options(qtav_ohos_xcomponent_example
    PRIVATE
        -Wall
        -Wextra
        -Wpedantic
        -Wno-missing-field-initializers)
target_link_options(
    qtav_ohos_xcomponent_example PRIVATE -Wl,-Bsymbolic)
