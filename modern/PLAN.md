# QtAVCore implementation plan

Last updated: 2026-07-30

Status legend:

- `[x]` complete and verified
- `[~]` implemented partially or suitable only as a development baseline
- `[ ]` not implemented

## Current baseline

The Qt-free core is functional and lives under `modern/`. The legacy QtAV code
has not been refactored or removed. QtAVCore builds independently with:

```sh
cmake -S modern -B build/modern
```

Continuation checkpoint:

- baseline implementation commit: `62e84956` (`Add Qt-free QtAVCore rewrite`);
- the latest completed scope connects `AudioSink` to `Player`, promotes a
  valid device clock to playback master, adds the portable render/audio
  reference backends, the Apple Metal software renderer and CoreAudio sink,
  and an optional VideoToolbox decode path that produces reference-counted
  `CVPixelBuffer` hardware frames with explicit software fallback, then imports
  limited/full-range NV12/P010 pixel-buffer planes into Metal without a CPU
  copy and applies structured SDR/HDR color metadata, plus the Windows D3D11
  software-frame renderer using borrowed native resources and the Windows
  WASAPI shared-mode device sink with event-driven playback and native
  clocking;
- the Apple reference path, including full extended-linear BT.2020 Metal EDR
  layer configuration, HDR10/HLG `CAEDRMetadata`, real-time display-headroom
  adaptation, HDR FP16 pixel validation, and a conditional real-screen EDR
  test, and the Windows D3D11 software-frame and WASAPI audio paths are
  complete;
  the D3D11VA device/frame/interop design and supplied-device core bridge are
  complete; the native `qtav_hw_d3d11va` decoder backend and
  `qtav_interop_d3d11` Video Processor path are complete, including
  same-device validation, SDR BGRA8 plus HDR RGB10/FP16 conversion, native
  H.264/NV12 and PQ/BT.2020 HEVC Main10/P010 zero-CPU-map rendering,
  Windows Advanced Color swap-chain/display tracking, lifecycle coverage,
  example wiring, installed target export, and strict native H.264/AAC
  playback through an active WASAPI render endpoint; the implementation,
  SDR-state tests, and active-HDR native display validation are complete;
- playback scheduling now isolates the FFmpeg demux/decode worker, bounded
  audio-output queue/worker, and bounded presentation queue/worker; UI/render
  callbacks cannot block device audio submission, `Player::position()` uses a
  cached device-clock snapshot, and late video is dropped instead of building
  unbounded presentation latency;
  Milestone 5 is complete and the active next platform task is the Android
  production path, followed by OHOS and then Linux; the shared Android/OHOS
  responsibility and lifecycle design is now recorded in `MOBILE.md`, and the
  Android arm64 Vulkan checkpoint cross-builds FFmpeg 8.1.2 plus QtAVCore,
  packages a minimal NativeActivity APK, verifies generated software A/V
  decode, and presents decoded software frames through a bounded three-frame
  platform-neutral Vulkan engine plus Android surface/swapchain adapter on a
  connected Android device, including SDR and native-HDR offscreen pixel
  goldens, required HDR10/PQ swapchain selection, static HDR metadata, and
  background/foreground HDR surface recreation; the shared OpenGL ES 3.x
  software renderer and Android EGL/window adapter are also complete with
  all advertised software upload families, SDR color/geometry readback, and
  real-window P010/PQ-to-SDR presentation;
- QtAVCore now requires FFmpeg 8.0 or newer (libavcodec major 62+); compatibility
  branches for FFmpeg 5–7 are intentionally out of scope;
- the root `README.md` and `AGENTS.md` now record the modern entry point and
  FFmpeg 8 minimum; legacy build guidance remains unchanged;
- Homebrew CMake 4.4.1 is installed at `/opt/homebrew/bin/cmake`;
- the last local FFmpeg used for verification was Homebrew FFmpeg 8.1.2.

Current public entry points:

- `modern/core/include/qtav/player.h`
- `modern/core/include/qtav/frame.h`
- `modern/core/include/qtav/color.h`
- `modern/core/include/qtav/media.h`
- `modern/core/include/qtav/video_render_api.h`
- `modern/core/include/qtav/audio_sink.h`
- `modern/core/include/qtav/audio_converter.h`
- `modern/core/include/qtav/hardware_decoder.h`
- `modern/core/include/qtav/hardware_frame.h`
- `modern/backends/audio/file/include/qtav/wav_audio_sink.h`
- `modern/backends/audio/coreaudio/include/qtav/coreaudio_audio_sink.h`
- `modern/backends/audio/wasapi/include/qtav/wasapi_audio_sink.h`
- `modern/backends/render/metal/include/qtav/metal_video_renderer.h`
- `modern/backends/render/d3d11/include/qtav/d3d11_video_renderer.h`
- `modern/backends/render/vulkan/include/qtav/vulkan_video_renderer.h`
- `modern/backends/render/vulkan/android/include/qtav/android_vulkan_video_renderer.h`
- `modern/backends/render/opengl/include/qtav/opengl_video_renderer.h`
- `modern/backends/render/opengl/android/include/qtav/android_opengl_video_renderer.h`
- `modern/backends/hwaccel/d3d11va/include/qtav/d3d11va_hardware_decoder.h`
- `modern/backends/hwaccel/videotoolbox/include/qtav/videotoolbox_hardware_decoder.h`
- `modern/backends/interop/cvmetal/include/qtav/cvmetal_frame_interop.h`
- `modern/backends/interop/d3d11/include/qtav/d3d11_frame_interop.h`
- `modern/MOBILE.md`

Current implementation:

- `modern/core/src/player.cpp`
- `modern/core/src/frame.cpp`
- `modern/core/src/backend.cpp`
- `modern/core/src/hardware_decoder.cpp`
- `modern/backends/render/cpu/include/qtav/cpu_video_renderer.h`
- `modern/backends/render/cpu/src/cpu_video_renderer.cpp`
- `modern/backends/audio/resample/include/qtav/swresample_audio_converter.h`
- `modern/backends/audio/resample/src/swresample_audio_converter.cpp`
- `modern/backends/audio/file/include/qtav/wav_audio_sink.h`
- `modern/backends/audio/file/src/wav_audio_sink.cpp`
- `modern/backends/audio/coreaudio/src/coreaudio_audio_sink.cpp`
- `modern/backends/audio/wasapi/src/wasapi_audio_sink.cpp`
- `modern/backends/render/metal/src/metal_video_renderer.mm`
- `modern/backends/render/d3d11/src/d3d11_video_renderer.cpp`
- `modern/backends/render/vulkan/src/vulkan_video_renderer.cpp`
- `modern/backends/render/vulkan/android/src/android_vulkan_video_renderer.cpp`
- `modern/backends/render/opengl/src/opengl_video_renderer.cpp`
- `modern/backends/render/opengl/android/src/android_opengl_video_renderer.cpp`
- `modern/backends/hwaccel/d3d11va/src/d3d11va_hardware_decoder.cpp`
- `modern/backends/hwaccel/videotoolbox/src/videotoolbox_hardware_decoder.cpp`
- `modern/backends/interop/cvmetal/src/cvmetal_frame_interop.mm`
- `modern/backends/interop/d3d11/src/d3d11_frame_interop.cpp`
- `modern/tests/audio_sink_player_test.cpp`
- `modern/tests/simulated_audio_sink.h`
- `modern/tests/simulated_audio_sink.cpp`
- `modern/tests/simulated_audio_sink_test.cpp`
- `modern/tests/simulated_audio_sink_player_test.cpp`
- `modern/tests/cpu_video_renderer_test.cpp`
- `modern/tests/audio_resample_player_test.cpp`
- `modern/tests/wav_audio_sink_test.cpp`
- `modern/tests/wav_audio_sink_player_test.cpp`
- `modern/tests/coreaudio_audio_sink_test.cpp`
- `modern/tests/wasapi_audio_sink_test.cpp`
- `modern/tests/metal_video_renderer_test.mm`
- `modern/tests/metal_edr_display_test.mm`
- `modern/tests/d3d11_video_renderer_test.cpp`
- `modern/tests/vulkan_video_renderer_test.cpp`
- `modern/tests/vulkan_video_renderer_test_support.cpp`
- `modern/tests/opengl_video_renderer_test_support.cpp`
- `modern/tests/d3d11va_hardware_decoder_test.cpp`
- `modern/tests/d3d11_frame_interop_test.cpp`
- `modern/tests/videotoolbox_hardware_decoder_test.cpp`
- `modern/tests/cvmetal_frame_interop_test.mm`
- `modern/tests/hardware_decode_device_test.cpp`
- `modern/examples/android/native_activity.cpp`
- `modern/examples/android/build-android.sh`
- `modern/examples/android/run-connected-device.sh`

Current verification:

