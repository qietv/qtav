#!/bin/sh
# SPDX-License-Identifier: LGPL-2.1-or-later

set -eu

script_directory=$(CDPATH= cd -- "$(dirname -- "$0")" && pwd)
repository_directory=$(CDPATH= cd -- "${script_directory}/../../.." && pwd)
if [ -n "${ANDROID_SDK_ROOT:-}" ]; then
    android_sdk=${ANDROID_SDK_ROOT}
elif [ -n "${ANDROID_HOME:-}" ]; then
    android_sdk=${ANDROID_HOME}
else
    android_sdk="${HOME}/Library/Android/sdk"
fi
adb="${android_sdk}/platform-tools/adb"
apk="${repository_directory}/build/android/qtav-core-test.apk"
result_directory="${repository_directory}/build/android/device"
package_name=org.qtav.core.test
activity_name=android.app.NativeActivity

if [ ! -x "${adb}" ]; then
    echo "missing adb: ${adb}" >&2
    exit 1
fi
if [ ! -f "${apk}" ]; then
    echo "missing APK; run ${script_directory}/build-android.sh first" >&2
    exit 1
fi

device_count=$("${adb}" devices | awk 'NR > 1 && $2 == "device" {count++} END {print count+0}')
if [ "${device_count}" -ne 1 ]; then
    echo "expected exactly one authorized Android device; found ${device_count}" >&2
    "${adb}" devices -l >&2
    exit 1
fi

"${adb}" shell input keyevent KEYCODE_WAKEUP
"${adb}" shell wm dismiss-keyguard
sleep 1
power_state=$("${adb}" shell dumpsys power | tr -d '\r')
keyguard_state=$("${adb}" shell dumpsys window policy | tr -d '\r')
if ! printf '%s\n' "${power_state}" | rg -q "mWakefulness=Awake" \
   || printf '%s\n' "${keyguard_state}" | rg -q "showing=true"; then
    echo "The Android device must be awake and unlocked for window/HDR validation." >&2
    echo "Unlock it manually, then ask the user before retrying." >&2
    exit 2
fi

mkdir -p "${result_directory}"
{
    echo "model=$("${adb}" shell getprop ro.product.model | tr -d '\r')"
    echo "abi=$("${adb}" shell getprop ro.product.cpu.abi | tr -d '\r')"
    echo "api=$("${adb}" shell getprop ro.build.version.sdk | tr -d '\r')"
    echo "release=$("${adb}" shell getprop ro.build.version.release | tr -d '\r')"
} > "${result_directory}/device.properties"
"${adb}" shell cmd gpu vkjson > "${result_directory}/vkjson.json"

set +e
install_output=$("${adb}" install --no-streaming -r -t "${apk}" 2>&1)
install_status=$?
set -e
printf '%s\n' "${install_output}"
if [ "${install_status}" -ne 0 ]; then
    echo "APK installation/update failed." >&2
    echo "Pause here and approve any installation prompt manually on the device, then ask the user before retrying." >&2
    exit 2
fi

"${adb}" logcat -c
"${adb}" shell am force-stop "${package_name}"
"${adb}" shell am start \
    -n "${package_name}/${activity_name}" \
    >/dev/null

attempt=0
while [ "${attempt}" -lt 10 ]; do
    "${adb}" logcat -d -s QtAVCoreTest:I '*:S' \
        > "${result_directory}/logcat.txt"
    if rg -q "QTAV_ANDROID_TEST: START" "${result_directory}/logcat.txt" \
       && rg -q "QTAV_ANDROID_TEST: OFFSCREEN_PASS" \
           "${result_directory}/logcat.txt" \
       && rg -q "QTAV_ANDROID_TEST: GLES_OFFSCREEN_PASS" \
           "${result_directory}/logcat.txt" \
       && rg -q "QTAV_ANDROID_TEST: RENDERER_SELECTION previous=none selected=vulkan" \
           "${result_directory}/logcat.txt" \
       && rg -q "QTAV_ANDROID_TEST: HDR_SWAPCHAIN" \
           "${result_directory}/logcat.txt" \
       && rg -q "QTAV_ANDROID_TEST: NATIVE_HDR_FRAME" \
           "${result_directory}/logcat.txt"; then
        break
    fi
    if rg -q "QTAV_ANDROID_TEST: FAIL" "${result_directory}/logcat.txt"; then
        cat "${result_directory}/logcat.txt" >&2
        "${adb}" shell am force-stop "${package_name}"
        exit 1
    fi
    attempt=$((attempt + 1))
    sleep 1
done
if [ "${attempt}" -ge 10 ]; then
    cat "${result_directory}/logcat.txt" >&2
    "${adb}" shell am force-stop "${package_name}"
    echo "timed out waiting for the native Android test to start" >&2
    exit 1
fi

