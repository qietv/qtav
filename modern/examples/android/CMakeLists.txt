cmake_minimum_required(VERSION 3.20)

project(QtAVCoreAndroidTest LANGUAGES CXX)

if(NOT ANDROID)
    message(FATAL_ERROR "The Android test application requires the Android NDK toolchain")
endif()

set(QTAV_CORE_BUILD_EXAMPLES OFF CACHE BOOL "" FORCE)
set(QTAV_CORE_BUILD_TESTS OFF CACHE BOOL "" FORCE)
set(QTAV_CORE_INSTALL OFF CACHE BOOL "" FORCE)
set(QTAV_RENDER_CPU OFF CACHE STRING "" FORCE)
set(QTAV_RENDER_OPENGL OFF CACHE STRING "" FORCE)
set(QTAV_RENDER_VULKAN ON CACHE STRING "" FORCE)
set(QTAV_RENDER_D3D11 OFF CACHE STRING "" FORCE)
set(QTAV_RENDER_METAL OFF CACHE STRING "" FORCE)
set(QTAV_AUDIO_RESAMPLE OFF CACHE STRING "" FORCE)
set(QTAV_AUDIO_FILE OFF CACHE STRING "" FORCE)
set(QTAV_AUDIO_WASAPI OFF CACHE STRING "" FORCE)
set(QTAV_AUDIO_COREAUDIO OFF CACHE STRING "" FORCE)
set(QTAV_AUDIO_ALSA OFF CACHE STRING "" FORCE)
set(QTAV_AUDIO_PULSEAUDIO OFF CACHE STRING "" FORCE)
set(QTAV_AUDIO_AAUDIO OFF CACHE STRING "" FORCE)
set(QTAV_HW_D3D11VA OFF CACHE STRING "" FORCE)
set(QTAV_HW_VIDEOTOOLBOX OFF CACHE STRING "" FORCE)
set(QTAV_HW_VAAPI OFF CACHE STRING "" FORCE)
set(QTAV_HW_MEDIACODEC OFF CACHE STRING "" FORCE)
set(QTAV_INTEROP_D3D11 OFF CACHE STRING "" FORCE)
set(QTAV_INTEROP_CVMETAL OFF CACHE STRING "" FORCE)
set(QTAV_INTEROP_VAAPI OFF CACHE STRING "" FORCE)

add_subdirectory(
    "${CMAKE_CURRENT_LIST_DIR}/../.."
    "${CMAKE_CURRENT_BINARY_DIR}/qtavcore"
)

add_library(
    qtav_android_test
    SHARED
    native_activity.cpp
    "${CMAKE_CURRENT_LIST_DIR}/../../tests/vulkan_video_renderer_test_support.cpp"
)
target_compile_features(qtav_android_test PRIVATE cxx_std_17)
target_compile_options(
    qtav_android_test
    PRIVATE
        -Wall
        -Wextra
        -Wpedantic
        -Wno-missing-field-initializers
)
target_compile_definitions(
    qtav_android_test PRIVATE VK_USE_PLATFORM_ANDROID_KHR)
target_include_directories(
    qtav_android_test
    PRIVATE
        "${CMAKE_CURRENT_LIST_DIR}/../../tests"
        "${CMAKE_CURRENT_LIST_DIR}/../../core/src"
        "${FFmpeg_avutil_INCLUDE_DIR}"
)
target_link_options(qtav_android_test PRIVATE
    "-Wl,--no-undefined"
    "-Wl,-z,max-page-size=16384"
)
target_link_libraries(qtav_android_test
    PRIVATE
        QtAV::Core
        QtAV::RenderVulkanAndroid
        android
        log
        vulkan
)
