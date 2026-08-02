#!/usr/bin/env bash
set -euo pipefail

usage() {
  cat <<'EOF'
usage: build-ohos.sh [ohos-sdk-root]

Build the OHOS arm64/API 23 FFmpeg dependency package on macOS. The SDK root
is the directory containing native/. It may also be supplied through
OHOS_SDK_ROOT, OHOS_NDK, or DEVECO_SDK_HOME. With no override, the standard
OpenHarmony SDK 23 path under the current user's Library is used.
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
  echo "OHOS dependencies are supported by this entry point only on macOS." >&2
  exit 1
fi

sdk_root="${1:-${OHOS_SDK_ROOT:-}}"
if [[ -z "$sdk_root" && -n "${OHOS_NDK:-}" ]]; then
  sdk_root="$(cd "$OHOS_NDK/.." && pwd)"
fi
if [[ -z "$sdk_root" && -n "${DEVECO_SDK_HOME:-}" ]]; then
  sdk_root="$DEVECO_SDK_HOME/default/openharmony"
fi
if [[ -z "$sdk_root" ]]; then
  sdk_root="$HOME/Library/OpenHarmony/Sdk/23"
fi
if [[ ! -f "$sdk_root/native/build/cmake/ohos.toolchain.cmake" ]]; then
  echo "OHOS API 23 toolchain not found under: $sdk_root" >&2
  echo "Pass the OpenHarmony SDK root that contains native/:" >&2
  echo "  $0 /absolute/path/to/openharmony-sdk" >&2
  exit 1
fi

if ! command -v patchelf >/dev/null 2>&1; then
  echo "A macOS-native patchelf is required because vcpkg models OHOS as Linux." >&2
  echo "Install it with: brew install patchelf" >&2
  exit 1
fi

sdk_root="$(cd "$sdk_root" && pwd)"
export OHOS_SDK_ROOT="$sdk_root"

script_dir="$(cd "$(dirname "$0")" && pwd)"
exec "$script_dir/install-unix.sh" arm64-ohos-23-static
