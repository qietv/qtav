cmake_minimum_required(VERSION 3.20)

project(QtAVCoreOHOSRenderInstallConsumer LANGUAGES CXX)

find_package(QtAVCore 2.0 CONFIG REQUIRED)

if(NOT QtAVCore_VERSION STREQUAL "2.0.0")
    message(FATAL_ERROR
        "Unexpected QtAVCore package version: ${QtAVCore_VERSION}")
endif()

add_library(qtav_ohos_render_install_consumer SHARED consumer.cpp)
target_compile_features(
    qtav_ohos_render_install_consumer PRIVATE cxx_std_17)
target_link_libraries(
    qtav_ohos_render_install_consumer
    PRIVATE
        QtAV::AudioOHAudio
        QtAV::AudioFilter
        QtAV::AudioResample
        QtAV::HWOHCodec
        QtAV::InteropOHCodecOpenGL
        QtAV::InteropOHCodecVulkan
        QtAV::RenderMobile
        QtAV::RenderOpenGLOHOS
        QtAV::RenderVulkanOHOS
)
target_link_options(
    qtav_ohos_render_install_consumer PRIVATE -Wl,-Bsymbolic)
