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
- Keep Objective-C++ (`.mm`) inside Apple backends.
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
│   ├── render/{cpu,mobile,opengl,vulkan,d3d11,metal}/
│   ├── audio/{resample,file,wasapi,coreaudio,alsa,pulseaudio,aaudio}/
│   ├── hwaccel/{d3d11va,videotoolbox,vaapi,mediacodec}/
│   └── interop/{d3d11,cvmetal,vaapi}/
├── platform/{windows,apple,linux,android}/
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
qtav_render_metal
qtav_audio_resample
qtav_audio_file
qtav_audio_wasapi
qtav_audio_coreaudio
qtav_hw_d3d11va
qtav_hw_videotoolbox
qtav_hw_mediacodec
qtav_interop_d3d11
qtav_interop_cvmetal
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
- Metal software-frame rendering into borrowed textures or drawables;
- CoreAudio device output with playback clock and latency reporting;
- VideoToolbox hardware decoding into retained `CVPixelBuffer` frames;
- MediaCodec H.264/HEVC hardware decoding into an application-supplied,
  versioned Android surface, with explicit present/drop output tokens;
- zero-copy VideoToolbox/CVMetal plane import and Metal rendering;
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
- structured video color-space/HDR10 metadata and Metal
  SDR/extended-linear output;
- libswresample conversion to negotiated interleaved PCM;
- RIFF/WAVE diagnostic output through an optional PCM file sink;
- headless console example;
- unit, integration, control, seek, prepare, and loop tests;
- migration and threading documentation.

Known intentional limitations:

- no native audio-device sink outside macOS and Windows yet;
- no OHOS or Linux platform adapters/device coverage yet;
- no Linux hardware decoder, Android GPU zero-copy interop, or OHOS hardware
  decoder yet;
- no subtitles or post-load track switching;
- no production network buffering/recovery policy;
- no compressed Dolby passthrough, Atmos object rendering, Dolby Vision, or
  certification support.

Dolby codec status:

- AC-3, E-AC-3, and TrueHD software decoding have been exercised through the
  FFmpeg audio-frame callback.
- This is decoded PCM-frame support only.

## Build and validation

Normal build:

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

Sanitizer build on Clang:

```sh
cmake -S modern -B build/modern-asan \
  -DCMAKE_BUILD_TYPE=Debug \
  -DQTAV_CORE_BUILD_EXAMPLES=OFF \
  -DCMAKE_CXX_FLAGS="-fsanitize=address,undefined -fno-omit-frame-pointer" \
  -DCMAKE_OBJCXX_FLAGS="-fsanitize=address,undefined -fno-omit-frame-pointer" \
  -DCMAKE_EXE_LINKER_FLAGS="-fsanitize=address,undefined"
cmake --build build/modern-asan --parallel
ASAN_OPTIONS=detect_leaks=0 \
  ctest --test-dir build/modern-asan --output-on-failure
```

`detect_leaks=0` is required by the AddressSanitizer shipped on the macOS
development host; explicit leak detection aborts as unsupported.

Last verified baseline:

- current macOS recheck builds successfully and passes 29/29 CTest after the
  scheduling-isolation audio tests were updated to wait explicitly for frame
  callbacks and to expect one sink drain per completed loop segment, including
  the mobile renderer selector state-machine coverage;
- static build: passed;
- shared build: passed;
- CTest: 29/29 passed;
- ASan/UBSan: 29/29 passed;
- all-backends-disabled macOS CTest: 10/10 passed;
- install plus external `find_package(QtAVCore)` consumption of
  `QtAV::Core`, `QtAV::RenderCPU`, `QtAV::RenderMetal`,
  `QtAV::AudioResample`, `QtAV::AudioFile`, `QtAV::AudioCoreAudio`,
  `QtAV::HWVideoToolbox`, and `QtAV::InteropCVMetal`: passed;
- configuration without `pkg-config`: passed;
- runtime linkage inspection: FFmpeg, Apple frameworks, C++ runtime, and
  system libraries only; no Qt dependency;
- MPEG-4/AAC generated-media playback: passed;
- AC-3, E-AC-3, and TrueHD audio-only decoding: passed.
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
4. Re-run the normal build and CTest baseline before a large refactor.
5. Start from the first unchecked item in the `Next task` section of
   `modern/PLAN.md`, unless the user gives a different priority.
