# FFmpeg dependency-build architecture

## Purpose and boundary

This subproject produces the pinned native dependency prefixes consumed by
QtAVCore. It owns dependency versions, target toolchains, vcpkg overlays,
FFmpeg feature policy, package verification, and CI artifacts. It does not
build QtAVCore itself and it does not provide a runtime plugin system.

The parent project consumes modular FFmpeg libraries and their static
third-party dependencies from one target-specific prefix. Android, OHOS, and
Windows outputs are independent and are never interchangeable.

## Build flow

```text
build-android.sh ─┐
build-ohos.sh ────┼─> install-unix.sh ─┐
build-ohos.ps1 ───┤                    │
build-windows.ps1 ┘                    │
                                      v
          vcpkg manifest + pinned registry baseline
                     + overlay ports
                     + target triplet
                     + chainloaded target toolchain
                                       │
                                       v
              target-specific installed prefix
                                       │
                                       v
                     verify-install.cmake
                                       │
                                       v
                   QtAVCore CMake consumption
```

The local scripts and GitHub Actions use the same entry points. This prevents
CI-only compiler flags or dependency selections from drifting away from local
developer builds.

## Directory responsibilities

| Path | Responsibility |
| --- | --- |
| `vcpkg.json` | Direct dependency and FFmpeg feature selection |
| `vcpkg-configuration.json` | Pinned builtin registry and overlay routing |
| `vcpkg/` | Unmodified pinned upstream vcpkg submodule |
| `ports/` | Project-specific port overlays and source patches |
| `DECISIONS.md` | Rationale, validation, and retirement criteria for compatibility patches |
| `triplets/` | Target architecture, ABI, linkage, platform, and build type |
| `triplets/toolchains/` | OHOS SDK and Windows clang-cl toolchain adaptation |
| `scripts/` | Stable local/CI entry points and vcpkg orchestration |
| `cmake/verify-install.cmake` | Installed-package contract checks |
| `build/` | Generated, untracked target build and install trees |

## Dependency graph and feature policy

The manifest selects one FFmpeg package with a deliberately small player
feature set rather than `ffmpeg[all]`:

```text
FFmpeg 8.1.2
├── OpenSSL                  HTTPS/TLS
├── libsmb2                  SMB protocol through the FFmpeg overlay
├── libass                   subtitle rendering
├── libplacebo
│   ├── Vulkan-Headers       Vulkan API definitions
│   ├── glslang              shader compilation
│   ├── OpenGL/OpenGL ES     GL/GLES/EGL rendering backend
│   ├── D3D11                Windows rendering backend
│   ├── SPIRV-Cross          Windows static shader translation closure
│   └── DOVI reshaping       built-in Dolby Vision shader processing
└── dav1d                    AV1 software decoding
```

FFmpeg's native decoders remain enabled, including native VVC/H.266 decoding.
VVenC is an encoder and is intentionally absent. The verifier rejects both
wolfSSL and VVenC if they appear in an installed prefix.

The FFmpeg policy also enables GPLv3/version3, Vulkan, LTO, and small-build
optimizations while disabling `avdevice`, IAMF, and command-line programs.
The package therefore contains modular libraries for direct player linkage,
not FFmpeg executables or one combined shared object.

## Target models

### Android

- Build host: macOS.
- Target: arm64-v8a, API 28.
- Toolchain: Android NDK r29 (`29.0.14206865`).
- vcpkg triplet: `arm64-android-28-static`.
- Dependencies: release-only static archives with the static C++ runtime.
- Default SDK discovery:
  `$ANDROID_SDK_ROOT/ndk/29.0.14206865`, falling back to Android Studio's
  standard SDK path under the user's Library.

### OHOS

- Build host: macOS or 64-bit Windows.
- Target: arm64-v8a, API 23.
- Toolchain: OpenHarmony native SDK `ohos.toolchain.cmake`.
- vcpkg triplet: `arm64-ohos-23-static`.
- Dependencies: release-only static archives; SDK system runtime linkage
  remains governed by the OHOS toolchain.
- Default SDK discovery: `$HOME/Library/OpenHarmony/Sdk/23` on macOS or the
  DevEco Studio SDK under `Program Files` on Windows.

vcpkg has no first-class OHOS platform model at the pinned baseline. The
triplet uses its Linux compatibility path for port selection, then the
chainloaded toolchain redirects every compiler and target flag to OHOS. The
`VCPKG_TARGET_IS_OHOS` marker lets overlays distinguish OHOS from a real Linux
target. Host pkg-config libraries must never enter this build. A native macOS
`patchelf` is used only for macOS packaging operations; vcpkg acquires its
Windows `patchelf.exe` tool when the PowerShell entry point requires it. The
Windows script uses a stable no-space SDK junction because MSYS-driven OpenSSL
and FFmpeg steps cannot reliably consume the normal `Program Files` path.

### Windows

- Build host and target: 64-bit Windows x64.
- ABI and SDK: Visual Studio/MSVC ABI and Windows SDK.
- Compiler/linker: Visual Studio's `clang-cl` and `lld-link`.
- vcpkg triplet: `x64-windows-static-md`.
- Dependencies: release-only static libraries using the dynamic MSVC runtime
  (`/MD`).

