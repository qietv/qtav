cmake_minimum_required(VERSION 3.20)

project(QtAVCoreAndroidPlayer LANGUAGES CXX)

if(NOT ANDROID)
    message(FATAL_ERROR
        "The QtAVCore Android player requires the Android NDK toolchain")
endif()

foreach(openssl_library IN ITEMS
        QTAV_ANDROID_OPENSSL_SSL_LIBRARY
        QTAV_ANDROID_OPENSSL_CRYPTO_LIBRARY)
    if(NOT IS_ABSOLUTE "${${openssl_library}}"
            OR NOT EXISTS "${${openssl_library}}")
        message(FATAL_ERROR
            "${openssl_library} must name the cross-built Android archive")
    endif()
endforeach()

list(APPEND CMAKE_MODULE_PATH "${CMAKE_CURRENT_LIST_DIR}/../../cmake")
find_package(FFmpeg REQUIRED COMPONENTS avformat avcodec avutil)

set(QTAV_CORE_BUILD_EXAMPLES OFF CACHE BOOL "" FORCE)
set(QTAV_CORE_BUILD_TESTS OFF CACHE BOOL "" FORCE)
set(QTAV_CORE_INSTALL OFF CACHE BOOL "" FORCE)
set(QTAV_RENDER_CPU OFF CACHE STRING "" FORCE)
set(QTAV_RENDER_MOBILE OFF CACHE STRING "" FORCE)
set(QTAV_RENDER_OPENGL ON CACHE STRING "" FORCE)
set(QTAV_RENDER_VULKAN ON CACHE STRING "" FORCE)
set(QTAV_RENDER_D3D11 OFF CACHE STRING "" FORCE)
set(QTAV_AUDIO_RESAMPLE ON CACHE STRING "" FORCE)
set(QTAV_AUDIO_FILE OFF CACHE STRING "" FORCE)
set(QTAV_AUDIO_WASAPI OFF CACHE STRING "" FORCE)
set(QTAV_AUDIO_AAUDIO ON CACHE STRING "" FORCE)
set(QTAV_HW_D3D11VA OFF CACHE STRING "" FORCE)
set(QTAV_HW_MEDIACODEC ON CACHE STRING "" FORCE)
set(QTAV_INTEROP_D3D11 OFF CACHE STRING "" FORCE)
set(QTAV_INTEROP_MEDIACODEC_VULKAN ON CACHE STRING "" FORCE)
set(QTAV_INTEROP_MEDIACODEC_OPENGL ON CACHE STRING "" FORCE)

add_subdirectory(
    "${CMAKE_CURRENT_LIST_DIR}/../.."
    "${CMAKE_CURRENT_BINARY_DIR}/qtavcore"
)

add_library(
    qtav_android_player
    SHARED
    native/android_vulkan_context.cpp
    native/player_jni.cpp
)
target_compile_features(qtav_android_player PRIVATE cxx_std_17)
target_compile_definitions(
    qtav_android_player
    PRIVATE
        VK_USE_PLATFORM_ANDROID_KHR
)
target_compile_options(
    qtav_android_player
    PRIVATE
        -Wall
        -Wextra
        -Wpedantic
        -Wno-missing-field-initializers
)
target_link_options(
    qtav_android_player
    PRIVATE
        "-Wl,--no-undefined"
        "-Wl,-Bsymbolic"
        "-Wl,-z,max-page-size=16384"
)
target_link_libraries(
    qtav_android_player
    PRIVATE
        QtAV::Core
        QtAV::AudioAAudio
        QtAV::AudioResample
        QtAV::HWMediaCodec
        QtAV::InteropMediaCodecOpenGL
        QtAV::InteropMediaCodecVulkan
        QtAV::RenderOpenGLAndroid
        QtAV::RenderVulkanAndroid
        FFmpeg::avformat
        FFmpeg::avcodec
        FFmpeg::avutil
        android
        EGL
        GLESv3
        log
        mediandk
        vulkan
        "${QTAV_ANDROID_OPENSSL_SSL_LIBRARY}"
        "${QTAV_ANDROID_OPENSSL_CRYPTO_LIBRARY}"
        dl
)
