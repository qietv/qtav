#!/usr/bin/env bash
set -euo pipefail

if [[ $# -ne 1 ]]; then
  echo "usage: $0 <triplet>" >&2
  exit 2
fi

script_dir="$(cd "$(dirname "$0")" && pwd)"
project_root="$(cd "$script_dir/.." && pwd)"
triplet="$1"
vcpkg_root="$project_root/vcpkg"
install_root="${QTAV_FFMPEG_INSTALL_ROOT:-$project_root/build/$triplet/vcpkg_installed}"

if [[ ! -f "$vcpkg_root/bootstrap-vcpkg.sh" ]]; then
  echo "vcpkg submodule is missing; run: git submodule update --init ffmpeg/vcpkg" >&2
  exit 1
fi

if [[ ! -x "$vcpkg_root/vcpkg" ]]; then
  "$vcpkg_root/bootstrap-vcpkg.sh" -disableMetrics
fi

"$vcpkg_root/vcpkg" install \
  --x-manifest-root="$project_root" \
  --x-install-root="$install_root" \
  --triplet="$triplet" \
  --overlay-ports="$project_root/ports" \
  --overlay-triplets="$project_root/triplets"

cmake \
  -DINSTALL_ROOT="$install_root" \
  -DTRIPLET="$triplet" \
  -P "$project_root/cmake/verify-install.cmake"