- the current macOS host recheck before and after the OpenGL ES work builds
  successfully, but CTest is 25/27: `qtav_core_audio_sink_player` consistently
  misses its expected audio-frame callback and
  `qtav_simulated_audio_sink_player` misses its expected loop drain. Both
  failures reproduced on the clean pre-change baseline and are outside this
  rendering slice;
- the current static and shared Windows builds pass 33/33 CTest tests,
  including the Advanced Color test, the
  WASAPI device test and strict native H.264/AAC playback;
- static and shared macOS builds pass 27/27 CTest tests, including numeric
  FP16 HDR/BT.2020/headroom checks and real-screen EDR presentation on the
  active EDR-capable display;
- ASan/UBSan passes 27/27 macOS-applicable tests with leak detection disabled;
- the Metal renderer passes an iOS 16 arm64 Objective-C++ syntax build;
- the all-backends-disabled build passes 11/11 tests, including the Windows
  platform device-access contract test;
- forcing an unimplemented backend to `ON` fails with a clear diagnostic;
- invalid backend option values are rejected;
- installation and external `QtAV::RenderCPU`, `QtAV::RenderMetal` including
  the EDR API,
  `QtAV::AudioResample`, `QtAV::AudioFile`, plus
  `QtAV::AudioCoreAudio`, `QtAV::HWVideoToolbox`, and
  `QtAV::InteropCVMetal` CMake consumption pass;
- FFmpeg 8 minimum enforcement passes in the source and installed package;
- configuration without `pkg-config` passes;
- runtime linkage contains no Qt;
- core public-header scans contain no Qt, FFmpeg, or platform SDK types;
- MPEG-4/AAC, AC-3, E-AC-3, and TrueHD decode tests pass.
- on Windows with Visual Studio 2026 and vcpkg FFmpeg 8.1.2, static/shared
  Release tests cover the supplied hardware-device bridge, deterministic WARP
  texture/slice/lifetime/locking contracts, D3D11 software/imported/mapped
  rendering, native H.264/NV12 and HEVC Main10/P010 D3D11VA-to-Video-Processor
  presentation with verified pixels and no CPU mapping, plus pause/resume,
  seek, media replacement, stop, surface recreation, and retained-frame use
  after player shutdown;
- Windows Advanced Color coverage now includes deterministic PQ/HLG EOTF,
  BT.2020 conversion, SDR tone mapping, FP16 scRGB and RGB10/PQ numeric
  readback, a native flip-model swap chain, `IDXGIOutput6`,
  `SetColorSpace1`, SDR-white lookup, and same-adapter display switching while
  Windows HDR is disabled and enabled. Active-HDR validation on a
  PHL 27B1U7903 reported a 10-bit G2084/P2020 output, system-derived 240-nit
  SDR white, 1405.11-nit peak luminance, and 1000-nit PQ output above scRGB
  `1.0`;
- the strict generated H.264/AAC native example test proves simultaneous
  D3D11VA decode, D3D11 rendering, audio decode, and audible WASAPI output
  through the active render endpoint;
- Windows multi-config FFmpeg imports select matching Debug/Release libraries,
  and project DLLs, tests, and examples share a runnable `bin/<Config>`
  directory;
- installation plus external CMake consumption of `QtAV::PlatformWindows`,
  `QtAV::HWD3D11VA`, `QtAV::RenderD3D11`, `QtAV::InteropD3D11`, and
  `QtAV::AudioWASAPI` together with the portable core, render, and audio
  targets passes for static and shared builds; the installed core token links
  without installing its private FFmpeg bridge header.
- on the macOS arm64 host, NDK r28c cross-builds the pinned FFmpeg 8.1.2
  minimal libraries and QtAVCore for Android `arm64-v8a`; the signed
  NativeActivity APK passes 16 KB ELF segment-alignment checks and installs on
  an Android 16/API 36 device with an Adreno 830;
- the connected-device harness decodes its generated AVI through QtAVCore and
  reports `PASS` with 180 MPEG-4 video frames, 180 Vulkan-presented frames,
  282 PCM audio frames, a required native HDR10/PQ swapchain with
  `VK_EXT_hdr_metadata`, one background/foreground HDR surface recreation
  without media reopen, and platform-neutral offscreen pixel goldens covering
  three-frame ring reuse, viewport, rotation, target replacement, limited/full
  range, BT.601/BT.709 conversion, P010/BT.2020 PQ/HLG HDR-to-SDR conversion,
  native 10-bit PQ/HLG output, HLG-to-PQ conversion, mastering-display, MaxCLL,
  and default-luminance selection; install
  authorization was confirmed manually on the device after the harness
  stopped on `INSTALL_FAILED_USER_RESTRICTED`.
- the same Android 16/Adreno 830 harness now builds and runs
  `QtAV::RenderOpenGL` plus `QtAV::RenderOpenGLAndroid`: offscreen readback
  covers YUV420/422/444, NV12/NV21, P010, RGB/BGR, RGBA/BGRA/ARGB, Gray8,
  viewport, rotation, and target-generation replacement, and the real EGL
  window adapter presents a P010/BT.2020/PQ frame through the documented SDR
  fallback after Vulkan HDR playback and lifecycle validation; the latest
  combined run passed with more than 170 decoded/presented video callbacks,
  282 audio frames, and one background/foreground surface recreation.
- Android install plus external CMake consumption of
  `QtAV::RenderOpenGLAndroid` and `QtAV::RenderOpenGL` passes; the exported
  static targets use logical `EGL`/`GLESv3` link names and contain no producer
  NDK or host path.

## Milestone 0 — Qt-free playback core

- [x] Standalone modern CMake project.
- [x] C++17 public API without Qt or FFmpeg types.
- [x] PIMPL `qtav::Player`.
- [x] Asynchronous playback state machine.
- [x] Interruptible FFmpeg I/O.
- [x] FFmpeg send/receive audio and video decoding.
- [x] Reference-counted audio/video frame lifetime.
- [x] Prepare, play, pause, resume, seek, and stop.
- [x] Playback rate, range, and loop.
- [x] Media information and track enumeration.
- [x] Frame callbacks.
- [x] Render scheduling and pull-style `renderVideo()`.
- [x] Headless example and automated tests.
- [x] Installable CMake package.

## Milestone 1 — Repository and target decomposition

Goal: establish the long-term monorepo layout before platform code is added.

- [x] Move the current library implementation to `modern/core/`.
- [x] Preserve installed header paths under `include/qtav/`.
- [x] Preserve the `qtav_core` and `QtAV::Core` target names.
- [x] Add empty/optional backend roots:
  - `modern/backends/render/`
  - `modern/backends/audio/`
  - `modern/backends/hwaccel/`
  - `modern/backends/interop/`
- [x] Add `modern/platform/` for small shared OS helpers.
- [x] Add CMake `AUTO/ON/OFF` backend options.
- [x] Verify that configuring unsupported backends as `ON` fails clearly.
- [x] Verify the core still builds when every backend is disabled.

Acceptance:

- the existing API remains source-compatible and its tests continue to pass;
- no platform SDK header is reachable from core headers;
- static, shared, sanitizer, and install-consumer tests still pass.

## Milestone 2 — Backend contracts

Goal: define stable compile-time backend APIs before implementing a native
renderer or audio device.

### Video

- [x] Add a generic `VideoRenderAPI` contract.
- [x] Define surface size, viewport, aspect ratio, rotation, and redraw
  lifecycle.
- [x] Define whether a backend owns or borrows its device/context/surface.
- [x] Add backend capability reporting.
- [x] Keep concrete platform native-handle types out of generic public
  headers.
- [x] Add backend-specific headers for strong native types where useful.
- [x] Allow multiple renderer instances keyed by application opaque pointer.

### Audio

- [x] Add an `AudioSink` contract.
- [x] Define open, close, pause, flush, write, drain, latency, and clock
  reporting.
- [x] Define negotiated device format separately from decoded frame format.
- [x] Bind an optional sink to `Player` and use a valid device clock as the
  playback master with monotonic fallback.

### Hardware frames

- [x] Add a generic hardware-frame handle/lifetime representation.
- [x] Define CPU-map and native-handle queries.
- [x] Define interop capabilities without including D3D, Metal, VAAPI, or
  Android headers in the core API.

Acceptance:

- mock video and audio backends can run in tests;
- backend callbacks can safely request player state changes;
- no C++ dynamic plugin ABI is committed yet.

### Review focus

- `VideoRenderAPI` lifecycle and resource-ownership fields;
- coexistence of the legacy `setVideoRenderer()` callback with multiple
  `VideoRenderAPI` instances keyed by application opaque pointers;
- `AudioSinkOpenResult` separating decoded and negotiated device formats;
- synchronous, non-owning `AudioBufferView` lifetime and plane layout;
- `HardwareFrameData`, reference-counted CPU mappings, and opaque
  `NativeHandle` representation;
