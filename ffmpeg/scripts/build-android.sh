#!/usr/bin/env bash
set -euo pipefail

usage() {
  cat <<'EOF'
usage: build-android.sh [android-ndk-root]

Build the Android arm64/API 24 FFmpeg dependency package on macOS.
The NDK path may also be supplied through ANDROID_NDK_HOME or
ANDROID_NDK_ROOT. With no override, Android Studio's standard NDK r29 path is
used.
EOF
}

if [[ "${1:-}" == "-h" || "${1:-}" == "--help" ]]; then
  usage
  exit 0
fi
if [[ $# -gt 1 ]]; then
  usage >&2
  exit 2
fi

if [[ "$(uname -s)" != Darwin ]]; then
  echo "Android dependencies are supported by this entry point only on macOS." >&2
  exit 1
fi

android_ndk_version="29.0.14206865"
ndk_root="${1:-${ANDROID_NDK_HOME:-${ANDROID_NDK_ROOT:-}}}"
if [[ -z "$ndk_root" ]]; then
  sdk_root="${ANDROID_SDK_ROOT:-${ANDROID_HOME:-$HOME/Library/Android/sdk}}"
  ndk_root="$sdk_root/ndk/$android_ndk_version"
fi

if [[ ! -f "$ndk_root/build/cmake/android.toolchain.cmake" ]]; then
  echo "Android NDK r29 toolchain not found under: $ndk_root" >&2
  echo "Install NDK $android_ndk_version in Android Studio or pass its path:" >&2
  echo "  $0 /absolute/path/to/android-ndk" >&2
  exit 1
fi

ndk_root="$(cd "$ndk_root" && pwd)"
export ANDROID_NDK_HOME="$ndk_root"
export ANDROID_NDK_ROOT="$ndk_root"

script_dir="$(cd "$(dirname "$0")" && pwd)"
exec "$script_dir/install-unix.sh" arm64-android-24-static
