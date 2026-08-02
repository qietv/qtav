#!/usr/bin/env bash
set -euo pipefail

if [[ "$(uname -s)" != Darwin ]]; then
  echo "Android dependencies are supported by this entry point only on macOS." >&2
  exit 1
fi

if [[ -z "${ANDROID_NDK_HOME:-${ANDROID_NDK_ROOT:-}}" ]]; then
  echo "Set ANDROID_NDK_HOME (or ANDROID_NDK_ROOT) to Android NDK r29." >&2
  exit 1
fi

ndk_root="${ANDROID_NDK_HOME:-${ANDROID_NDK_ROOT}}"
if [[ ! -f "$ndk_root/build/cmake/android.toolchain.cmake" ]]; then
  echo "Android NDK toolchain not found under: $ndk_root" >&2
  exit 1
fi

script_dir="$(cd "$(dirname "$0")" && pwd)"
exec "$script_dir/install-unix.sh" arm64-android-24-static
