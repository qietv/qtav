#!/bin/sh
# SPDX-License-Identifier: LGPL-2.1-or-later

set -eu

script_directory=$(CDPATH= cd -- "$(dirname -- "$0")" && pwd)
modern_directory=$(CDPATH= cd -- "${script_directory}/../.." && pwd)
repository_directory=$(CDPATH= cd -- "${modern_directory}/.." && pwd)

if [ -n "${ANDROID_SDK_ROOT:-}" ]; then
    android_sdk=${ANDROID_SDK_ROOT}
elif [ -n "${ANDROID_HOME:-}" ]; then
    android_sdk=${ANDROID_HOME}
else
    android_sdk="${HOME}/Library/Android/sdk"
fi

ndk_version=${QTAV_ANDROID_NDK_VERSION:-29.0.14206865}
android_api=${QTAV_ANDROID_API:-28}
compile_sdk=${QTAV_ANDROID_COMPILE_SDK:-36}
build_tools_version=${QTAV_ANDROID_BUILD_TOOLS:-37.0.0}
cmake_version=${QTAV_ANDROID_CMAKE_VERSION:-4.1.2}
ffmpeg_triplet=arm64-android-28-static

ndk_directory="${android_sdk}/ndk/${ndk_version}"
toolchain_directory="${ndk_directory}/toolchains/llvm/prebuilt/darwin-x86_64"
android_toolchain="${ndk_directory}/build/cmake/android.toolchain.cmake"
build_tools_directory="${android_sdk}/build-tools/${build_tools_version}"
cmake_directory="${android_sdk}/cmake/${cmake_version}"
cmake_executable="${cmake_directory}/bin/cmake"
ninja_executable="${cmake_directory}/bin/ninja"
android_jar="${android_sdk}/platforms/android-${compile_sdk}/android.jar"
jbr_directory="/Applications/Android Studio.app/Contents/jbr/Contents/Home"

android_build_directory="${repository_directory}/build/android-player"
ffmpeg_install_directory="${repository_directory}/ffmpeg/build/${ffmpeg_triplet}/vcpkg_installed"
ffmpeg_prefix="${ffmpeg_install_directory}/${ffmpeg_triplet}"
ffmpeg_status="${ffmpeg_install_directory}/vcpkg/status"
vcpkg_toolchain="${repository_directory}/ffmpeg/vcpkg/scripts/buildsystems/vcpkg.cmake"
pkg_config_executable=${QTAV_HOST_PKG_CONFIG:-$(command -v pkg-config || true)}
native_build_directory="${android_build_directory}/native-android${android_api}-ndk${ndk_version}-${ffmpeg_triplet}"
java_class_directory="${android_build_directory}/java-classes"
dex_directory="${android_build_directory}/dex"
package_directory="${android_build_directory}/package"
native_library_directory="${package_directory}/lib/arm64-v8a"
unsigned_apk="${android_build_directory}/qtav-core-player-unsigned.apk"
aligned_apk="${android_build_directory}/qtav-core-player-aligned.apk"
signed_apk="${android_build_directory}/qtav-core-player.apk"
debug_keystore="${android_build_directory}/debug.keystore"
java_archive="${android_build_directory}/qtav-core-player-java.jar"
java_source="${script_directory}/java/org/qtav/core/player/QtAVPlayerActivity.java"
third_party_notice="${script_directory}/THIRD_PARTY_NOTICES.txt"

if [ ! -x "${pkg_config_executable}" ]; then
    echo "missing host pkg-config executable: ${pkg_config_executable}" >&2
    exit 1
fi

for required_path in \
    "${ndk_directory}/build/cmake/android.toolchain.cmake" \
    "${toolchain_directory}/bin/aarch64-linux-android${android_api}-clang" \
    "${build_tools_directory}/aapt2" \
    "${build_tools_directory}/apksigner" \
    "${build_tools_directory}/d8" \
    "${build_tools_directory}/zipalign" \
    "${cmake_executable}" \
    "${ninja_executable}" \
    "${android_jar}" \
    "${jbr_directory}/bin/jar" \
    "${jbr_directory}/bin/javac" \
    "${jbr_directory}/bin/keytool" \
    "${vcpkg_toolchain}" \
    "${ffmpeg_status}" \
    "${ffmpeg_prefix}/include/libavcodec/avcodec.h" \
    "${ffmpeg_prefix}/lib/libavformat.a" \
    "${ffmpeg_prefix}/lib/libavcodec.a" \
    "${ffmpeg_prefix}/lib/libavutil.a" \
    "${ffmpeg_prefix}/lib/libswresample.a" \
    "${ffmpeg_prefix}/lib/libssl.a" \
    "${ffmpeg_prefix}/lib/libcrypto.a" \
    "${ffmpeg_prefix}/lib/pkgconfig/libavformat.pc" \
    "${ffmpeg_prefix}/share/ffmpeg/copyright" \
    "${ffmpeg_prefix}/share/libplacebo/copyright" \
    "${ffmpeg_prefix}/share/openssl/copyright" \
    "${java_source}" \
    "${third_party_notice}"