attempt=0
while [ "${attempt}" -lt 5 ]; do
    "${adb}" shell dumpsys display \
        > "${result_directory}/display-hdr.txt"
    if rg -q "mIsHdrLayerPresent=true" \
        "${result_directory}/display-hdr.txt"; then
        break
    fi
    attempt=$((attempt + 1))
    sleep 1
done
if [ "${attempt}" -ge 5 ]; then
    "${adb}" shell am force-stop "${package_name}"
    echo "Android compositor did not report an active HDR layer" >&2
    exit 1
fi
echo "QTAV_ANDROID_TEST: COMPOSITOR_HDR_PASS"

cp \
    "${result_directory}/logcat.txt" \
    "${result_directory}/startup-logcat.txt"
"${adb}" logcat -c
"${adb}" shell input keyevent KEYCODE_HOME
attempt=0
while [ "${attempt}" -lt 10 ]; do
    "${adb}" logcat -d -s QtAVCoreTest:I '*:S' \
        > "${result_directory}/logcat.txt"
    if rg -q "QTAV_ANDROID_TEST: SURFACE_REMOVED" \
        "${result_directory}/logcat.txt"; then
        break
    fi
    attempt=$((attempt + 1))
    sleep 1
done
if [ "${attempt}" -ge 10 ]; then
    cat "${result_directory}/logcat.txt" >&2
    "${adb}" shell am force-stop "${package_name}"
    echo "timed out waiting for Android surface removal" >&2
    exit 1
fi

"${adb}" shell am start \
    -n "${package_name}/${activity_name}" \
    >/dev/null

attempt=0
while [ "${attempt}" -lt 30 ]; do
    "${adb}" logcat -d -s QtAVCoreTest:I '*:S' \
        > "${result_directory}/logcat.txt"
    if rg -q "QTAV_ANDROID_TEST: MEDIACODEC_WAITING_FOR_SURFACE_RESET" \
        "${result_directory}/logcat.txt" \
       && rg -q "QTAV_ANDROID_TEST: RENDERER_SELECTION previous=vulkan selected=vulkan.*Recreated vulkan renderer" \
           "${result_directory}/logcat.txt" \
       && rg -q "QTAV_ANDROID_TEST: GLES_SDR_FALLBACK_PASS.*surface=rgba8.*color_space=srgb" \
           "${result_directory}/logcat.txt" \
       && rg -q "QTAV_ANDROID_TEST: GLES_HDR_SURFACE_PASS.*surface=rgb10_a2.*component_bits=10.*color_space=bt2020_(pq|hlg)" \
           "${result_directory}/logcat.txt" \
       && rg -q "QTAV_ANDROID_TEST: AAUDIO_PASS.*format=float.*latency_ms=.*clock_ms=" \
           "${result_directory}/logcat.txt"; then
        break
    fi
    if rg -q "QTAV_ANDROID_TEST: FAIL" "${result_directory}/logcat.txt"; then
        cat "${result_directory}/logcat.txt" >&2
        "${adb}" shell am force-stop "${package_name}"
        exit 1
    fi
    attempt=$((attempt + 1))
    sleep 1
done
if [ "${attempt}" -ge 30 ]; then
    cat "${result_directory}/logcat.txt" >&2
    "${adb}" shell am force-stop "${package_name}"
    echo "timed out waiting for the software renderer phase" >&2
    exit 1
fi

gles_hdr_attempt=0
while [ "${gles_hdr_attempt}" -lt 5 ]; do
    "${adb}" shell dumpsys display \
        > "${result_directory}/display-gles-hdr.txt"
    if rg -q "mIsHdrLayerPresent=true" \
        "${result_directory}/display-gles-hdr.txt"; then
        break
    fi
    gles_hdr_attempt=$((gles_hdr_attempt + 1))
    sleep 1
done
if [ "${gles_hdr_attempt}" -ge 5 ]; then
    cat "${result_directory}/logcat.txt" >&2
    "${adb}" shell am force-stop "${package_name}"
    echo "Android compositor did not recognize the OpenGL ES EGL surface as an HDR layer" >&2
    exit 1
fi
echo "QTAV_ANDROID_TEST: COMPOSITOR_GLES_HDR_PASS"

cp \
    "${result_directory}/logcat.txt" \
    "${result_directory}/software-logcat.txt"
"${adb}" logcat -c
"${adb}" shell input keyevent KEYCODE_HOME
attempt=0
while [ "${attempt}" -lt 10 ]; do
    "${adb}" logcat -d -s QtAVCoreTest:I '*:S' \
        > "${result_directory}/logcat.txt"
    if rg -q "QTAV_ANDROID_TEST: MEDIACODEC_PREPARE_SURFACE_REMOVED" \
        "${result_directory}/logcat.txt"; then
        break
    fi
    attempt=$((attempt + 1))
    sleep 1
done
if [ "${attempt}" -ge 10 ]; then
    cat "${result_directory}/logcat.txt" >&2
    "${adb}" shell am force-stop "${package_name}"
    echo "timed out preparing the MediaCodec surface" >&2
    exit 1
