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
android_build_directory="${repository_directory}/build/android"
ffmpeg_install_directory="${repository_directory}/ffmpeg/build/${ffmpeg_triplet}/vcpkg_installed"
ffmpeg_prefix="${ffmpeg_install_directory}/${ffmpeg_triplet}"
ffmpeg_status="${ffmpeg_install_directory}/vcpkg/status"
vcpkg_toolchain="${repository_directory}/ffmpeg/vcpkg/scripts/buildsystems/vcpkg.cmake"
pkg_config_executable=${QTAV_HOST_PKG_CONFIG:-$(command -v pkg-config || true)}
native_build_directory="${android_build_directory}/native-android${android_api}-ndk${ndk_version}-${ffmpeg_triplet}"
package_directory="${android_build_directory}/package"
asset_directory="${package_directory}/assets"
native_library_directory="${package_directory}/lib/arm64-v8a"
unsigned_apk="${android_build_directory}/qtav-core-test-unsigned.apk"
aligned_apk="${android_build_directory}/qtav-core-test-aligned.apk"
signed_apk="${android_build_directory}/qtav-core-test.apk"
debug_keystore="${android_build_directory}/debug.keystore"
jbr_directory="/Applications/Android Studio.app/Contents/jbr/Contents/Home"
dovi_sample_url=https://fate-suite.ffmpeg.org/hevc/dv84.mov
dovi_sample_sha256=aaa9289a9755eaebd9962204f24a6acf8a19ff104657a3a79b6b1fa672993721
dovi_download_directory="${android_build_directory}/downloads"
dovi_sample_source=${QTAV_ANDROID_DOVI_SAMPLE:-"${dovi_download_directory}/dv84.mov"}

if [ ! -x "${pkg_config_executable}" ]; then
    echo "missing host pkg-config executable: ${pkg_config_executable}" >&2
    exit 1
fi

for required_path in \
    "${android_toolchain}" \
    "${toolchain_directory}/bin/aarch64-linux-android${android_api}-clang" \
    "${build_tools_directory}/aapt2" \
    "${build_tools_directory}/apksigner" \
    "${build_tools_directory}/zipalign" \
    "${cmake_executable}" \
    "${ninja_executable}" \
    "${android_jar}" \
    "${jbr_directory}/bin/keytool" \
    "${vcpkg_toolchain}" \
    "${ffmpeg_status}" \
    "${ffmpeg_prefix}/include/libavcodec/avcodec.h" \
    "${ffmpeg_prefix}/lib/libavformat.a" \
    "${ffmpeg_prefix}/lib/libavcodec.a" \
    "${ffmpeg_prefix}/lib/libavutil.a" \
    "${ffmpeg_prefix}/lib/libswresample.a" \
    "${ffmpeg_prefix}/lib/libswscale.a" \
    "${ffmpeg_prefix}/lib/pkgconfig/libavformat.pc"
do
    if [ ! -e "${required_path}" ]; then
        echo "missing Android build dependency: ${required_path}" >&2
        exit 1
    fi
done

mkdir -p \
    "${native_library_directory}" \
    "${asset_directory}" \
    "${dovi_download_directory}"

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
if [ ! -f "${dovi_sample_source}" ]; then
    if [ -n "${QTAV_ANDROID_DOVI_SAMPLE:-}" ]; then
        echo "missing QTAV_ANDROID_DOVI_SAMPLE: ${dovi_sample_source}" >&2
        exit 1
    fi
    dovi_sample_partial="${dovi_sample_source}.part"
    rm -f "${dovi_sample_partial}"
    curl \
        --fail \
        --location \
        --retry 2 \
        --output "${dovi_sample_partial}" \
        "${dovi_sample_url}"
    mv "${dovi_sample_partial}" "${dovi_sample_source}"
fi
dovi_sample_actual_sha256=$(
    shasum -a 256 "${dovi_sample_source}" | awk '{ print $1 }'
)
if [ "${dovi_sample_actual_sha256}" != "${dovi_sample_sha256}" ]; then
    echo "unexpected Dolby Vision sample checksum: ${dovi_sample_actual_sha256}" >&2
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
"${host_ffmpeg}" \
    -hide_banner \
    -loglevel error \
    -f lavfi \
    -i "testsrc2=size=160x90:rate=30:duration=6" \
    -c:v libx264 \
    -preset ultrafast \
    -pix_fmt yuv420p \
    -movflags +faststart \
    -an \
    -y \
    "${asset_directory}/qtav-mediacodec-h264.mp4"
"${host_ffmpeg}" \
    -hide_banner \
    -loglevel error \
    -f lavfi \
    -i "testsrc2=size=160x90:rate=30:duration=6" \
    -c:v libx265 \
    -preset ultrafast \
    -pix_fmt yuv420p \
    -tag:v hvc1 \
    -movflags +faststart \
    -an \
    -y \
    "${asset_directory}/qtav-mediacodec-hevc.mp4"
cp \
    "${dovi_sample_source}" \
    "${asset_directory}/qtav-mediacodec-dovi.mov"

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
