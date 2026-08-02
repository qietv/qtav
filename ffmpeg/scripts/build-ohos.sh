#!/usr/bin/env bash
set -euo pipefail

if [[ "$(uname -s)" != Darwin ]]; then
  echo "OHOS dependencies are supported by this entry point only on macOS." >&2
  exit 1
fi

sdk_root="${OHOS_SDK_ROOT:-}"
if [[ -z "$sdk_root" && -n "${OHOS_NDK:-}" ]]; then
  sdk_root="$(cd "$OHOS_NDK/.." && pwd)"
  export OHOS_SDK_ROOT="$sdk_root"
fi
if [[ -z "$sdk_root" || ! -f "$sdk_root/native/build/cmake/ohos.toolchain.cmake" ]]; then
  echo "Set OHOS_SDK_ROOT to an OpenHarmony SDK root containing native/." >&2
  exit 1
fi

if ! command -v patchelf >/dev/null 2>&1; then
  echo "A macOS-native patchelf is required because vcpkg models OHOS as Linux." >&2
  echo "Install it with: brew install patchelf" >&2
  exit 1
fi

script_dir="$(cd "$(dirname "$0")" && pwd)"
exec "$script_dir/install-unix.sh" arm64-ohos-12-static
