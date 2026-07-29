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
│   ├── render/{cpu,opengl,vulkan,d3d11,metal}/
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
- D3D11 software-frame rendering into borrowed Windows render-target views;
- WASAPI shared-mode device output with playback clock and latency reporting;
- Metal software-frame rendering into borrowed textures or drawables;
- CoreAudio device output with playback clock and latency reporting;
- VideoToolbox hardware decoding into retained `CVPixelBuffer` frames;
- zero-copy VideoToolbox/CVMetal plane import and Metal rendering;
- structured video color-space/HDR10 metadata and Metal
  SDR/extended-linear output;
- libswresample conversion to negotiated interleaved PCM;
- RIFF/WAVE diagnostic output through an optional PCM file sink;
- headless console example;
- unit, integration, control, seek, prepare, and loop tests;
- migration and threading documentation.

Known intentional limitations:

- no native audio-device sink outside macOS and Windows yet;
- no OpenGL or Vulkan renderer yet;
- no non-Apple hardware decoder or GPU zero-copy interop yet;
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

- static build: passed;
- shared build: passed;
- CTest: 24/24 passed;
- ASan/UBSan: 24/24 passed;
- all-backends-disabled CTest: 8/8 passed;
- install plus external `find_package(QtAVCore)` consumption of
  `QtAV::Core`, `QtAV::RenderCPU`, `QtAV::RenderMetal`,
  `QtAV::AudioResample`, `QtAV::AudioFile`, `QtAV::AudioCoreAudio`,
  `QtAV::HWVideoToolbox`, and `QtAV::InteropCVMetal`: passed;
- configuration without `pkg-config`: passed;
- runtime linkage inspection: FFmpeg, Apple frameworks, C++ runtime, and
  system libraries only; no Qt dependency;
- MPEG-4/AAC generated-media playback: passed;
- AC-3, E-AC-3, and TrueHD audio-only decoding: passed.
- Windows Visual Studio 2026 static/shared Release CTest: 21/21 passed,
  including WARP D3D11 rendering and WASAPI device/Player playback;
  all-backends-disabled CTest: 8/8 passed;
- Windows install plus external `QtAV::RenderD3D11` and
  `QtAV::AudioWASAPI` CMake consumption: passed.

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
