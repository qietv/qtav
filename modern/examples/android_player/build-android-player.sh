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
openssl_version=3.5.7
openssl_sha256=a8c0d28a529ca480f9f36cf5792e2cd21984552a3c8e4aa11a24aa31aeac98e8
ffmpeg_configuration=qtav-android-player-openssl-v6

ndk_directory="${android_sdk}/ndk/${ndk_version}"
toolchain_directory="${ndk_directory}/toolchains/llvm/prebuilt/darwin-x86_64"
build_tools_directory="${android_sdk}/build-tools/${build_tools_version}"
cmake_directory="${android_sdk}/cmake/${cmake_version}"
cmake_executable="${cmake_directory}/bin/cmake"
ninja_executable="${cmake_directory}/bin/ninja"
android_jar="${android_sdk}/platforms/android-${compile_sdk}/android.jar"
jbr_directory="/Applications/Android Studio.app/Contents/jbr/Contents/Home"

android_build_directory="${repository_directory}/build/android-player"
cache_directory="${android_build_directory}/cache"
ffmpeg_archive="${cache_directory}/ffmpeg-${ffmpeg_version}.tar.xz"
shared_ffmpeg_archive="${repository_directory}/build/android/cache/ffmpeg-${ffmpeg_version}.tar.xz"
ffmpeg_source_directory="${android_build_directory}/src/ffmpeg-${ffmpeg_version}"
openssl_archive="${cache_directory}/openssl-${openssl_version}.tar.gz"
openssl_source_directory="${android_build_directory}/src/openssl-${openssl_version}"
openssl_runtime_prefix="/qtavcore/openssl-${openssl_version}-android${android_api}-arm64-v8a"
openssl_stage_directory="${android_build_directory}/prefix/openssl-${openssl_version}-android${android_api}-stage"
openssl_prefix="${openssl_stage_directory}${openssl_runtime_prefix}"
ffmpeg_build_directory="${android_build_directory}/ffmpeg-${ffmpeg_version}-android${android_api}-${ffmpeg_configuration}-arm64-v8a"
ffmpeg_prefix="${android_build_directory}/prefix/ffmpeg-${ffmpeg_version}-android${android_api}-${ffmpeg_configuration}/arm64-v8a"
native_build_directory="${android_build_directory}/native-android${android_api}-ndk${ndk_version}-arm64-v8a"
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
    /usr/bin/perl \
    "${java_source}" \
    "${third_party_notice}"
do
    if [ ! -e "${required_path}" ]; then
        echo "missing Android player build dependency: ${required_path}" >&2
        exit 1
    fi
done

mkdir -p \
    "${cache_directory}" \
    "${android_build_directory}/src" \
    "${openssl_stage_directory}" \
    "${ffmpeg_build_directory}" \
    "${ffmpeg_prefix}" \
    "${native_library_directory}" \
    "${java_class_directory}" \
    "${dex_directory}"

if [ ! -f "${ffmpeg_archive}" ]; then
    if [ -f "${shared_ffmpeg_archive}" ]; then
        cp "${shared_ffmpeg_archive}" "${ffmpeg_archive}"
    else
        ffmpeg_partial_archive="${ffmpeg_archive}.part"
        rm -f "${ffmpeg_partial_archive}"
        if ! curl --fail --location --retry 3 \
                --output "${ffmpeg_partial_archive}" \
                "https://ffmpeg.org/releases/ffmpeg-${ffmpeg_version}.tar.xz"
        then
            rm -f "${ffmpeg_partial_archive}"
            exit 1
        fi
        mv "${ffmpeg_partial_archive}" "${ffmpeg_archive}"
    fi
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

if [ ! -f "${openssl_archive}" ]; then
    openssl_partial_archive="${openssl_archive}.part"
    rm -f "${openssl_partial_archive}"
    if ! curl --fail --location --retry 3 \
            --output "${openssl_partial_archive}" \
            "https://github.com/openssl/openssl/releases/download/openssl-${openssl_version}/openssl-${openssl_version}.tar.gz"
    then
        rm -f "${openssl_partial_archive}"
        exit 1
    fi
    mv "${openssl_partial_archive}" "${openssl_archive}"
fi

actual_openssl_sha256=$(shasum -a 256 "${openssl_archive}" | awk '{print $1}')
if [ "${actual_openssl_sha256}" != "${openssl_sha256}" ]; then
    echo "OpenSSL archive checksum mismatch" >&2
    echo "expected: ${openssl_sha256}" >&2
    echo "actual:   ${actual_openssl_sha256}" >&2
    exit 1
fi

if [ ! -f "${openssl_source_directory}/Configure" ]; then
    tar -xf "${openssl_archive}" -C "${android_build_directory}/src"
fi

if [ ! -f "${openssl_prefix}/lib/libssl.a" ] \
        || [ ! -f "${openssl_prefix}/lib/libcrypto.a" ]; then
    (
        cd "${openssl_source_directory}"
        if [ -f Makefile ]; then
            make clean
        fi
        env \
            ANDROID_NDK_ROOT="${ndk_directory}" \
            PATH="${toolchain_directory}/bin:${PATH}" \
            /usr/bin/perl ./Configure \
                android-arm64 \
                -D__ANDROID_API__="${android_api}" \
                --prefix="${openssl_runtime_prefix}" \
                --openssldir=/system/etc/security/cacerts \
                --libdir=lib \
                no-apps \
                no-docs \
                no-engine \
                no-legacy \
                no-module \
                no-shared \
                no-tests
        env \
            ANDROID_NDK_ROOT="${ndk_directory}" \
            PATH="${toolchain_directory}/bin:${PATH}" \
            make -j"${QTAV_BUILD_JOBS:-$(sysctl -n hw.ncpu)}"
        env \
            ANDROID_NDK_ROOT="${ndk_directory}" \
            PATH="${toolchain_directory}/bin:${PATH}" \
            make DESTDIR="${openssl_stage_directory}" install_sw
    )