- keeping backend calls outside the player mutex so backend callbacks may
  safely request player state changes.

## Completed portable reference backends

Completed scope:

1. [x] Add a `qtav_render_cpu` target backed by libswscale.
2. [x] Define an application-owned CPU image-buffer target and deterministic
   pixel-format conversion tests.
3. [x] Add a libswresample conversion backend that can satisfy a negotiated
   interleaved PCM `AudioFormat`.
4. [x] Connect that conversion path between decoded frames and `AudioSink`.
5. [x] Keep Metal and D3D deferred until the portable reference paths pass
   tests.

Completed CPU rendering checkpoint:

- `QtAV::RenderCPU` is optional under `QTAV_RENDER_CPU=AUTO/ON/OFF` and links
  libswscale without adding it to `QtAV::Core`;
- `CpuImageBuffer` describes an application-owned positive-stride packed
  destination without exposing FFmpeg types;
- `CpuVideoRenderer` implements the `VideoRenderAPI` lifecycle and reports its
  initial full-surface `Stretch`/`Rotate0` capability;
- deterministic lossless-frame tests cover scaling to BGRA, RGBA, and Gray8,
  invalid stride rejection, and untouched row padding.

Completed audio conversion checkpoint:

- `AudioFrameConverter` keeps PCM conversion optional and injected, so
  `QtAV::Core` does not link libswresample;
- `QtAV::AudioResample` converts sample format, sample rate, and channel layout
  to an interleaved device format;
- `Player` opens the converter only when sink negotiation requires it, resets
  it on flush/seek, drains it at natural end, and closes it with the sink;
- deterministic PCM tests cover 8 kHz mono to 16 kHz stereo S16 conversion,
  exact drained sample count, continuous timestamps, and seek reset.

Completed audio integration checkpoint:

- `Player::setAudioSink()` binds or removes an optional sink;
- the first decoded audio frame opens it, exact-format frames are written
  without disturbing `onAudioFrame()`, and unsupported conversion reports a
  media event;
- pause/resume, seek, media replacement, stop, sink replacement, and shutdown
  drive the sink lifecycle without holding `Player::Impl::mutex_`;
- a supported, valid device clock is the playback master with monotonic
  fallback;
- deterministic mock tests cover device-clock use, backend event reentrancy,
  pause, seek, media replacement, stop, and shutdown.

Completed playback scheduling isolation checkpoint:

- FFmpeg demux and audio/video decode remain together on the asynchronous
  playback worker so control interruption and codec ownership stay simple;
- decoded audio crosses a bounded 500 ms queue to a dedicated audio-output
  worker, preventing render callbacks and UI work from starving the device;
- decoded video and frame/render notifications cross a bounded,
  timestamp-ordered presentation queue; stale late video is discarded when the
  application render path falls behind;
- device-clock reads are sampled by the audio-output worker and published as a
  generation-checked cache, so UI calls to `Player::position()` cannot wait on
  a sink write;
- a deterministic regression test blocks both the first sink write and the
  render callback, verifies that `position()` remains non-blocking, and verifies
  that audio writes continue while presentation is blocked;
- separate packet-demux and per-stream decoder workers are intentionally
  deferred: the current bounded post-decode queues remove the observed
  cross-layer blocking without duplicating FFmpeg ownership. Revisit packet
  queues when production buffering, track switching, or live-stream recovery
  requires independent decode back-pressure.

## Completed deterministic portable audio validation

Completed before starting Apple production backends:

1. [x] Add a test-only simulated `AudioSink` with configurable negotiated
   format, queue capacity, latency, and device-clock position.
2. [x] Model deterministic buffer consumption, pause/resume, flush, underrun,
   and end-of-stream drain without sleeping on wall-clock time.
3. [x] Add A/V master-clock tests covering resampling, simulated device
   latency, seek, loop, media replacement, and monotonic fallback.
4. [x] Decide whether the simulated sink should remain test-only or become an
   optional installed `qtav_audio_null` backend.
5. [x] Add an optional file/PCM diagnostic sink only after the timing contract
   is proven by the simulated sink.

Decision checkpoint:

- keep `SimulatedAudioSink` under `modern/tests/`; its manual clock controls,
  snapshots, and test synchronization API are intentionally not an installed
  playback backend;
- `AudioSink::drain()` now has a source-compatible default no-op, and `Player`
  drains queued sink audio at natural end after draining the converter;
- deterministic tests cover capacity rejection, dynamic queued latency,
  pause/resume, flush/re-anchor, underrun de-duplication, drain, resampling,
  A/V device-master timing, seek, loop, media replacement, and invalid-clock
  monotonic fallback;
- a future `qtav_audio_null` target, if needed, should be a separate production
  pacing/diagnostic backend without exposing the simulator's test controls.

Completed file-output checkpoint:

- `QtAV::AudioFile` is optional under `QTAV_AUDIO_FILE=AUTO/ON/OFF`, has no
  dependency beyond `QtAV::Core`, and installs as an exported package target;
- `WavAudioSink` derives a requested interleaved PCM format from decoded audio,
  allowing the injected converter to handle planar input, resampling, and
  channel conversion;
- the sink writes little-endian RIFF/WAVE output, updates header sizes on close,
  flushes on seek without discarding captured samples, and intentionally
  reports no device clock;
- deterministic tests verify header fields, sample byte order, invalid planar
  negotiation, and an exact 64,000-byte converted player capture.

## Next task

Begin the Android production path:

Windows Advanced Color validation is complete: on a PHL 27B1U7903,
`qtav_render_d3d11_advanced_color_test` passed with
`QTAV_REQUIRE_ACTIVE_HDR=1`, active G2084/P2020 output, an FP16 G10/P709 scRGB
swap chain, system SDR white and panel luminance queries, and preservation of
the 1000-nit PQ sample above scRGB `1.0`; static and shared Windows CTest runs
passed 33/33.

1. [x] Complete the shared Android/OHOS mobile design checkpoint below before
   adding either platform's hardware decoder.
2. [x] Establish reproducible macOS-hosted Android NDK and FFmpeg 8+
   cross-builds, package a minimal native test application, and run it on a
   connected arm64 Android device.
3. [x] Add the reusable Vulkan renderer engine and Android surface adapter,
   keeping window, device, and swapchain ownership outside core.
4. [x] Add native Vulkan HDR output before advancing the mobile plan: make the
   current-target color space explicit, implement HDR10/PQ, HDR10/HLG, and
   extended-linear shader output, select Android HDR swapchains, submit static
   HDR metadata, and validate 10-bit goldens plus a real HDR device lifecycle.
5. [x] Add the shared OpenGL ES 3.x renderer and Android EGL/window adapter
   defined in `MOBILE.md`, sharing color/geometry semantics with Vulkan where
   practical without hiding incompatible API lifecycles.
6. [ ] Add the application/platform renderer selector. Validate
   Vulkan-unavailable, initial-failure, fatal-runtime-failure, recoverable
   same-API recreation, one-way fallback, and both-backends-unavailable cases.
7. [ ] Add AAudio output and device-clock/latency validation.
8. [ ] Add MediaCodec hardware decode and direct-surface presentation first,
   then add the confirmed `AImageReader`/`AHardwareBuffer` Vulkan and
   `SurfaceTexture` external-OES OpenGL ES zero-CPU-copy texture paths only
   after presentation, drop, flush, and surface recreation semantics are
   deterministic.

### Shared Android/OHOS mobile design checkpoint

Complete this checkpoint once and reuse it for both mobile production paths:

1. [x] Define a platform-neutral Vulkan renderer engine for software-frame
   upload, YUV/RGB conversion, range/matrix/transfer/primaries handling,
   viewport, aspect ratio, rotation, synchronization, and retained in-flight
   resources.
2. [x] Keep Android `ANativeWindow`/EGL/Vulkan objects and OHOS
   `OHNativeWindow`/XComponent/EGL/Vulkan objects in separate platform or
   backend-specific adapters; do not expose either SDK through core public
   headers or merge the two SDK lifecycles into one platform class.
3. [x] Reuse shader inputs, color-conversion math, geometry generation,
   staging/upload helpers, capability rules, golden test vectors, and
   renderer contract tests between Android and OHOS. Share OpenGL ES code only
   where the API and resource-lifetime behavior actually match.
4. [x] Define a surface-backed hardware-decode presentation contract shared
   by MediaCodec and FFmpeg 8 OHCodec: explicit decoder selection, frame
   present/drop, playback-clock scheduling, bounded outstanding buffers,
   seek/flush, stop, media replacement, surface loss/recreation, and retained
   frame lifetime.
