cmake_minimum_required(VERSION 3.20)

project(QtAVCoreAndroidOpenGLInstallConsumer LANGUAGES CXX)

find_package(QtAVCore CONFIG REQUIRED)

add_library(qtav_android_opengl_install_consumer SHARED consumer.cpp)
target_compile_features(
    qtav_android_opengl_install_consumer PRIVATE cxx_std_17)
target_link_libraries(
    qtav_android_opengl_install_consumer
    PRIVATE QtAV::RenderOpenGLAndroid)
