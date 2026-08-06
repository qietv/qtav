cmake_minimum_required(VERSION 3.20)
project(ohos_native_buffer_vulkan_probe LANGUAGES CXX)

if(NOT CMAKE_SYSTEM_NAME MATCHES "^(OHOS|OpenHarmony)$")
    message(FATAL_ERROR "This probe must be cross-compiled with an OHOS toolchain")
endif()

find_library(PROBE_NATIVE_BUFFER_LIBRARY native_buffer REQUIRED)
find_library(PROBE_VULKAN_LIBRARY vulkan REQUIRED)
find_package(PkgConfig REQUIRED)
pkg_check_modules(
    PROBE_LIBPLACEBO
    REQUIRED
    IMPORTED_TARGET
    "libplacebo>=7.351.0")

add_library(ohos_native_buffer_vulkan_probe STATIC
    native_buffer_vulkan_probe.cpp
    libplacebo_wrap_probe.cpp)
target_compile_features(
    ohos_native_buffer_vulkan_probe PUBLIC cxx_std_17)
set_target_properties(
    ohos_native_buffer_vulkan_probe PROPERTIES CXX_EXTENSIONS OFF)
target_compile_definitions(
    ohos_native_buffer_vulkan_probe PRIVATE VK_USE_PLATFORM_OHOS)
target_include_directories(
    ohos_native_buffer_vulkan_probe PUBLIC "${CMAKE_CURRENT_SOURCE_DIR}")
target_link_libraries(ohos_native_buffer_vulkan_probe
    PUBLIC
        "${PROBE_NATIVE_BUFFER_LIBRARY}"
        "${PROBE_VULKAN_LIBRARY}"
        PkgConfig::PROBE_LIBPLACEBO)
target_compile_options(ohos_native_buffer_vulkan_probe
    PRIVATE
        -Wall
        -Wextra
        -Wpedantic
        -Werror
        -Wno-missing-field-initializers
        -Wno-unused-command-line-argument)
