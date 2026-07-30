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
ndk_version=${QTAV_ANDROID_NDK_VERSION:-28.2.13676358}
android_api=${QTAV_ANDROID_API:-28}
compile_sdk=${QTAV_ANDROID_COMPILE_SDK:-36}
build_tools_version=${QTAV_ANDROID_BUILD_TOOLS:-37.0.0}
cmake_version=${QTAV_ANDROID_CMAKE_VERSION:-4.1.2}
ffmpeg_version=8.1.2
ffmpeg_sha256=464beb5e7bf0c311e68b45ae2f04e9cc2af88851abb4082231742a74d97b524c
ffmpeg_configuration=qtav-minimal-v1

ndk_directory="${android_sdk}/ndk/${ndk_version}"
toolchain_directory="${ndk_directory}/toolchains/llvm/prebuilt/darwin-x86_64"
build_tools_directory="${android_sdk}/build-tools/${build_tools_version}"
cmake_directory="${android_sdk}/cmake/${cmake_version}"
cmake_executable="${cmake_directory}/bin/cmake"
ninja_executable="${cmake_directory}/bin/ninja"
android_jar="${android_sdk}/platforms/android-${compile_sdk}/android.jar"
android_build_directory="${repository_directory}/build/android"
cache_directory="${android_build_directory}/cache"
ffmpeg_archive="${cache_directory}/ffmpeg-${ffmpeg_version}.tar.xz"
ffmpeg_source_directory="${android_build_directory}/src/ffmpeg-${ffmpeg_version}"
ffmpeg_build_directory="${android_build_directory}/ffmpeg-${ffmpeg_version}-android${android_api}-${ffmpeg_configuration}-arm64-v8a"
ffmpeg_prefix="${android_build_directory}/prefix/ffmpeg-${ffmpeg_version}-android${android_api}-${ffmpeg_configuration}/arm64-v8a"
native_build_directory="${android_build_directory}/native-android${android_api}-ndk${ndk_version}-arm64-v8a"
package_directory="${android_build_directory}/package"
asset_directory="${package_directory}/assets"
native_library_directory="${package_directory}/lib/arm64-v8a"
unsigned_apk="${android_build_directory}/qtav-core-test-unsigned.apk"
aligned_apk="${android_build_directory}/qtav-core-test-aligned.apk"
signed_apk="${android_build_directory}/qtav-core-test.apk"
debug_keystore="${android_build_directory}/debug.keystore"
jbr_directory="/Applications/Android Studio.app/Contents/jbr/Contents/Home"

for required_path in \
    "${ndk_directory}/build/cmake/android.toolchain.cmake" \
    "${toolchain_directory}/bin/aarch64-linux-android${android_api}-clang" \
    "${build_tools_directory}/aapt2" \
    "${build_tools_directory}/apksigner" \
    "${build_tools_directory}/zipalign" \
    "${cmake_executable}" \
    "${ninja_executable}" \
    "${android_jar}" \
    "${jbr_directory}/bin/keytool"
do
    if [ ! -e "${required_path}" ]; then
        echo "missing Android build dependency: ${required_path}" >&2
        exit 1
    fi
done

mkdir -p \
    "${cache_directory}" \
    "${android_build_directory}/src" \
    "${ffmpeg_build_directory}" \
    "${ffmpeg_prefix}" \
    "${native_library_directory}" \
    "${asset_directory}"

if [ ! -f "${ffmpeg_archive}" ]; then
    curl --fail --location --retry 3 \
        --output "${ffmpeg_archive}" \
        "https://ffmpeg.org/releases/ffmpeg-${ffmpeg_version}.tar.xz"
fi

actual_ffmpeg_sha256=$(shasum -a 256 "${ffmpeg_archive}" | awk '{print $1}')
if [ "${actual_ffmpeg_sha256}" != "${ffmpeg_sha256}" ]; then
    echo "FFmpeg archive checksum mismatch" >&2
    echo "expected: ${ffmpeg_sha256}" >&2
    echo "actual:   ${actual_ffmpeg_sha256}" >&2
    exit 1
fi

if [ ! -f "${ffmpeg_source_directory}/configure" ]; then
    tar -xf "${ffmpeg_archive}" -C "${android_build_directory}/src"
fi