fi

"${adb}" shell am start \
    -n "${package_name}/${activity_name}" \
    >/dev/null
attempt=0
while [ "${attempt}" -lt 10 ]; do
    "${adb}" logcat -d -s QtAVCoreTest:I '*:S' \
        > "${result_directory}/logcat.txt"
    if rg -q "QTAV_ANDROID_TEST: MEDIACODEC_PHASE_READY codec=h264" \
        "${result_directory}/logcat.txt" \
       && rg -q "QTAV_ANDROID_TEST: MEDIACODEC_FIRST_OUTPUT codec=h264" \
           "${result_directory}/logcat.txt"; then
        break
    fi
    if rg -q "QTAV_ANDROID_TEST: FAIL" "${result_directory}/logcat.txt"; then
        cat "${result_directory}/logcat.txt" >&2
        "${adb}" shell am force-stop "${package_name}"
        exit 1
    fi
    attempt=$((attempt + 1))
    sleep 1
done
if [ "${attempt}" -ge 10 ]; then
    cat "${result_directory}/logcat.txt" >&2
    "${adb}" shell am force-stop "${package_name}"
    echo "timed out starting MediaCodec H.264" >&2
    exit 1
fi

"${adb}" shell input keyevent KEYCODE_HOME
attempt=0
while [ "${attempt}" -lt 10 ]; do
    "${adb}" logcat -d -s QtAVCoreTest:I '*:S' \
        > "${result_directory}/logcat.txt"
    if rg -q "QTAV_ANDROID_TEST: MEDIACODEC_SURFACE_REMOVED" \
        "${result_directory}/logcat.txt"; then
        break
    fi
    attempt=$((attempt + 1))
    sleep 1
done
if [ "${attempt}" -ge 10 ]; then
    cat "${result_directory}/logcat.txt" >&2
    "${adb}" shell am force-stop "${package_name}"
    echo "timed out removing the MediaCodec H.264 surface" >&2
    exit 1
fi

"${adb}" shell am start \
    -n "${package_name}/${activity_name}" \
    >/dev/null
attempt=0
while [ "${attempt}" -lt 45 ]; do
    "${adb}" logcat -d -s QtAVCoreTest:I '*:S' \
        > "${result_directory}/logcat.txt"
    if rg -q "QTAV_ANDROID_TEST: PASS.*aaudio=pass.*gles_fallback=pass.*gles_offscreen=pass.*gles_hdr=pass.*native_hdr=pass.*hdr_source=pass.*mediacodec=h264,hevc.*mediacodec_surface_recreations=[1-9]" \
        "${result_directory}/logcat.txt" \
       && rg -q "QTAV_ANDROID_TEST: MEDIACODEC_SEEK codec=h264" \
           "${result_directory}/logcat.txt" \
       && rg -q "QTAV_ANDROID_TEST: MEDIACODEC_SURFACE_RECREATED" \
           "${result_directory}/logcat.txt" \
       && rg -q "QTAV_ANDROID_TEST: MEDIACODEC_STALE_SURFACE_REJECTED" \
           "${result_directory}/logcat.txt" \
       && rg -q "QTAV_ANDROID_TEST: MEDIACODEC_H264_PASS" \
           "${result_directory}/logcat.txt" \
       && rg -q "QTAV_ANDROID_TEST: MEDIACODEC_STOP codec=hevc" \
           "${result_directory}/logcat.txt" \
       && rg -q "QTAV_ANDROID_TEST: MEDIACODEC_HEVC_PASS" \
           "${result_directory}/logcat.txt"; then
        shutdown_attempt=0
        while [ "${shutdown_attempt}" -lt 10 ]; do
            "${adb}" logcat -d -s QtAVCoreTest:I '*:S' \
                > "${result_directory}/logcat.txt"
            if rg -q "QTAV_ANDROID_TEST: SHUTDOWN_PASS" \
                "${result_directory}/logcat.txt"; then
                break
            fi
            shutdown_attempt=$((shutdown_attempt + 1))
            sleep 1
        done
        if [ "${shutdown_attempt}" -ge 10 ]; then
            cat "${result_directory}/logcat.txt" >&2
            "${adb}" shell am force-stop "${package_name}"
            echo "timed out waiting for clean NativeActivity shutdown" >&2
            exit 1
        fi
        cat "${result_directory}/logcat.txt"
        "${adb}" shell am force-stop "${package_name}"
        exit 0
    fi
    if rg -q "QTAV_ANDROID_TEST: FAIL" "${result_directory}/logcat.txt"; then
        cat "${result_directory}/logcat.txt" >&2
        "${adb}" shell am force-stop "${package_name}"
        exit 1
    fi
    attempt=$((attempt + 1))
    sleep 1
done

cat "${result_directory}/logcat.txt" >&2
"${adb}" shell am force-stop "${package_name}"
echo "timed out waiting for the MediaCodec Android test result" >&2
exit 1