libplacebo cannot use the MSVC `cl.exe` C compiler. The chainloaded toolchain
therefore discovers `clang-cl` inside Visual Studio even when a self-hosted
runner service account does not have it on `PATH`. FFmpeg emits LLVM LTO
objects through clang-cl and links them directly with lld-link; a narrow patch
allows exactly this otherwise compatible driver-type pair. LTO and external
NASM remain enabled, with one narrow MLP inline-assembly exception. The
libplacebo allocator must preserve 16-byte alignment across its Windows x64
public/private object boundary. The evidence, rejected alternatives, and patch
retirement criteria are recorded in [FD-001 and FD-003](DECISIONS.md).

## Overlay design

The upstream vcpkg submodule stays immutable. Overlays contain every
project-specific behavior:

- FFmpeg imports the libsmb2 protocol integration, preserves the player
  feature policy, fixes pinned Windows/target portability issues, and keeps
  LTO working with clang-cl/lld-link apart from the single incompatible MLP
  inline-assembly label optimization described above.
- libass avoids desktop system-font discovery on mobile/OHOS, so the
  application must provide subtitle fonts explicitly.
- libplacebo enables its Vulkan and OpenGL backends, including OpenGL ES/EGL
  support on Android and OHOS, plus its built-in Dolby Vision reshaping
  component. Windows additionally enables its D3D11 backend and carries the
  complete static SPIRV-Cross translation closure. Its overlay supplies the
  Python glad generator, receives pinned glslang discovery, and normalizes
  Windows system library flags for FFmpeg's pkg-config probes. The optional
  external `libdovi` raw-RPU parser is not part of the dependency closure.
  The overlay also corrects generated Dolby Vision MMR GLES shaders to use
  integer array indices and valid third-order branch syntax; this is required
  by the strict Maleoon compiler exercised by the OHOS Profile 8.4 run.
  QtAVCore exposes only libplacebo's D3D11 path on Windows, not its installed
  OpenGL or Vulkan capabilities. The static shader-translation closure is
  governed by [FD-002](DECISIONS.md).
- the Android FFmpeg overlay enables FFmpeg's built-in RPU decoder for the
  HEVC MediaCodec wrapper and PTS-correlates parsed Dolby Vision metadata with
  hardware output frames before QtAVCore receives them.
- the OHOS FFmpeg overlay requires the native H.264 and HEVC OHCodec wrappers;
  configure failure is fatal. Its HEVC wrapper enables FFmpeg's built-in RPU
  parser, keeps a bounded metadata queue keyed by the exact microsecond packet
  PTS submitted to OHCodec, and attaches the result to the returned output with
  the same PTS. Flush and close clear the queue and parser state. The overlay
  also exposes a narrow opaque OHCodec surface output token with explicit
  render, timed-render, and drop decisions. An undecided final frame release is
  always a drop, never an implicit present. The verifier checks both decoder
  symbols, the public header, and both release symbols in the installed
  `libavcodec.a` archive. The compatibility boundary is governed by
  [FD-004](DECISIONS.md).
- libsmb2 supplies the missing private Winsock link metadata required by a
  static Windows consumer.

An overlay may replace a builtin port completely, so copied metadata and
wrapper files must be updated when the pinned registry baseline changes.

## Output and consumption contract

The default install database and target prefix are:

```text
ffmpeg/build/<triplet>/vcpkg_installed/
├── vcpkg/                         package status database
└── <triplet>/
    ├── include/
    ├── lib/
    └── share/
```

`include/` exposes FFmpeg and dependency headers, `lib/` contains target
libraries and pkg-config files, and `share/` contains CMake metadata, usage
files, and license notices. Installed metadata must be relocatable and must
not retain paths into `vcpkg/packages`, `vcpkg/buildtrees`, or downloads.

The parent target configures vcpkg with the same triplet, overlay triplet
directory, and installed database. This gives QtAVCore's normal FFmpeg CMake
discovery the complete target-specific static link closure.

## Verification and CI

Every entry script runs `cmake/verify-install.cmake` after vcpkg installation.
The verifier checks:

- required FFmpeg 8/libavcodec 62 headers and metadata;
- H.264/HEVC OHCodec decoder symbols plus the explicit surface-output header
  and release symbols in the OHOS archive;
- OpenSSL, libsmb2, libass, libplacebo/glslang/OpenGL/OpenGL ES/Dolby Vision
  reshaping, dav1d, and Vulkan, plus libplacebo D3D11/SPIRV-Cross on Windows;
- the required FFmpeg feature records;
- absence of wolfSSL and VVenC;
- relocatable installed FFmpeg CMake metadata.

The root GitHub Actions workflow runs automatically for changes to
`ffmpeg/**`: Android and OHOS use self-hosted macOS arm64 runners, while
Windows uses a self-hosted Visual Studio runner. Successful target prefixes
and the vcpkg status database are uploaded as artifacts for parent-project
builds and inspection. That is the current CI topology, not a local-host
restriction; the OHOS package also has a supported Windows PowerShell entry.

These fixed self-hosted runners retain vcpkg's default local binary archive
directory between jobs. CI intentionally does not use `actions/cache` for that
directory: restoring and uploading the complete archive duplicates persistent
runner storage and can transfer roughly a gigabyte even when very little was
rebuilt. If the runners become ephemeral, reevaluate this policy using measured
transfer and build times, preferably with an incremental vcpkg binary source
instead of archiving the entire shared cache.