5. [x] Preserve separate targets for hardware decode, hardware-frame interop,
   rendering, and audio output. Android uses MediaCodec/AAudio; OHOS uses
   OHCodec/OHAudio. Shared code must not introduce a lowest-common-denominator
   platform ABI.
6. [x] Create reusable device-test media and lifecycle scenarios, plus thin
   platform-specific APK/HAP launch, signing, deployment, log collection, and
   result adapters for connected-device validation from macOS.
7. [x] Define Vulkan as the preferred Android/OHOS software renderer and
   OpenGL ES/EGL as its required fallback. Keep recoverable surface recreation
   within the active API, switch one-way to OpenGL ES after fatal Vulkan
   failure, keep playback alive if both renderers fail, and leave selection in
   the application/platform layer rather than core.
8. [x] Define zero-CPU-copy native-buffer interop for both Vulkan and OpenGL ES:
   no CPU map, software-frame transfer, staging copy, or re-upload; explicit
   native-buffer lifetime and fence synchronization; capability-gated format
   support; and independent decoder, interop, and renderer fallback policies.

Accepted design and Android foundation checkpoint:

- [`MOBILE.md`](MOBILE.md) fixes the shared Vulkan engine, the OpenGL ES
  fallback engine and selection/recovery policy, separate Android/OHOS
  Vulkan/EGL surface adapters, direct-surface hardware-output state machine,
  Vulkan/OpenGL ES zero-CPU-copy interop contract, audio boundaries, reusable
  lifecycle scenarios, and on-device installation authorization gate;
- the Android harness pins FFmpeg 8.1.2 by checksum, NDK r28c, API 28,
  compile SDK 36, SDK CMake 4.1.2, and build-tools 37.0.0 while allowing
  explicit installed-tool overrides;
- its minimal LGPL-compatible FFmpeg configuration enables AVI, MPEG-4 video,
  PCM S16 audio, file I/O, libswscale, and libswresample, then statically links
  QtAVCore into one NativeActivity shared library without Qt;
- AAPT2, zipalign, Android Studio's JBR, and apksigner produce a debug APK
  without a Gradle dependency; generated sources, libraries, media, keys,
  package output, logs, and device facts remain under `build/android/`;
- deployment requires exactly one authorized device, runs installation once,
  and stops for manual device confirmation on failure rather than retrying or
  bypassing a modern Android/OHOS authorization prompt;
- the first connected device is model `2410DPN6CC`, Android 16/API 36,
  `arm64-v8a`, Adreno 830, Vulkan 1.3.284, and OpenGL ES 3.2; generated
  software playback now passes with 180 video and 282 audio frames plus one
  background/foreground surface recreation.

Completed Android Vulkan implementation checkpoint:

- `QtAV::RenderVulkan` is now a platform-neutral backend target with borrowed
  physical/logical device and queue handles plus an application-supplied
  current-image target; Vulkan declarations remain in its backend-specific
  header and do not reach core public headers;
- the engine submission path packs YUV420/422/444, NV12/NV21, P010,
  RGB/BGR/RGBA/BGRA/ARGB, or Gray8 software planes into a coherent storage
  buffer and applies range, matrix, transfer, primaries, viewport, aspect, and
  rotation logic in generated SPIR-V shaders; the current-target contract now
  carries `VkColorSpaceKHR` and the shader emits SDR sRGB, native HDR10/PQ,
  native HDR10/HLG, extended-linear sRGB, or linear BT.2020 as requested;
- the engine uses a bounded three-frame in-flight ring and retains each source
  `VideoFrame` until its slot fence completes;
- `QtAV::RenderVulkanAndroid` retains the active `ANativeWindow` generation
  and owns only its Android `VkSurfaceKHR`, swapchain, image views, and
  per-frame acquire/present semaphores while borrowing the
  application-created Vulkan instance, device, and graphics/present queue; it
  exposes prefer-HDR, require-HDR, and SDR-only policies, reports its selected
  surface format/HDR state, and submits frame-derived `VK_EXT_hdr_metadata`
  when that borrowed-device extension was enabled;
- the NativeActivity harness now selects a presentation-capable queue, creates
  the borrowed Vulkan context, renders decoded YUV420P frames through the
  Android adapter, and passes on the recorded Adreno 830 device with 180/180
  frames submitted and presented plus 282 decoded audio frames;
- the deployment test backgrounds and resumes the same NativeActivity,
  invalidates and rebuilds the active window/swapchain generation, and
  continues playback without reopening media;
- platform-neutral offscreen image readback checks exercise six queued
  submissions across the three-frame ring, Fit letterboxing, custom viewport,
  180-degree rotation, target-generation replacement, limited/full-range
  YUV, BT.601/BT.709 matrices, and numeric P010/BT.2020 PQ/HLG
  HDR-input-to-SDR goldens, then switch to packed 10-bit targets to verify
  native HDR10/PQ, HLG-to-PQ, and native HDR10/HLG code values, plus FP16
  targets to verify extended-linear sRGB and linear BT.2020 values above the
  100-nit reference white;
- HDR checks verify mastering-display maximum luminance takes precedence over
  MaxCLL, MaxCLL is used when mastering luminance is absent, and HDR input
  without either uses the renderer's documented 1000-nit default;
- the NativeActivity enables `VK_EXT_swapchain_colorspace`, enables
  `VK_EXT_hdr_metadata` when exposed, requires an implemented HDR
  format/color-space pair, and passes on the recorded Adreno 830 with an
  `A2B10G10R10`/HDR10-ST2084 swapchain, Android compositor HDR-layer
  recognition, presentation of a synthetic P010/BT.2020/PQ frame carrying
  mastering/MaxCLL metadata, and surface recreation;
- a standalone Android install exports `QtAV::RenderVulkan` and
  `QtAV::RenderVulkanAndroid` with their installed headers and Vulkan
  dependency metadata without embedding the local NDK or build-tree path; an
  external Android consumer compiles the installed HDR preference and
  selected-surface query API.

Completed Android OpenGL ES fallback checkpoint:

- `QtAV::RenderOpenGL` is a reusable OpenGL ES 3.x `VideoRenderAPI` target
  that draws into a caller-supplied current framebuffer and keeps EGL/window
  ownership outside the engine;
- software uploads cover YUV420/422/444, NV12/NV21, little-endian P010,
  RGB/BGR/RGBA/BGRA/ARGB, and Gray8, with structured range, matrix, transfer,
  and primaries handling plus the common Fit/Fill/Stretch, custom viewport,
  and right-angle rotation contract;
- the fallback output is explicitly SDR sRGB-coded; PQ/HLG inputs use the
  deterministic HDR-to-SDR shoulder and native HDR EGL output is not claimed;
- `QtAV::RenderOpenGLAndroid` retains the active `ANativeWindow` generation
  and owns its EGL display, OpenGL ES 3.x context, window surface, and swap
  while keeping Android/EGL declarations out of core public headers;
- Android offscreen readback covers every advertised software family,
  viewport, rotation, target-generation replacement, and P010/PQ-to-SDR;
  the real window adapter also presents that fallback frame after the Vulkan
  HDR playback and background/foreground lifecycle checks complete;
- the engine and Android adapter do not implement automatic API selection.
  Startup probing, bounded recovery, fatal one-way fallback, and the
  no-renderer state remain the next renderer-selector slice.

Completed Metal software-frame checkpoint:

- `QtAV::RenderMetal` is Apple-only, optional under
  `QTAV_RENDER_METAL=AUTO/ON/OFF`, and keeps Objective-C++ and Apple framework
  types inside its backend target and backend-specific public header;
- `BorrowedMetalDevice` and `BorrowedMetalCommandQueue` make native resource
  roles explicit, while `MetalCurrentTargetCallback` obtains the current
  application-owned texture or drawable for each render;
- the renderer uploads decoded YUV420/422/444, NV12/NV21, little-endian P010,
  RGB/BGR/RGBA/BGRA/ARGB, and Gray8 planes into a command-buffer-retained Metal
  buffer and performs SDR conversion in a Metal fragment shader;
- Fit, Fill, Stretch, custom viewports, all right-angle rotations, resize,
  surface-loss reporting, drawable presentation, and redraw notification are
  implemented;
- deterministic offscreen GPU readback tests exercise RGB24, YUV420P, and NV12
  decoding plus viewport, aspect, rotation, resize, surface loss, and redraw.

Completed CoreAudio checkpoint:

- `QtAV::AudioCoreAudio` is macOS-only, optional under
  `QTAV_AUDIO_COREAUDIO=AUTO/ON/OFF`, and keeps AudioToolbox/CoreAudio types
  inside its backend target and backend-specific public header;
- `CoreAudioDevice` strongly identifies a non-owning `AudioDeviceID`; an empty
  device selection follows the default output device;
- the sink negotiates interleaved Float32 mono/stereo PCM at the output
  device's nominal sample rate, allowing `QtAV::AudioResample` to convert the
  decoded format;
