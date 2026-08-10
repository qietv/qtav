cmake_minimum_required(VERSION 3.20)

project(QtAVCoreInstallConsumer LANGUAGES CXX)

find_package(QtAVCore 2.0 CONFIG REQUIRED)

if(NOT QtAVCore_VERSION STREQUAL "2.0.0"
   OR NOT QtAVCore_VERSION_MAJOR STREQUAL "2"
   OR NOT QtAVCore_VERSION_MINOR STREQUAL "0"
   OR NOT QtAVCore_VERSION_PATCH STREQUAL "0")
    message(FATAL_ERROR
        "Unexpected QtAVCore package version: ${QtAVCore_VERSION}")
endif()

add_executable(qtav_core_install_consumer consumer.cpp)
target_compile_features(
    qtav_core_install_consumer PRIVATE cxx_std_17)
target_link_libraries(qtav_core_install_consumer PRIVATE QtAV::Core)
