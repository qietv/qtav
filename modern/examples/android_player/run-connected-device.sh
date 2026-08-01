#!/bin/sh
# SPDX-License-Identifier: LGPL-2.1-or-later

set -eu

script_directory=$(CDPATH= cd -- "$(dirname -- "$0")" && pwd)
repository_directory=$(CDPATH= cd -- "${script_directory}/../../.." && pwd)
apk="${repository_directory}/build/android-player/qtav-core-player.apk"

if [ -n "${ANDROID_SDK_ROOT:-}" ]; then
    android_sdk=${ANDROID_SDK_ROOT}
elif [ -n "${ANDROID_HOME:-}" ]; then
    android_sdk=${ANDROID_HOME}
else
    android_sdk="${HOME}/Library/Android/sdk"
fi
adb="${android_sdk}/platform-tools/adb"

if [ ! -x "${adb}" ]; then
    echo "missing adb: ${adb}" >&2
    exit 1
fi
if [ ! -f "${apk}" ]; then
    echo "missing APK; build it first: ${apk}" >&2
    exit 1
fi
if [ "${QTAV_ANDROID_INSTALL_CONFIRMED:-0}" != "1" ]; then
    echo "PAUSED before Android installation." >&2
    echo "Unlock the device and be ready to approve its install/replace prompt." >&2
    echo "Then rerun with QTAV_ANDROID_INSTALL_CONFIRMED=1." >&2
    exit 3
fi

device_count=$(
    "${adb}" devices \
        | awk 'NR > 1 && $2 == "device" { count += 1 } END { print count + 0 }'
)
if [ "${device_count}" -ne 1 ]; then
    echo "exactly one authorized Android device is required" >&2
    "${adb}" devices -l >&2
    exit 1
fi

if ! "${adb}" install -r "${apk}"; then
    echo "Android installation failed." >&2
    echo "If the device is asking for authorization, approve it manually." >&2
    echo "This script will not retry or bypass that prompt." >&2
    exit 1
fi

"${adb}" shell am force-stop org.qtav.core.player
"${adb}" shell am start \
    -n org.qtav.core.player/.QtAVPlayerActivity
echo "QtAVCore Player launched. Use adb logcat -s QtAVCorePlayer for native diagnostics."