- a bounded AudioQueue buffer pool implements playback pacing, pause, flush,
  and natural-end drain while keeping accepted PCM lifetime independent of
  decoded frames;
- AudioQueue sample time is anchored to media timestamps and supplies the
  `Player` device-master clock; reported latency combines queued media and HAL
  device, safety-offset, and I/O-buffer frames;
- the console example uses CoreAudio and libswresample automatically on
  macOS, and the native playback path was exercised with generated MPEG-4/AAC
  media;
- a silent device test covers capability reporting, format negotiation,
  pause/resume, queued playback, clock bounds, drain, flush, and close. It
  skips only the device portion when a headless runner cannot create an output
  queue.

Completed VideoToolbox hardware-decode checkpoint:

1. [x] Add `qtav_hw_videotoolbox` as an optional FFmpeg hardware-decoder
   selection path.
2. [x] Attach reference-counted `CVPixelBuffer` storage to `HardwareFrame`
   without exposing Apple or FFmpeg types through core headers.
3. [x] Keep software decode as an explicit fallback when device creation or
   pixel-format negotiation fails.
4. [x] Add lifecycle tests for seek, media replacement, stop, and shutdown.
5. [x] Add `qtav_interop_cvmetal` only after VideoToolbox hardware-frame
   lifetime is stable.

VideoToolbox implementation notes:

- `HardwareDecodeConfig` selects a generic FFmpeg hardware device without
  exposing FFmpeg types; changing it while media is open asynchronously
  reopens the decode path;
- `QtAV::HWVideoToolbox` supplies the Apple-specific configuration helper and
  borrowed `CVPixelBufferRef` accessor, while Core public headers remain free
  of Apple SDK types;
- codec hardware capabilities, device creation, and hardware pixel-format
  negotiation are checked independently, with a caller-controlled software
  fallback and distinct fallback/error media events;
- a decoded hardware `VideoFrame` exposes no fake software planes; its
  `HardwareFrame` retains the FFmpeg frame/CVPixelBuffer, reports the
  underlying software format, and supports read mapping through
  `av_hwframe_transfer_data`;
- H.264 integration tests exercise native hardware output, CPU mapping,
  explicit fallback policy, seek, media replacement, stop, and retained frame
  lifetime after player shutdown.

Completed CVMetal interop checkpoint:

- `QtAV::InteropCVMetal` is Apple-only, optional under
  `QTAV_INTEROP_CVMETAL=AUTO/ON/OFF`, and depends on the Metal renderer without
  coupling VideoToolbox decode to rendering;
- `CVMetalFrameInterop` owns a `CVMetalTextureCache` for a borrowed device and
  imports limited/full-range bi-planar NV12/P010 `CVPixelBuffer` planes as
  retained R/RG Metal texture views without calling the CPU mapping path;
- `MetalVideoRenderer` accepts an optional backend-specific hardware interop,
  advertises its source hardware device, selects a texture-sampling shader for
  hardware frames, and retains each imported frame until its command buffer
  completes;
- deterministic tests verify direct plane import, zero CPU mapping, texture
  and source lifetime, capability reporting, and actual
  VideoToolbox-to-CVMetal-to-Metal H.264 rendering.

Completed HDR and color-space checkpoint:

- `VideoFrame` exposes toolkit-independent structured range, primaries,
  transfer, matrix, and chroma-location values while retaining the diagnostic
  `colorSpace()` string;
- HDR10 mastering-display chromaticities/luminance and content-light levels
  are copied from FFmpeg frame side data into reference-counted public values;
- CVMetal imports limited- and full-range NV12/P010 pixel buffers and reports
  their native range to the renderer;
- Metal selects BT.601, BT.709, or BT.2020 YUV conversion, handles full versus
  limited code values, and processes PQ/HLG transfer plus BT.2020/Display-P3
  source primaries;
- `MetalRenderTarget` can return an application-owned `CAMetalLayer` for
  renderer configuration before `nextDrawable`: `RGBA16Float`,
  extended-linear BT.2020, `wantsExtendedDynamicRangeContent`, and
  frame-derived HDR10/HLG `CAEDRMetadata`;
- extended-linear BT.2020 output preserves BT.2020 source primaries and linear
  HDR brightness above `1.0`; the older extended-linear sRGB mode remains an
  explicit narrower-gamut option;
- system tone mapping and shader-based display-adaptive tone mapping are
  explicit modes. The adaptive path samples live `NSScreen`/`UIScreen`
  headroom for every frame and avoids double tone mapping;
- deterministic FP16 readback verifies HDR pixels above `1.0`, BT.2020 gamut
  preservation, and changing 2x/4x headroom; a macOS onscreen test presents
  through a real EDR display and skips when live EDR headroom is unavailable;
- structured HDR10 side-data lifetime and full-range CVMetal import remain
  covered.

Next active implementation order:

1. [x] Add backend-specific Windows headers for strong borrowed D3D11 types.
2. [x] Add `qtav_render_d3d11` and render software frames first.
3. [x] Add resize, viewport, aspect-ratio, rotation, surface recreation, and
   device-loss tests.
4. [x] Add WASAPI.
5. [x] Complete the D3D11VA device, frame-lifetime, and interop design
   checkpoint below.
6. [x] Add D3D11VA decode and D3D11 zero-copy interop.

Completed D3D11 software-frame checkpoint:

- `QtAV::RenderD3D11` is Windows-only, optional under
  `QTAV_RENDER_D3D11=AUTO/ON/OFF`, and keeps D3D11/DXGI/WRL types and headers
  inside its backend target and backend-specific public header;
- `BorrowedD3D11Device` and `BorrowedD3D11DeviceContext` make native resource
  roles explicit, while `D3D11CurrentTargetCallback` obtains the current
  application-owned render-target view for every frame;
- the renderer uploads YUV420/422/444, NV12/NV21, P010,
  RGB/BGR/RGBA/BGRA/ARGB, and Gray8 software frames into D3D11 shader
  resources and applies limited/full-range BT.601/BT.709/BT.2020 conversion,
  PQ/HLG EOTF, linear primaries conversion, and display-aware tone mapping;
- BGRA8/RGBA8 SDR, FP16 scRGB, and RGB10/PQ targets are supported; an optional
  borrowed `IDXGISwapChain3` enables per-frame `IDXGIOutput6` capability
  discovery, SDR-white lookup, `SetColorSpace1`, and display-switch handling;
- Fit, Fill, Stretch, custom viewports, all right-angle rotations, resize,
  render-target recreation, foreign-device rejection, and missing-surface or
  device-removal event classification are implemented;
- deterministic WARP offscreen tests cover RGB24, YUV420P, NV12, PQ/HLG P010,
  SDR tone mapping, FP16 scRGB/RGB10 numeric output, viewport, aspect,
  rotation, resize, surface recreation, and error handling; a native
  flip-model test covers the current output and same-adapter display moves;
- Windows multi-config discovery now maps vcpkg FFmpeg Debug and Release
  libraries correctly, and a common runtime directory makes shared-library
  tests and examples directly runnable.

Completed WASAPI checkpoint:

- `QtAV::AudioWASAPI` is Windows-only, optional under
  `QTAV_AUDIO_WASAPI=AUTO/ON/OFF`, installable as an exported package target,
  and keeps Windows SDK, COM, and WASAPI types out of core public headers;
- `WasapiEndpointId` owns an optional endpoint identifier so the sink can
  select a device without transferring an apartment-bound `IMMDevice`; an
  empty identifier follows the current default multimedia render endpoint;
- the sink negotiates shared-mode interleaved Float32 mono/stereo PCM at the
  endpoint mix rate and uses the injected `QtAV::AudioResample` backend for
  decoded planar, sample-rate, or channel-layout conversion;
- a dedicated MMCSS thread owns COM and the event-driven audio client while a
  bounded backend queue copies accepted PCM independently of decoded-frame
  lifetime;
- pause/resume, seek flush, natural-end drain, endpoint invalidation, bounded
  backpressure, underrun recovery, and close are synchronized without calling
  native interfaces from `Player::position()`;
- cached `IAudioClock` position is anchored to media timestamps, reported
  latency combines engine padding and stream latency, and an underrun
  invalidates the device clock until the next accepted buffer re-anchors it;
- device integration tests cover capabilities, invalid input, shared-mode
  format negotiation, pause/resume, write, clock/latency, drain, flush,
  re-anchor, and close; the Windows console example exercises
  `Player`/libswresample/WASAPI playback with generated MPEG-4/AAC media.

D3D11VA and zero-copy design checkpoint:

Completed after WASAPI and before implementing `qtav_hw_d3d11va` or
`qtav_interop_d3d11`. The accepted contract and implementation order are in
[`D3D11VA.md`](D3D11VA.md).