do
    if [ ! -e "${required_path}" ]; then
        echo "missing Android player build dependency: ${required_path}" >&2
        exit 1
    fi
done

mkdir -p \
    "${native_library_directory}" \
    "${java_class_directory}" \
    "${dex_directory}"

env \
    PKG_CONFIG_LIBDIR="${ffmpeg_prefix}/lib/pkgconfig" \
    PKG_CONFIG_PATH= \
    PKG_CONFIG_SYSROOT_DIR= \
    "${cmake_executable}" \
    -S "${script_directory}" \
    -B "${native_build_directory}" \
    -G Ninja \
    -DCMAKE_MAKE_PROGRAM="${ninja_executable}" \
    -DCMAKE_TOOLCHAIN_FILE="${vcpkg_toolchain}" \
    -DVCPKG_CHAINLOAD_TOOLCHAIN_FILE="${android_toolchain}" \
    -DVCPKG_TARGET_TRIPLET="${ffmpeg_triplet}" \
    -DVCPKG_INSTALLED_DIR="${ffmpeg_install_directory}" \
    -DVCPKG_MANIFEST_MODE=OFF \
    -DPKG_CONFIG_EXECUTABLE="${pkg_config_executable}" \
    -DANDROID_ABI=arm64-v8a \
    -DANDROID_PLATFORM="android-${android_api}" \
    -DANDROID_STL=c++_static \
    -DCMAKE_BUILD_TYPE=Release
if [ -n "${QTAV_BUILD_JOBS:-}" ]; then
    "${cmake_executable}" --build "${native_build_directory}" \
        --parallel "${QTAV_BUILD_JOBS}"
else
    "${cmake_executable}" --build "${native_build_directory}" --parallel
fi

rm -rf "${java_class_directory}" "${dex_directory}" "${package_directory}"
mkdir -p \
    "${java_class_directory}" \
    "${dex_directory}" \
    "${package_directory}/assets" \
    "${native_library_directory}"

"${jbr_directory}/bin/javac" \
    -encoding UTF-8 \
    -Xlint:-deprecation \
    -Xlint:-options \
    --release 8 \
    -classpath "${android_jar}" \
    -d "${java_class_directory}" \
    "${java_source}"
"${jbr_directory}/bin/jar" \
    cf "${java_archive}" \
    -C "${java_class_directory}" .
env JAVA_HOME="${jbr_directory}" \
    "${build_tools_directory}/d8" \
        --lib "${android_jar}" \
        --min-api "${android_api}" \
        --output "${dex_directory}" \
        "${java_archive}"

cp \
    "${native_build_directory}/libqtav_android_player.so" \
    "${native_library_directory}/libqtav_android_player.so"
"${toolchain_directory}/bin/llvm-strip" \
    --strip-unneeded \
    "${native_library_directory}/libqtav_android_player.so"
cp "${dex_directory}/classes.dex" "${package_directory}/classes.dex"
cp \
    "${third_party_notice}" \
    "${package_directory}/assets/THIRD_PARTY_NOTICES.txt"
cp \
    "${ffmpeg_prefix}/share/openssl/copyright" \
    "${package_directory}/assets/OpenSSL-Apache-2.0.txt"
cp \
    "${ffmpeg_prefix}/share/ffmpeg/copyright" \
    "${package_directory}/assets/FFmpeg-GPL-3.0.txt"
cp \
    "${ffmpeg_prefix}/share/libplacebo/copyright" \
    "${package_directory}/assets/libplacebo-LGPL-2.1-or-later.txt"

rm -f "${unsigned_apk}" "${aligned_apk}" "${signed_apk}"
"${build_tools_directory}/aapt2" link \
    -I "${android_jar}" \
    --manifest "${script_directory}/AndroidManifest.xml" \
    --min-sdk-version "${android_api}" \
    --target-sdk-version "${compile_sdk}" \
    --version-code 1 \
    --version-name 1.0 \
    -o "${unsigned_apk}"
(
    cd "${package_directory}"
    zip -q -r "${unsigned_apk}" assets classes.dex lib
)
"${build_tools_directory}/zipalign" \
    -f \
    -p \
    4 \
    "${unsigned_apk}" \
    "${aligned_apk}"

if [ ! -f "${debug_keystore}" ]; then
    "${jbr_directory}/bin/keytool" \
        -genkeypair \
        -keystore "${debug_keystore}" \
        -storepass android \
        -alias androiddebugkey \
        -keypass android \
        -dname "CN=Android Debug,O=Android,C=US" \
        -keyalg RSA \
        -keysize 2048 \
        -validity 10000 \
        -noprompt
fi

env JAVA_HOME="${jbr_directory}" \
    "${build_tools_directory}/apksigner" sign \
        --ks "${debug_keystore}" \
        --ks-key-alias androiddebugkey \
        --ks-pass pass:android \
        --key-pass pass:android \
        --out "${signed_apk}" \
        "${aligned_apk}"
env JAVA_HOME="${jbr_directory}" \
    "${build_tools_directory}/apksigner" verify --verbose "${signed_apk}"

echo "Android player APK: ${signed_apk}"
echo "No device installation was performed."