if [ ! -f "${ffmpeg_prefix}/lib/libavcodec.a" ]; then
    (
        cd "${ffmpeg_build_directory}"
        "${ffmpeg_source_directory}/configure" \
            --prefix="${ffmpeg_prefix}" \
            --target-os=android \
            --arch=aarch64 \
            --cpu=armv8-a \
            --enable-cross-compile \
            --sysroot="${toolchain_directory}/sysroot" \
            --cc="${toolchain_directory}/bin/aarch64-linux-android${android_api}-clang" \
            --cxx="${toolchain_directory}/bin/aarch64-linux-android${android_api}-clang++" \
            --ar="${toolchain_directory}/bin/llvm-ar" \
            --nm="${toolchain_directory}/bin/llvm-nm" \
            --ranlib="${toolchain_directory}/bin/llvm-ranlib" \
            --strip="${toolchain_directory}/bin/llvm-strip" \
            --disable-autodetect \
            --disable-debug \
            --disable-doc \
            --disable-avdevice \
            --disable-avfilter \
            --disable-iconv \
            --disable-programs \
            --disable-network \
            --disable-shared \
            --enable-static \
            --enable-pic \
            --disable-everything \
            --enable-avcodec \
            --enable-avformat \
            --enable-avutil \
            --enable-swresample \
            --enable-swscale \
            --enable-decoder=mpeg4 \
            --enable-decoder=pcm_s16le \
            --enable-demuxer=avi \
            --enable-parser=mpeg4video \
            --enable-protocol=file
        make -j"${QTAV_BUILD_JOBS:-$(sysctl -n hw.ncpu)}"
        make install
    )
fi

"${cmake_executable}" \
    -S "${script_directory}" \
    -B "${native_build_directory}" \
    -G Ninja \
    -DCMAKE_MAKE_PROGRAM="${ninja_executable}" \
    -DCMAKE_TOOLCHAIN_FILE="${ndk_directory}/build/cmake/android.toolchain.cmake" \
    -DANDROID_ABI=arm64-v8a \
    -DANDROID_PLATFORM="android-${android_api}" \
    -DANDROID_STL=c++_static \
    -DCMAKE_BUILD_TYPE=Release \
    -DFFmpeg_avformat_INCLUDE_DIR="${ffmpeg_prefix}/include" \
    -DFFmpeg_avformat_LIBRARY="${ffmpeg_prefix}/lib/libavformat.a" \
    -DFFmpeg_avcodec_INCLUDE_DIR="${ffmpeg_prefix}/include" \
    -DFFmpeg_avcodec_LIBRARY="${ffmpeg_prefix}/lib/libavcodec.a" \
    -DFFmpeg_avutil_INCLUDE_DIR="${ffmpeg_prefix}/include" \
    -DFFmpeg_avutil_LIBRARY="${ffmpeg_prefix}/lib/libavutil.a"
"${cmake_executable}" --build "${native_build_directory}" --parallel

host_ffmpeg=${QTAV_HOST_FFMPEG:-}
if [ -z "${host_ffmpeg}" ]; then
    if [ -x /opt/homebrew/bin/ffmpeg ]; then
        host_ffmpeg=/opt/homebrew/bin/ffmpeg
    else
        host_ffmpeg=$(command -v ffmpeg || true)
    fi
fi
if [ ! -x "${host_ffmpeg}" ]; then
    echo "missing host FFmpeg executable: ${host_ffmpeg}" >&2
    exit 1
fi
"${host_ffmpeg}" \
    -hide_banner \
    -loglevel error \
    -f lavfi \
    -i "color=c=red:size=160x90:rate=30:duration=6,drawbox=x=80:y=0:w=80:h=90:color=blue:t=fill" \
    -f lavfi \
    -i "sine=frequency=1000:sample_rate=48000:duration=6" \
    -c:v mpeg4 \
    -q:v 2 \
    -pix_fmt yuv420p \
    -c:a pcm_s16le \
    -shortest \
    -y \
    "${asset_directory}/qtav-test.avi"

cp \
    "${native_build_directory}/libqtav_android_test.so" \
    "${native_library_directory}/libqtav_android_test.so"

rm -f "${unsigned_apk}" "${aligned_apk}" "${signed_apk}"
"${build_tools_directory}/aapt2" link \
    -I "${android_jar}" \
    --manifest "${script_directory}/AndroidManifest.xml" \
    --min-sdk-version "${android_api}" \
    --target-sdk-version "${compile_sdk}" \
    --version-code 1 \
    --version-name 1.0 \
    -A "${asset_directory}" \
    -o "${unsigned_apk}"
(
    cd "${package_directory}"
    zip -q -r "${unsigned_apk}" lib
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

echo "Android APK: ${signed_apk}"