1. [x] Decide how the hardware decoder receives the application-selected
   D3D11 device. Prefer decoding on the same borrowed device used by the
   renderer so the normal path does not require a cross-device copy.
2. [x] Define backend-specific retained-frame access to both the
   `ID3D11Texture2D` and its array slice while keeping D3D11 and FFmpeg types
   out of core public headers.
3. [x] Define device-context locking, playback-worker access, render-thread
   access, and frame-pool lifetime using the FFmpeg 8 D3D11VA device and
   frames-context contracts.
4. [x] Keep hardware decode, D3D11 texture interop, D3D11 Video Processor
   operations, and final rendering as separate responsibilities and targets
   where the existing module boundaries require them.
5. [x] Specify explicit software-map/copy fallback, foreign-device rejection,
   seek and flush behavior, device removal, surface recreation, and retained
   frame lifetime after player shutdown.
6. [x] Define deterministic WARP coverage for contracts and error paths plus
   real-GPU integration coverage for hardware decode and zero-copy texture
   rendering.
7. [x] Treat Aleksoid1978/VideoRenderer as an isolated GPL-3.0 behavioral
   reference only. Do not vendor it, link it, or copy its C++, shaders, data
   tables, or vendor-specific extensions; implement against FFmpeg and
   Microsoft public APIs.

Accepted design summary:

- a shared `D3D11DeviceAccess` object retains the selected device/immediate
  context and supplies the recursive lock used by FFmpeg, interop, and
  rendering;
- an opaque core hardware-device token lets the D3D11VA backend supply a
  referenced FFmpeg device context without exposing FFmpeg or Windows types in
  core installed headers;
- the backend-specific retained frame view carries the decoder
  `ID3D11Texture2D`, array slice, source-device identity, and source
  `HardwareFrame`;
- because decoder texture arrays cannot be shader-resource views, the initial
  zero-CPU-copy path consumes the decoder slice through the D3D11 Video
  Processor and returns a same-device shader-readable intermediate to the
  renderer;
- hardware-decode fallback and renderer software-map fallback are separate
  explicit policies; foreign devices are rejected before context access and
  device removal requires application-led device recreation;
- WARP covers deterministic API/lifetime/error contracts while opt-in
  real-GPU tests cover native decode and zero-CPU-copy rendering.

Next implementation slice:

1. [x] Add the common Windows D3D11 device-access target and shared recursive
   context guard.
2. [x] Add the opaque supplied-hardware-device token and private FFmpeg bridge
   in core.
3. [x] Prove device identity, COM lifetime, locking, and install/export
   behavior with deterministic tests before opening the native decoder.

Completed Windows D3D11 device-access checkpoint:

- `QtAV::PlatformWindows` verifies that a selected context is the chosen
  device's immediate context, retains both COM interfaces, and exports
  `D3D11DeviceAccess` plus a move-only recursive `D3D11ContextGuard`;
- the D3D11 renderer accepts shared device access while preserving its
  borrowed device/context convenience constructor, and it holds the common
  guard for immediate-context rendering calls;
- deterministic WARP tests cover null, foreign, and deferred-context
  rejection, retained COM lifetime, same-thread recursion, cross-thread
  exclusion, guard lifetime, and renderer participation in the shared lock;
- static/shared builds and installed external consumption of
  `QtAV::PlatformWindows` and `QtAV::RenderD3D11` pass.

Completed supplied hardware-device bridge checkpoint:

- public `HardwareDecodeDevice` is a cheap, PIMPL-backed reference-counted
  token that exposes only `HardwareDeviceType` and an opaque native identity;
- `HardwareDecodeConfig` optionally carries that token while preserving the
  existing FFmpeg-created-device path when no token is supplied;
- an uninstalled core bridge retains and returns referenced FFmpeg
  `AVHWDeviceContext` buffers for in-tree hardware backends without placing
  FFmpeg declarations in installed headers;
- `Player` takes its own device-context reference before decoder open, rejects
  a token/requested-type mismatch through the existing fallback policy, and
  asynchronously reopens loaded media when the supplied token changes;
- deterministic tests cover invalid construction, reference ownership, copy
  identity, independent tokens, player config copying, and type-mismatch
  software fallback plus disabled-fallback failure.

Completed D3D11VA hardware-decode checkpoint:

- `QtAV::HWD3D11VA` is Windows-only, optional under
  `QTAV_HW_D3D11VA=AUTO/ON/OFF`, installable as an exported package target,
  and depends on `QtAV::PlatformWindows` without depending on the renderer;
- `d3d11vaHardwareDecodeConfig()` allocates FFmpeg's D3D11VA device on the
  application-selected retained device and immediate context, installs
  callbacks using the shared recursive context lock, and requests a bounded
  zero-to-64 extra decoder surfaces with a default of four;
- a required-supplied-device flag prevents a failed selected-device setup from
  silently opening a different FFmpeg-created device while preserving the
  explicit software-fallback policy;
- core `NativeHandle` carries the D3D11 decoder texture-array slice without
  exposing D3D11 or FFmpeg types, while the Windows-only `D3D11VAFrame`
  validates and retains NV12/P010 texture, slice, dimensions, and device;
- deterministic tests cover device/context identity, shared-lock exclusion,
  option bounds, native frame validation, invalid slice/format/size/type, and
  retained synthetic texture lifetime;
- the current Windows adapter passes generated H.264 native decode, CPU
  mapping, seek, media replacement, stop, and retained frame access after
  player shutdown.

Next implementation slice:

1. [x] Add decoder-independent `D3D11HardwareFrameInterop` and retained
   `D3D11TextureFrame` interfaces to `QtAV::RenderD3D11`.
2. [x] Add renderer capability reporting plus explicit enabled/disabled
   software-map fallback using mock interop tests.
3. [x] Implement `QtAV::InteropD3D11` with same-device validation and a D3D11
   Video Processor pass into shader-readable SDR BGRA8 or HDR RGB10/FP16
   intermediates.
4. [x] Add WARP contract tests, native zero-CPU-copy H.264 rendering coverage,
   example wiring, and install-consumer validation.

Completed D3D11 renderer interop-contract checkpoint:

- `QtAV::RenderD3D11` exposes decoder-independent
  `D3D11HardwareFrameInterop` and `D3D11TextureFrame` interfaces without
  depending on `QtAV::HWD3D11VA`;
- an interop object identifies the retained `D3D11DeviceAccess` whose shared
  recursive guard protects its immediate/video-context work;
- an imported texture frame reports its dimensions, packed pixel format, DXGI
  format, and color space and keeps its borrowed `ID3D11Texture2D` and
  `ID3D11ShaderResourceView` valid for the texture-frame lifetime;
- deterministic WARP coverage proves capability/source-device reporting,
  import dispatch, shared device-access identity, and COM resource retention
  after the original texture and view references are released.

Completed D3D11 renderer interop-consumption checkpoint:

- `D3D11VideoRenderer` advertises hardware devices only while a compatible
  interop object using the same retained `D3D11DeviceAccess` is installed;
- imported SDR BGRA8/RGBA8, FP16 scRGB, or RGB10/PQ shader-readable texture
  frames feed the final viewport, aspect-ratio, rotation, and color pass
  without CPU mapping;
- software mapping is disabled by default and can be explicitly enabled
  independently of decoder fallback; successful use emits an observable
  detail event, while disabled or failed mapping makes rendering fail;
- mock WARP tests cover direct import with zero map calls, capability changes,
  enabled/disabled mapping fallback, mapped pixel output, and mapping failure.

Completed D3D11 Video Processor interop checkpoint:

- `QtAV::InteropD3D11` validates D3D11VA NV12/P010 texture-array slices,
  exact source/target device identity, device health, format support, and
  dimensions before entering the shared recursive context guard;
- `D3D11FrameInterop` caches the Video Processor enumerator/processor and
  returns a retained per-import SDR BGRA8, FP16 scRGB, or RGB10/PQ texture plus
  shader-resource view without CPU mapping or a cross-device copy;
- the renderer passes structured range/matrix/transfer/chroma metadata through
  a backward-compatible color-aware interop overload; Direct3D 11.1 color
  spaces preserve PQ/BT.2020 as RGB10/PQ (or FP16 scRGB) and HLG/BT.2020 as
  FP16 scRGB (or RGB10/PQ), with legacy SDR BT.601/709 fallback;
- WARP covers texture-array/slice extraction, retained lifetime, recursive
  locking, and safe Video Processor unavailability while mock WARP tests cover
  renderer consumption and error contracts;
- the current hardware adapter renders generated H.264/NV12 and PQ/BT.2020
  HEVC Main10/P010 D3D11VA frames with zero map calls, verified red/blue pixel
  readback, and FP16 scRGB values above `1.0`; the H.264 path also covers
  pause/resume, seek, media replacement, explicit stop, surface recreation,
  and retained source/import lifetime after `Player` shutdown;
