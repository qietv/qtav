cmake_minimum_required(VERSION 3.20)

project(QtAVCoreAndroidMobileInstallConsumer LANGUAGES CXX)

find_package(QtAVCore 2.0 CONFIG REQUIRED)

if(NOT QtAVCore_VERSION STREQUAL "2.0.0")
    message(FATAL_ERROR
        "Unexpected QtAVCore package version: ${QtAVCore_VERSION}")
endif()

add_library(qtav_android_opengl_install_consumer SHARED consumer.cpp)
target_compile_features(
    qtav_android_opengl_install_consumer PRIVATE cxx_std_17)
target_link_libraries(
    qtav_android_opengl_install_consumer
    PRIVATE
        QtAV::RenderMobile
        QtAV::RenderOpenGLAndroid
        QtAV::AudioAAudio
        QtAV::AudioFilter
        QtAV::AudioTimeStretch
        QtAV::HWMediaCodec
        QtAV::InteropMediaCodecOpenGL
        QtAV::InteropMediaCodecVulkan
)