fi

if [ ! -f "${ffmpeg_prefix}/lib/libavcodec.a" ]; then
    (
        cd "${ffmpeg_build_directory}"
        env \
            PKG_CONFIG_LIBDIR="${openssl_prefix}/lib/pkgconfig" \
            PKG_CONFIG_PATH= \
            PKG_CONFIG_SYSROOT_DIR="${openssl_stage_directory}" \
            "${ffmpeg_source_directory}/configure" \
            --prefix="${ffmpeg_prefix}" \
            --target-os=android \
            --arch=aarch64 \
            --cpu=armv8-a \
            --enable-cross-compile \
            --enable-jni \
            --sysroot="${toolchain_directory}/sysroot" \
            --cc="${toolchain_directory}/bin/aarch64-linux-android${android_api}-clang" \
            --cxx="${toolchain_directory}/bin/aarch64-linux-android${android_api}-clang++" \
            --ar="${toolchain_directory}/bin/llvm-ar" \
            --nm="${toolchain_directory}/bin/llvm-nm" \
            --ranlib="${toolchain_directory}/bin/llvm-ranlib" \
            --strip="${toolchain_directory}/bin/llvm-strip" \
            --pkg-config-flags=--static \
            --extra-cflags="-I${openssl_prefix}/include -ffile-prefix-map=${repository_directory}=. -fmacro-prefix-map=${repository_directory}=." \
            --extra-ldflags="-L${openssl_prefix}/lib" \
            --extra-libs=-ldl \
            --disable-autodetect \
            --disable-debug \
            --disable-doc \
            --disable-avdevice \
            --disable-avfilter \
            --disable-iconv \
            --disable-programs \
            --disable-shared \
            --enable-network \
            --enable-openssl \
            --enable-mediacodec \
            --enable-static \
            --enable-pic \
            --disable-everything \
            --enable-avcodec \
            --enable-avformat \
            --enable-avutil \
            --enable-swresample \
            --enable-swscale \
            --enable-bsf=h264_mp4toannexb \
            --enable-bsf=hevc_mp4toannexb \
            --enable-decoder=aac \
            --enable-decoder=aac_latm \
            --enable-decoder=ac3 \
            --enable-decoder=alac \
            --enable-decoder=av1 \
            --enable-decoder=eac3 \
            --enable-decoder=flac \
            --enable-decoder=h264 \
            --enable-decoder=h264_mediacodec \
            --enable-decoder=hevc \
            --enable-decoder=hevc_mediacodec \
            --enable-decoder=mp3 \
            --enable-decoder=mpeg4 \
            --enable-decoder=opus \
            --enable-decoder=pcm_s16le \
            --enable-decoder=pcm_s24le \
            --enable-decoder=pcm_s32le \
            --enable-decoder=truehd \
            --enable-decoder=vorbis \
            --enable-decoder=vp8 \
            --enable-decoder=vp9 \
            --enable-demuxer=aac \
            --enable-demuxer=ac3 \
            --enable-demuxer=avi \
            --enable-demuxer=flac \
            --enable-demuxer=flv \
            --enable-demuxer=h264 \
            --enable-demuxer=hevc \
            --enable-demuxer=matroska \
            --enable-demuxer=mov \
            --enable-demuxer=mp3 \
            --enable-demuxer=mpegts \
            --enable-demuxer=ogg \
            --enable-demuxer=wav \
            --enable-parser=aac \
            --enable-parser=aac_latm \
            --enable-parser=ac3 \
            --enable-parser=av1 \
            --enable-parser=h264 \
            --enable-parser=hevc \
            --enable-parser=mpegaudio \
            --enable-parser=mpeg4video \
            --enable-parser=opus \
            --enable-parser=vorbis \
            --enable-parser=vp8 \
            --enable-parser=vp9 \
            --enable-protocol=fd \
            --enable-protocol=file \
            --enable-protocol=http \
            --enable-protocol=https \
            --enable-protocol=tcp \
            --enable-protocol=tls \
            --enable-protocol=udp
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
    -DFFmpeg_avutil_LIBRARY="${ffmpeg_prefix}/lib/libavutil.a" \
    -DFFmpeg_swresample_INCLUDE_DIR="${ffmpeg_prefix}/include" \
    -DFFmpeg_swresample_LIBRARY="${ffmpeg_prefix}/lib/libswresample.a" \
    -DQTAV_ANDROID_OPENSSL_SSL_LIBRARY="${openssl_prefix}/lib/libssl.a" \
    -DQTAV_ANDROID_OPENSSL_CRYPTO_LIBRARY="${openssl_prefix}/lib/libcrypto.a"
"${cmake_executable}" --build "${native_build_directory}" --parallel

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
    "${openssl_source_directory}/LICENSE.txt" \
    "${package_directory}/assets/OpenSSL-Apache-2.0.txt"
cp \
    "${ffmpeg_source_directory}/COPYING.LGPLv2.1" \
    "${package_directory}/assets/FFmpeg-QtAVCore-LGPL-2.1.txt"

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