- the console example wires D3D11VA, `QtAV::InteropD3D11`, and offscreen D3D11
  rendering; the strict H.264/AAC test passes through an active WASAPI render
  endpoint and still makes unavailable endpoint coverage an explicit skip;
  the installed CMake package exports `QtAV::InteropD3D11`.

Following platform slice:

1. [~] Begin the Android production path with the shared Android/OHOS mobile
   design checkpoint and connected-device build harness.

Platform implementation order after the contracts are stable:

1. Apple reference path on the current macOS host.
2. Windows reference path.
3. Android production path from macOS with connected-device validation.
4. OHOS production path from macOS with connected-device validation.
5. Linux production path, beginning in WSL and moving to native Linux at the
   mandatory environment gate defined in Milestone 8.

## Milestone 3 — Portable reference backends

- [x] CPU video-frame conversion using libswscale.
- [x] CPU renderer or image-buffer target for deterministic tests.
- [x] Audio conversion/resampling using libswresample.
- [x] Null/mock audio sink with deterministic latency.
- [x] Optional file/PCM diagnostic sink.
- [x] Pixel-format and channel-layout conversion tests.
- [x] A/V clock tests with simulated device latency.

Acceptance:

- renderer and audio contracts are proven without a platform SDK;
- deterministic golden tests cover format conversion and timing.

Status: complete and verified.

## Milestone 4 — Apple production path

### Metal

- [x] `qtav_render_metal` target using Objective-C++ only inside the backend.
- [x] Borrowed `MTLDevice`, command queue, and current-target callback.
- [x] NV12/P010/YUV/RGB upload and shader conversion.
- [x] Resize, viewport, aspect ratio, rotation, and redraw.
- [x] HDR metadata and color-space plumbing after the SDR path is stable.
- [x] Complete `CAMetalLayer` EDR configuration and extended-linear BT.2020
  output with HDR10/HLG `CAEDRMetadata`.
- [x] Adapt to live macOS/iOS EDR headroom and validate HDR FP16 pixels plus a
  conditional real-screen EDR presentation path.

### Audio and hardware decode

- [x] `qtav_audio_coreaudio`.
- [x] Device format negotiation and latency/clock reporting.
- [x] `qtav_hw_videotoolbox`.
- [x] `qtav_interop_cvmetal` for `CVPixelBuffer`/Metal zero copy.

Acceptance:

- macOS native example plays A/V without Qt;
- software and VideoToolbox decode both work;
- renderer survives resize, pause, seek, media replacement, and shutdown;
- no Apple type leaks into core public headers.

## Milestone 5 — Windows production path

### D3D11

- [x] `qtav_render_d3d11`.
- [x] Borrowed `ID3D11Device`, context, and render target.
- [x] Software-frame texture upload.
- [x] Resize, viewport, aspect ratio, rotation, and redraw.
- [x] Windows Advanced Color SDR, FP16 scRGB, and RGB10/PQ output.
- [x] Per-frame display/HDR-state switching, SDR reference white, PQ/HLG,
  primaries conversion, and display-aware tone mapping.

### Audio and hardware decode

- [x] `qtav_audio_wasapi`.
- [x] Shared-mode PCM negotiation and audio clock.
- [x] Complete the D3D11VA device/frame/interop design checkpoint.
- [x] `qtav_hw_d3d11va`.
- [x] `qtav_interop_d3d11` for zero-copy decoder textures.

Acceptance:

- [x] Windows native example plays A/V without Qt. Generated H.264/AAC proves
  D3D11VA decode, zero-copy rendering, audio decode, and audible WASAPI output
  together.
- [x] Software and D3D11 hardware decode both work.
- [x] Device-loss and surface-recreation paths are tested.
- [x] Active-HDR native display validation with the Windows HDR setting
  enabled, including HDR numeric readback and HDR-disabled native
  swap-chain/display-switch tests.
- [x] No Windows type leaks into core public headers.

Status: complete; resume Milestone 6 from its first unchecked item.

## Milestone 6 — Android production path

### Toolchain and application shell

- [x] Reproducible macOS-hosted Android NDK build for QtAVCore and the required
  FFmpeg 8+ libraries, initially targeting arm64.
- [x] Minimal APK/native application shell that owns activity, lifecycle,
  permissions, and current rendering surfaces without adding Android types to
  core. NativeActivity creation/destruction, packaged media, active-window
  retain/release, background/foreground, and replacement-surface scenarios
  are proven.
- [x] Connected-device deployment, logging, generated-media playback, and
  automated result collection.

### Rendering

- [x] Shared Vulkan renderer engine from the mobile design checkpoint.
- [x] Android Vulkan surface/swapchain adapter using application-owned native
  resources.
- [x] Native Vulkan HDR target contract and Android HDR swapchain selection,
  including HDR10/PQ, HDR10/HLG, extended-linear output, static metadata,
  deterministic 10-bit goldens, and connected-device lifecycle validation.
- [x] Shared OpenGL ES 3.x renderer plus Android EGL/window adapter as the
  required Vulkan fallback, without an SDL3 dependency.
- [ ] Application/platform renderer selector implementing the accepted
  startup, recovery, fatal-error, one-way fallback, and no-renderer behavior.
- [x] Software YUV/NV12/P010/RGB upload, structured color conversion, viewport,
  aspect ratio, rotation, resize, redraw, and surface recreation.

### Audio and hardware decode

- [ ] `qtav_audio_aaudio`, with OpenSL fallback only if the selected minimum
  Android API or real-device coverage requires it.
- [ ] Device format negotiation, bounded callback-safe buffering, device clock,
  latency, pause, flush, drain, route change, and disconnect handling.
- [ ] `qtav_hw_mediacodec` with explicit wrapper-decoder selection and
  application-supplied surface/device lifetime.
- [ ] Direct-surface presentation with explicit present/drop behavior before
  texture interop.
- [ ] Android Vulkan interop using an application-owned private,
  GPU-sampled `AImageReader`: supply its `ANativeWindow` to MediaCodec,
  correlate codec and acquired-image timestamps, import the retained
  `AHardwareBuffer` through
  `VK_ANDROID_external_memory_android_hardware_buffer`, apply
  YCbCr/external-format capability checks, bridge acquire/release fences, and
  return the release fence through asynchronous `AImage` deletion without
  `AHardwareBuffer_lock*()` or a staging upload.
- [ ] Android MediaCodec OpenGL ES interop using a `SurfaceTexture` producer
  and `GL_TEXTURE_EXTERNAL_OES` as the primary path, with explicit
  timestamp/generation and current-image lifetime handling. Keep private
  `AImageReader` plus `AHardwareBuffer`/`EGLImage` import as a
  capability-gated alternative; neither path may map or re-upload decoded
  pixels.
- [ ] On Vulkan-to-OpenGL ES renderer fallback, attempt compatible GLES native
  interop for subsequent frames; otherwise follow an explicit direct-surface,
  software-decode, or no-video policy without implicit hardware-frame mapping.
- [ ] Software fallback independent of renderer mapping/interop fallback.

Acceptance:

- a macOS-hosted build installs and runs on at least one connected arm64
  Android device;
- software decode prefers Vulkan, falls back to OpenGL ES for unavailable or
  fatally failed Vulkan, and AAudio produces synchronized audible output;
- connected-device or deterministic adapter tests cover initial fallback,
  recoverable Vulkan recreation without an API switch, fatal one-way fallback
  without media reopen, and the both-renderers-unavailable error path;
- MediaCodec H.264 and HEVC paths cover pause/resume, seek, media replacement,
  stop, background/foreground transition, surface recreation, and shutdown;
- direct-surface hardware output is verified before any texture-interoperable
  path is described as complete;
- on capable devices, H.264 and HEVC native frames render through both Vulkan
  and OpenGL ES with zero CPU map/transfer/upload calls, correct fence ordering,
  bounded retained buffers, color/format validation, and lifecycle coverage;
- pixel-validation tests may read back the final render target, but decoded
  source map, transfer, staging-copy, and re-upload counters remain zero;
- an unsupported Vulkan or OpenGL ES import capability is reported explicitly
  as unavailable or skipped, not counted as zero-CPU-copy success;
- Android SDK types remain outside core public headers.

## Milestone 7 — OHOS production path

Target clarification gate:

- [ ] Record whether the initial target is a HarmonyOS NEXT commercial device
  application, a specific OpenHarmony distribution/device, or both; record the
  SDK/API version, signing requirements, available system capabilities, and
  connected-device workflow before fixing backend availability rules.

### Toolchain and application shell

- [ ] Reproducible macOS-hosted OHOS native build for QtAVCore and FFmpeg 8+,
  initially targeting arm64.
