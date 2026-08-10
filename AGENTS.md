# QtAV repository agent guide

## Project direction

This repository contains two code paths:

1. The repository-root QtAV implementation is the legacy Qt/FFmpeg project.
2. `modern/` is the active Qt-free rewrite, named **QtAVCore**.

New development should target `modern/` unless the task explicitly asks for a
legacy QtAV change. Do not mechanically replace Qt types in the legacy source.
The rewrite is an independent modern C++ implementation that preserves useful
QtAV behavior and uses the public API shape of mdk-sdk as design inspiration.
It is not intended to be source- or binary-compatible with mdk-sdk, and no
mdk-sdk source code should be copied.

Before changing the rewrite, read:

- `modern/README.md`
- `modern/MIGRATION.md`
- `modern/PLAN.md`

`modern/PLAN.md` is the source of truth for milestone status and the next
implementation task. Update it whenever a milestone or meaningful subtask is
completed.

QtAVCore is maintained for Windows, Android, and OHOS targets only. The former
macOS/iOS implementation is preserved under `archived_apple/` as unmaintained
historical material and is not part of the active build, package, test, or
support matrix. Linux is outside the active target matrix and roadmap.

## FFmpeg dependency source

Supported-target QtAVCore builds must consume FFmpeg and its transitive
dependencies from this repository's `ffmpeg/` vcpkg subproject. Do not use a
system/Homebrew FFmpeg or an independently downloaded target FFmpeg as a
fallback. A host `ffmpeg` executable may be used only to generate test media.

When locating an existing dependency package, check these target prefixes
first:

- Android arm64/API 28:
  `ffmpeg/build/arm64-android-28-static/vcpkg_installed/arm64-android-28-static`
- OHOS arm64/API 23:
  `ffmpeg/build/arm64-ohos-23-static/vcpkg_installed/arm64-ohos-23-static`
- Windows x64:
  `ffmpeg/build/x64-windows-static-md/vcpkg_installed/x64-windows-static-md`

Each prefix contains the target `include/`, `lib/`, and `share/` directories;
the associated vcpkg status database is the sibling `vcpkg/` directory under
`vcpkg_installed/`. Project builds and examples must resolve dependencies in
this order:

1. use a valid matching local prefix when present;
2. if it is missing or fails verification, run the matching platform build
   script locally and consume the package it produces.

Do not download a GitHub Actions artifact as a dependency fallback. Workflow
artifacts may remain CI outputs for diagnostics or archival purposes, but they
are not part of the supported local package-resolution path.

Regardless of the resolution order above, any task that modifies `ffmpeg/**`
must run the directly affected native build script locally and pass
`cmake/verify-install.cmake`. Use `ffmpeg/scripts/build-android.sh` or
`ffmpeg/scripts/build-ohos.sh` on macOS,
`ffmpeg/scripts/build-android.ps1` for an Android cross-build on Windows,
`ffmpeg/scripts/build-ohos.ps1` for an OHOS cross-build on Windows, and
`ffmpeg/scripts/build-windows.ps1` for the Windows x64 target.

## Non-negotiable architecture rules

- `modern/` must not include or link Qt.
- `modern/` requires FFmpeg 8.0 or newer. Do not add compatibility branches
  for FFmpeg 5–7.
- Source and text files must use UTF-8 without a byte-order mark (BOM).
- Repository text files must use LF line endings; do not commit CRLF or mixed
  line endings.
- Public QtAVCore headers must not expose Qt or FFmpeg types.
- Use standard C++17 or newer, RAII, PIMPL, standard threading, and
  reference-counted frame lifetime.
- Keep the core independent of window systems, GUI toolkits, graphics APIs,
  and platform audio APIs.
- Platform implementations are separate CMake targets in the same repository,
  not separate repositories at this stage.
- Organize platform work by responsibility:
  - `backends/render/`
  - `backends/audio/`
  - `backends/hwaccel/`
  - `backends/interop/`
  - shared operating-system helpers under `platform/`
- Do not reconnect, modify, or present `archived_apple/` as supported code
  unless the user explicitly requests revival of Apple platform support.
- Keep Windows SDK, COM, WRL, D3D, DXGI, and WASAPI headers inside Windows
  backends.
- Before starting implementation that depends on Windows platform features,
  including the Windows SDK, COM, D3D, DXGI, WASAPI, or D3D11VA, determine
  whether the current development host is Windows.
