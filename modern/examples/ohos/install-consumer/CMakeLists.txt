cmake_minimum_required(VERSION 3.20)

project(QtAVCoreOHOSRenderInstallConsumer LANGUAGES CXX)

find_package(QtAVCore CONFIG REQUIRED)

add_library(qtav_ohos_render_install_consumer SHARED consumer.cpp)
target_compile_features(
    qtav_ohos_render_install_consumer PRIVATE cxx_std_17)
target_link_libraries(
    qtav_ohos_render_install_consumer
    PRIVATE
        QtAV::RenderMobile
        QtAV::RenderOpenGLOHOS
        QtAV::RenderVulkanOHOS
)
target_link_options(
    qtav_ohos_render_install_consumer PRIVATE -Wl,-Bsymbolic)