- [ ] Add `modern/platform/ohos/` for small shared OHOS helpers while keeping
  media, graphics, and audio implementations in their responsibility-specific
  backend targets.
- [ ] Minimal HAP/native application shell using ArkUI/XComponent only at the
  integration boundary.
- [ ] Connected-device deployment, signing, logging, generated-media playback,
  and automated result collection through thin OHOS-specific adapters to the
  shared mobile test scenarios.

### Vulkan and OpenGL ES rendering

- [ ] Reuse the Android-proven platform-neutral Vulkan renderer engine,
  shaders, color conversion, geometry, synchronization rules, golden vectors,
  and renderer contract tests.
- [ ] Add the OHOS `OHNativeWindow`/XComponent Vulkan surface and swapchain
  adapter as a separate target or platform helper.
- [ ] Reuse OpenGL ES renderer internals where compatible, with a separate OHOS
  EGL/window adapter and explicit capability checks.
- [ ] Reuse the Android-proven renderer selector and one-way Vulkan-to-OpenGL
  ES policy while keeping OHOS window, EGL, and error classification in its
  own adapter.
- [ ] Validate software YUV/NV12/P010/RGB upload, viewport, aspect ratio,
  rotation, resize, redraw, surface loss/recreation, SDR, and supported HDR
  output behavior on a real device.

### Audio and hardware decode

- [ ] Add an OHOS OHAudio sink target with negotiated PCM, bounded callback-safe
  buffering, device clock/latency, pause, flush, drain, route change, and
  disconnect behavior.
- [ ] Add an OHOS OHCodec hardware-decode target using FFmpeg 8
  `AV_HWDEVICE_TYPE_OHCODEC` and explicit H.264/HEVC wrapper-decoder selection.
- [ ] Reuse the shared surface-backed presentation contract for playback-clock
  scheduling, present/drop, outstanding-buffer bounds, flush, stop, and surface
  recreation.
- [ ] Implement direct `OHNativeWindow` presentation first.
- [ ] Add the confirmed OHOS OpenGL ES path: supply the `OHNativeWindow`
  produced by `OH_NativeImage` to OHCodec surface output, update the surface
  image, and sample its bound `GL_TEXTURE_EXTERNAL_OES` texture while
  enforcing the selected SDK's token, generation, and image-lifetime rules.
- [ ] Before claiming OHOS Vulkan zero-CPU-copy interop, add a backend or
  narrowly scoped FFmpeg bridge that exposes and retains the decoded
  `OH_AVBuffer`/`OH_NativeBuffer` through GPU completion. The current FFmpeg 8
  OHCodec buffer branch calls `OH_AVBuffer_GetAddr()` and `av_image_copy2()`
  and is therefore disallowed; its surface branch exposes only present/drop
  tokens, not a Vulkan-importable native buffer.
- [ ] After that bridge exists, add the OHOS Vulkan adapter using the target
  SDK's native-buffer/external-memory path and release the codec output only
  after GPU completion. Record exact format, lifetime, protected-content, and
  fence capabilities on the target device; keep the feature unavailable when
  any required capability is absent.
- [ ] On Vulkan-to-OpenGL ES renderer fallback, attempt compatible GLES native
  interop for subsequent frames; otherwise follow an explicit direct-surface,
  software-decode, or no-video policy without implicit hardware-frame mapping.
- [ ] Keep software decode fallback independent of Vulkan/OpenGL ES interop
  fallback.

Acceptance:

- a macOS-hosted build installs and runs on the recorded connected arm64 OHOS
  target;
- software decode prefers Vulkan, falls back to OpenGL ES for unavailable or
  fatally failed Vulkan, and OHAudio produces synchronized audible output;
- connected-device or deterministic adapter tests cover initial fallback,
  recoverable Vulkan recreation without an API switch, fatal one-way fallback
  without media reopen, and the both-renderers-unavailable error path;
- OHCodec H.264 and HEVC paths cover pause/resume, seek, media replacement,
  stop, background/foreground transition, surface recreation, and shutdown;
- on a capable target, H.264 and HEVC native frames render through both Vulkan
  and OpenGL ES with zero CPU map/transfer/upload calls, correct fence ordering,
  bounded retained buffers, color/format validation, and lifecycle coverage;
- the OHOS Vulkan result specifically proves that the active decode path did
  not call `OH_AVBuffer_GetAddr()` or `av_image_copy2()`; final-render-target
  readback for pixel validation is permitted;
- unsupported native-buffer import capability is reported explicitly as
  unavailable or skipped, not counted as zero-CPU-copy success;
- Android/OHOS share renderer engines and deterministic tests without sharing
  platform SDK types or incorrectly treating their native lifecycles as ABI
  compatible;
- OHOS SDK types remain outside core public headers.

## Milestone 8 — Linux production path

### WSL development and mandatory native-Linux migration gate

- [ ] Begin with WSL/WSLg for Linux cross-platform compilation, all-backends-off
  tests, reusable Vulkan/OpenGL code, deterministic offscreen tests, and
  integration work that does not require authoritative physical Linux device
  behavior.
- [ ] Before starting work whose result depends on native Linux graphics,
  audio, hardware decode, driver, device-loss, or display-server behavior,
  explicitly remind the user that migration to a native Linux installation is
  now required and wait for confirmation of the new environment.
- [ ] Do not mark native Linux rendering, audio, VAAPI, or the milestone
  acceptance complete from WSL-only results. At the latest, migrate before
  real Wayland/X11 presentation, physical ALSA/PulseAudio device validation,
  VAAPI zero-copy validation, or GPU/display driver recovery testing.
- [ ] After migration, record the Linux distribution, compositor/display
  server, GPU, driver, audio stack, and FFmpeg 8+ build used for validation.

### Rendering

- [ ] Reuse the Android/OHOS-proven Vulkan renderer engine, shaders, color
  conversion, geometry, upload helpers, synchronization rules, and tests.
- [ ] Add native Linux Vulkan surface/swapchain adapters without coupling core
  to Wayland or X11.
- [ ] Adapt the shared OpenGL ES renderer internals to the selected native
  Linux OpenGL/EGL path, keeping display-server context and surface ownership
  in Linux-specific code.
- [ ] Validate resize, viewport, aspect ratio, rotation, redraw, surface loss,
  display-server recreation, and SDR/HDR capability reporting on native Linux.

### Audio and hardware decode

- [ ] ALSA and/or PulseAudio sink with negotiated PCM, device clock, latency,
  pause, flush, drain, route/device loss, and recovery.
- [ ] VAAPI hardware decode.
- [ ] VAAPI/Vulkan or VAAPI/OpenGL interop with explicit software mapping
  fallback and no default CPU copy.

Acceptance:

- native Linux software playback produces synchronized A/V without Qt;
- Vulkan and the selected OpenGL path reuse the mobile-proven implementation
  where appropriate while Linux window-system code remains isolated;
- native audio-device timing and VAAPI zero-copy behavior are validated on a
  real Linux installation, not only WSL;
- no Linux, Wayland, X11, ALSA, PulseAudio, or VAAPI type leaks into core public
  headers.

## Milestone 9 — Playback feature parity

- [ ] Active audio/video track switching after load.
- [ ] Subtitle packet decode.
- [ ] Plain-text subtitle callback.
- [ ] libass renderer as an optional module.
- [ ] External audio and subtitle sources.
- [ ] Packet buffering policy and buffering status.
- [ ] Low-latency/live-stream drop policy.
- [ ] Reconnect and recoverable network errors.
- [ ] Frame stepping and accurate seek.
- [ ] Audio time-stretch for playback rate without pitch change.
- [ ] Filter/plugin contracts.

## Milestone 10 — Dolby and HDR scope

- [x] Generic FFmpeg AC-3 software decode.
- [x] Generic FFmpeg E-AC-3 software decode.
- [x] Generic FFmpeg TrueHD software decode.
- [ ] Multichannel PCM device-output validation.
- [ ] IEC 61937 compressed passthrough contract.
- [ ] Windows HDMI/WASAPI passthrough.
- [ ] Apple platform capability investigation.
- [ ] Atmos object-metadata preservation/rendering feasibility.
- [x] HDR10 metadata propagation.
- [ ] Dolby Vision metadata and rendering feasibility.
- [ ] Licensing and certification review before product claims.

Codec decoding must never be described as Dolby certification or Atmos
rendering.

## Release and plugin strategy

- [ ] Keep all modules in this repository while interfaces evolve.
- [ ] Version the core C++ API and CMake package.
- [ ] Add continuous integration for supported host platforms.
- [ ] Define a C ABI only when runtime-loaded plugins become necessary.
- [ ] Split a backend into another repository only when it has an independent
  license, team, release cycle, or closed-source delivery requirement.