- If the current development host is not Windows, warn the user and refuse to
  proceed with the implementation or its next development step. Continue only
  when the user explicitly states that cross-platform development for that
  task is intended; when continuing, clearly identify any Windows-native build
  or runtime validation that cannot be performed on the current host.
- Newer Android and OHOS releases may require the user to approve installation
  or replacement of a test application on the physical device. If installing
  or updating a test application fails and device-side authorization may be
  pending, pause deployment and ask the user to respond to the prompt manually
  on the device. Do not repeatedly retry, bypass, or automate that approval.
- Prefer explicit, optional backend linkage first. Do not introduce runtime
  plugin loading until the backend API is stable.
- If dynamic plugins are introduced later, use a versioned C ABI at the shared
  library boundary instead of exposing STL or C++ virtual ABI across arbitrary
  compilers.
- Hardware decode, hardware-frame interop, rendering, and audio output are
  separate responsibilities. Do not combine them in a single platform class.
- Preserve the legacy QtAV tree unless a migration requires a narrowly scoped
  compatibility change.

## Target module layout

The intended structure is:

```text
modern/
├── core/
│   ├── include/qtav/
│   └── src/
├── backends/
│   ├── render/{cpu,mobile,opengl,vulkan,d3d11}/
│   ├── audio/{resample,file,wasapi,aaudio,ohaudio}/
│   ├── hwaccel/{d3d11va,mediacodec,ohcodec}/
│   └── interop/{d3d11,mediacodec_vulkan,mediacodec_opengl,ohcodec_vulkan,ohcodec_opengl}/
├── platform/{windows,android,ohos}/
├── examples/
└── tests/
```

The core implementation now lives under `modern/core/` while preserving its
installed include paths and `QtAV::Core` target name.

Expected target names:

```text
qtav_core
qtav_render_cpu
qtav_render_mobile
qtav_render_opengl
qtav_render_vulkan
qtav_render_d3d11
qtav_audio_resample
qtav_audio_file
qtav_audio_wasapi
qtav_audio_aaudio
qtav_hw_d3d11va
qtav_hw_mediacodec
qtav_interop_d3d11
qtav_interop_mediacodec_vulkan
qtav_interop_mediacodec_opengl
```

## API conventions

The main namespace is `qtav`.

The `Player` facade follows these conventions:

- `setMedia()`, `prepare()`, `setState()`, and `seek()` control playback.
- Operations that perform I/O or decoding are asynchronous.
- State, status, event, audio-frame, and video-frame notifications use
  `std::function`.
- `setRenderCallback()` asks the application to schedule a redraw.
- `renderVideo()` is called by the application on the native render thread.
- Frame objects are cheap, reference-counted views whose data remains valid
  while a copied frame object is alive.
- Callbacks currently run on the playback worker. They may request a new state
  but must not destroy the player from inside the callback.
- New public API needs documentation in `modern/README.md` and migration notes
  where it replaces a legacy QtAV concept.

Platform-specific APIs may use strong native types in backend-specific headers,
but those headers must not be included by the core public headers.

## Current implementation snapshot

The first Qt-free core milestone is complete.

Implemented under `modern/`:

- standalone CMake 3.20 project and installable `QtAV::Core` package;
- C++17 `qtav::Player` facade with PIMPL;
- asynchronous standard-library worker and state machine;
- interruptible FFmpeg open/read when stopping, seeking, or changing media;
- FFmpeg 8+ send/receive software decoding;
- best-stream audio/video selection;
- `setMedia`, `prepare`, play, pause, resume, stop, and seek;
- monotonic playback clock and playback-rate control;
- A-B playback range and finite/infinite loop;
- media and track information;
- string properties and `avformat.*` option forwarding;
- reference-counted `VideoFrame` and `AudioFrame`;
- audio/video frame callbacks;
- mdk-style render scheduling with `setRenderCallback()`,
  `setVideoRenderer()`, and `renderVideo()`;
- libswscale CPU rendering into application-owned image buffers;
- D3D11 software-frame rendering into borrowed Windows render-target views,
  with optional swap-chain-driven Advanced Color SDR, FP16 scRGB, and RGB10
  HDR10 output;
- WASAPI shared-mode device output with playback clock and latency reporting;
- D3D11VA hardware decoding into retained NV12/P010 texture-array slices;
- zero-CPU-map D3D11VA/D3D11 Video Processor interop into shader-readable
  SDR BGRA8, FP16 scRGB, or RGB10/PQ textures;
- Windows PQ/HLG EOTF, BT.2020 conversion, display-aware tone mapping,
  per-frame `IDXGIOutput6` capability/display switching, and system SDR
  reference-white handling;
