set(VCPKG_TARGET_ARCHITECTURE arm64)
set(VCPKG_CRT_LINKAGE dynamic)
set(VCPKG_LIBRARY_LINKAGE static)
set(VCPKG_BUILD_TYPE release)
set(VCPKG_C_FLAGS "-Qunused-arguments")
set(VCPKG_CXX_FLAGS "-Qunused-arguments")

# vcpkg has no first-class OHOS platform. The qie overlay models it as Linux
# and redirects all compilation through the OpenHarmony native toolchain.
set(VCPKG_CMAKE_SYSTEM_NAME Linux)
# Keep the Linux compatibility path while allowing helpers such as Meson to
# recognize that target binaries cannot run on the macOS build host.
set(VCPKG_TARGET_IS_OHOS ON)
set(VCPKG_MAKE_BUILD_TRIPLET "--host=aarch64-unknown-linux-ohos")
set(VCPKG_CMAKE_CONFIGURE_OPTIONS
    -DOHOS_ARCH=arm64-v8a
    -DOHOS_SDK_NATIVE_PLATFORM=ohos-12
    -DOHOS_STL=c++_static
    -DCMAKE_PLATFORM_NO_VERSIONED_SONAME=ON
)
set(VCPKG_CHAINLOAD_TOOLCHAIN_FILE
    "${CMAKE_CURRENT_LIST_DIR}/toolchains/ohos-native-sdk.cmake"
)
set(VCPKG_ENV_PASSTHROUGH OHOS_SDK_ROOT OHOS_NDK)