- MediaCodec H.264/HEVC hardware decoding into an application-supplied,
  versioned Android surface, with explicit present/drop output tokens;
- private GPU-sampled Android `AImageReader`/`AHardwareBuffer` MediaCodec
  interop into Vulkan external-format YCbCr textures, with timestamp
  correlation, aligned-allocation cropping, foreign-queue ownership, release
  sync-fd return, and zero decoded-source CPU map/transfer/staging/upload;
- Android MediaCodec `SurfaceTexture` interop into
  `GL_TEXTURE_EXTERNAL_OES`, with timestamp/generation correlation,
  single-current-image lifetime, seek/flush and EGL surface-recreation
  coverage, and zero decoded-source CPU map/transfer/staging/upload;
- platform-neutral Vulkan software-frame rendering with structured color and
  geometry shaders, explicit SDR/HDR10/extended-linear output color spaces,
  plus an Android `ANativeWindow` adapter with native HDR swapchain selection
  and optional `VK_EXT_hdr_metadata` submission;
- platform-neutral OpenGL ES 3.x software-frame rendering for
  YUV/NV12/P010/RGB families with structured color and geometry handling,
  explicit SDR/PQ/HLG output encoding, plus an Android EGL/`ANativeWindow`
  adapter with exact RGB10_A2 native HDR and explicit RGBA8/sRGB fallback;
- a platform-neutral mobile renderer selector with Vulkan-preferred startup,
  bounded same-API surface recovery, fatal one-way OpenGL ES fallback, native
  window suspension/recreation, and explicit no-renderer behavior;
- explicit mobile hardware-frame fallback routes that reconfigure subsequent
  decoder output for compatible OpenGL ES interop, direct-surface
  presentation, software decode, or no video without retrying or mapping a
  frame from the retired Vulkan surface;
- structured video color-space/HDR10 metadata across active renderers;
- libswresample conversion to negotiated interleaved PCM;
- RIFF/WAVE diagnostic output through an optional PCM file sink;
- headless console example;
- unit, integration, control, seek, prepare, and loop tests;
- migration and threading documentation.

Known intentional limitations:

- no OHOS platform adapter or native audio-device coverage yet;
- no OHOS hardware decoder yet;
- no subtitles or post-load track switching;
- no production network buffering/recovery policy;
- no compressed Dolby passthrough, Atmos object rendering, Dolby Vision, or
  certification support.

Dolby codec status:

- AC-3, E-AC-3, and TrueHD software decoding have been exercised through the
  FFmpeg audio-frame callback.
- This is decoded PCM-frame support only.

## Build and validation

Run native builds only on Windows or with an Android/OHOS target toolchain.
Configuring `modern/` for a macOS/iOS or Linux target must fail at the platform
support gate.

Normal supported-target build:

```sh
cmake -S modern -B build/modern \
  -DQTAV_CORE_BUILD_TESTS=ON \
  -DQTAV_CORE_BUILD_EXAMPLES=ON
cmake --build build/modern --parallel
ctest --test-dir build/modern --output-on-failure
```

Shared-library build:

```sh
cmake -S modern -B build/modern-shared \
  -DBUILD_SHARED_LIBS=ON \
  -DQTAV_CORE_BUILD_TESTS=ON
cmake --build build/modern-shared --parallel
ctest --test-dir build/modern-shared --output-on-failure
```

Sanitizer build on a supported Clang target:

```sh
cmake -S modern -B build/modern-asan \
  -DCMAKE_BUILD_TYPE=Debug \
  -DQTAV_CORE_BUILD_EXAMPLES=OFF \
  -DCMAKE_CXX_FLAGS="-fsanitize=address,undefined -fno-omit-frame-pointer" \
  -DCMAKE_EXE_LINKER_FLAGS="-fsanitize=address,undefined"
cmake --build build/modern-asan --parallel
ctest --test-dir build/modern-asan --output-on-failure
```

Last verified baseline:

- the former macOS/iOS validation record is historical and retained only in
  `archived_apple/README.md`;
- after archival, the Android arm64 NativeActivity harness and user-player
  native library both reconfigure and link; changed generic tests compile with
  the Android toolchain, and native macOS configuration fails at the intended
  supported-target gate;
- Android arm64 FFmpeg 8.1.2/QtAVCore/Vulkan build, APK packaging, and
  connected Adreno 830 device playback: passed with 180 decoded and
  Vulkan-presented video frames through a required HDR10/PQ swapchain, 282
  decoded audio frames, `VK_EXT_hdr_metadata`, a background/foreground HDR
  surface recreation, synthetic P010/BT.2020/PQ presentation with mastering
  and MaxCLL metadata, Android compositor HDR-layer recognition, and offscreen
  Vulkan goldens for the three-frame ring, SDR and P010/BT.2020 PQ/HLG color
  conversion, native 10-bit PQ/HLG output, HLG-to-PQ conversion, FP16
  extended-linear/BT.2020-linear output, HDR luminance-metadata selection,
  viewport, rotation, and target recreation.
- Android OpenGL ES 3.2 fallback build, package export, external CMake
  consumption, offscreen readback, and connected Adreno 830 device
  presentation: passed for YUV420/422/444, NV12/NV21, P010,
  RGB/BGR/RGBA/BGRA/ARGB, Gray8, viewport, rotation, target-generation
  replacement, P010/PQ-to-SDR, native BT.2020/PQ and BT.2020/HLG encoding,
  real-window exact RGB10_A2 BT.2020/PQ presentation, and Android compositor
  HDR-layer recognition.
- Android mobile selector NDK build, package export, deterministic startup,
  recovery, fatal-fallback, one-way, and no-renderer tests, plus connected
  Adreno 830 Vulkan HDR surface recreation and forced initial EGL fallback:
  passed.
- Android MediaCodec H.264/HEVC NDK build, package export, direct-surface
  present/drop, seek/flush, media replacement, explicit stop, surface
  recreation, stale-generation rejection, and clean shutdown on the connected
  Adreno 830 device: passed.
- Android MediaCodec H.264/HEVC private-AImageReader Vulkan import, package
  export, static/shared cross-builds, external CMake consumption,
  external-format YCbCr sampling, bounded timestamp correlation, and
  release-fence return: passed on the connected Adreno 830 device with 89
  imports and 89 release fences per codec, maximum pending depth one, and zero
  decoded-source CPU map/transfer/staging/upload.
- Android MediaCodec H.264/HEVC `SurfaceTexture` OpenGL ES interop, package
  export, static/shared cross-builds, external CMake consumption,
  `GL_TEXTURE_EXTERNAL_OES` sampling, bounded timestamp correlation,
  seek/flush, EGL window suspension/recreation, and clean shutdown: passed on
  the connected Adreno 830 device with 223 H.264 and 179 HEVC images latched,
  maximum pending depth two, and zero decoded-source CPU
  map/transfer/staging/upload.
- Android fatal Vulkan-to-OpenGL ES MediaCodec fallback on the same H.264
  media session: passed with 32 Vulkan-generation frames, 180
  SurfaceTexture-generation frames, 30 AHardwareBuffer imports and matching
  release fences, 179 external-OES images, and zero decoded-source
  map/transfer/staging/upload.
- Windows Visual Studio 2026 static/shared Release CTest: 32/32 passed,
  including WARP D3D11 contracts, D3D11VA lifecycle, native H.264/NV12 plus
  HEVC Main10/P010 zero-CPU-map Video Processor rendering, WASAPI device
  output, and strict H.264/AAC native A/V playback with audible output;
  all-backends-disabled CTest: 11/11 passed;
- Windows static/shared install plus external `QtAV::RenderD3D11`,
  `QtAV::HWD3D11VA`, `QtAV::InteropD3D11`, and `QtAV::AudioWASAPI`
  CMake consumption: passed.
- current Windows static/shared Release CTest after the high-level D3D11
  composition output: 34/34 passed with Windows HDR disabled and enabled,
  including native FP16/RGB10 flip-model output, PQ/BT.2020 Main10
  HDR-preserving zero-CPU-map readback, and active-HDR validation on a
  PHL 27B1U7903.

Before finishing any implementation turn:

1. Run the tests proportional to the changed scope.
2. Run `git diff --check`.
3. Search new core code for accidental Qt dependencies.
4. Update `modern/PLAN.md`.
5. Update documentation when public API or threading behavior changes.

## Continuation procedure for a new conversation

1. Read this file and `modern/PLAN.md`.
2. Inspect `git status --short`; existing changes may be intentional.
3. Do not discard or overwrite the existing uncommitted `modern/` rewrite.
4. Re-run the appropriate Windows, Android, or OHOS build/test baseline before
   a large refactor; do not use a macOS/iOS or Linux target as active support
   validation.
5. Start from the first unchecked item in the `Next task` section of
   `modern/PLAN.md`, unless the user gives a different priority.
