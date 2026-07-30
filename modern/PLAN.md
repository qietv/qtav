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
- the Apple reference path, including HDR and color-space metadata plumbing,
  and the Windows D3D11 software-frame and WASAPI audio paths are complete;
  the D3D11VA device/frame/interop design and supplied-device core bridge are
  complete; the native `qtav_hw_d3d11va` decoder backend is now complete, and
  the decoder-independent D3D11 renderer interop interfaces are complete; the
  active next task is renderer capability reporting and software-map fallback,
  followed by the Video Processor implementation;
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
- `modern/backends/hwaccel/d3d11va/include/qtav/d3d11va_hardware_decoder.h`
- `modern/backends/hwaccel/videotoolbox/include/qtav/videotoolbox_hardware_decoder.h`
- `modern/backends/interop/cvmetal/include/qtav/cvmetal_frame_interop.h`

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
- `modern/backends/hwaccel/d3d11va/src/d3d11va_hardware_decoder.cpp`
- `modern/backends/hwaccel/videotoolbox/src/videotoolbox_hardware_decoder.cpp`
- `modern/backends/interop/cvmetal/src/cvmetal_frame_interop.mm`
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
- `modern/tests/d3d11_video_renderer_test.cpp`
- `modern/tests/d3d11va_hardware_decoder_test.cpp`
- `modern/tests/videotoolbox_hardware_decoder_test.cpp`
- `modern/tests/cvmetal_frame_interop_test.mm`
- `modern/tests/hardware_decode_device_test.cpp`

Current verification:

- static and shared builds pass 27/27 CTest tests on Windows after adding the
  D3D11VA contract and native lifecycle coverage;
- ASan/UBSan passes the prior 24/24 macOS-applicable tests with leak detection
  disabled;
- the all-backends-disabled build passes 11/11 tests, including the Windows
  platform device-access contract test;
- forcing an unimplemented backend to `ON` fails with a clear diagnostic;
- invalid backend option values are rejected;
- installation and external `QtAV::RenderCPU`, `QtAV::RenderMetal`,
  `QtAV::AudioResample`, `QtAV::AudioFile`, plus
  `QtAV::AudioCoreAudio`, `QtAV::HWVideoToolbox`, and
  `QtAV::InteropCVMetal` CMake consumption pass;
- FFmpeg 8 minimum enforcement passes in the source and installed package;
- configuration without `pkg-config` passes;
- runtime linkage contains no Qt;
- core public-header scans contain no Qt, FFmpeg, or platform SDK types;
- MPEG-4/AAC, AC-3, E-AC-3, and TrueHD decode tests pass.
- on Windows with Visual Studio 2026 and vcpkg FFmpeg 8.1.2, the static Release
  build passes 27/27 CTest tests, including the supplied hardware-device
  bridge, deterministic WARP D3D11 rendering, WASAPI device lifecycle,
  Player-driven WASAPI playback, and native H.264 D3D11VA decode with mapping,
  seek, media replacement, stop, and retained-frame shutdown lifetime;
- Windows multi-config FFmpeg imports select matching Debug/Release libraries,
  and project DLLs, tests, and examples share a runnable `bin/<Config>`
  directory;
- installation plus external CMake consumption of `QtAV::PlatformWindows`,
  `QtAV::HWD3D11VA`, `QtAV::RenderD3D11`, and `QtAV::AudioWASAPI` together
  with the portable core, render, and audio targets passes for static and
  shared builds; the installed core token links without installing its private
  FFmpeg bridge header.

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

Continue the Apple reference path on the current macOS host:

1. [x] Add backend-specific Apple headers for strong borrowed native types
   without including them from core public headers.
2. [x] Add the `qtav_render_metal` target and define borrowed `MTLDevice`,
   command-queue, and current-target callback ownership.
3. [x] Render software NV12/YUV/RGB frames through Metal before adding hardware
   decode or zero-copy interop.
4. [x] Add resize, viewport, aspect-ratio, rotation, and redraw tests.
5. [x] Add `qtav_audio_coreaudio` only after the Metal software-frame path is
   stable.

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
- `MetalRenderTarget` can request `ExtendedLinearSRGB` on `RGBA16Float`, with
  configurable reference-white luminance and values above `1.0` retained for
  application-configured EDR output;
- deterministic tests cover structured HDR10 side-data lifetime, the extended
  linear target contract, and full-range CVMetal import.

Next active implementation order:

1. [x] Add backend-specific Windows headers for strong borrowed D3D11 types.
2. [x] Add `qtav_render_d3d11` and render software frames first.
3. [x] Add resize, viewport, aspect-ratio, rotation, surface recreation, and
   device-loss tests.
4. [x] Add WASAPI.
5. [x] Complete the D3D11VA device, frame-lifetime, and interop design
   checkpoint below.
6. [~] D3D11VA decode is complete; add D3D11 zero-copy interop.

Completed D3D11 software-frame checkpoint:

- `QtAV::RenderD3D11` is Windows-only, optional under
  `QTAV_RENDER_D3D11=AUTO/ON/OFF`, and keeps D3D11/DXGI/WRL types and headers
  inside its backend target and backend-specific public header;
- `BorrowedD3D11Device` and `BorrowedD3D11DeviceContext` make native resource
  roles explicit, while `D3D11CurrentTargetCallback` obtains the current
  application-owned render-target view for every frame;
- the renderer uploads YUV420/422/444, NV12/NV21, P010,
  RGB/BGR/RGBA/BGRA/ARGB, and Gray8 software frames into D3D11 shader
  resources and applies limited/full-range BT.601/BT.709/BT.2020 conversion;
- Fit, Fill, Stretch, custom viewports, all right-angle rotations, resize,
  render-target recreation, foreign-device rejection, and missing-surface or
  device-removal event classification are implemented;
- deterministic WARP offscreen tests cover RGB24, YUV420P, and NV12 pixel
  output plus viewport, aspect, rotation, resize, surface recreation, and
  error handling;
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
2. [ ] Add renderer capability reporting plus explicit enabled/disabled
   software-map fallback using mock interop tests.
3. [ ] Implement `QtAV::InteropD3D11` with same-device validation and a D3D11
   Video Processor pass into a shader-readable BGRA8 intermediate.
4. [ ] Add WARP contract tests, native zero-CPU-copy H.264 rendering coverage,
   example wiring, and install-consumer validation.

Completed D3D11 renderer interop-contract checkpoint:

- `QtAV::RenderD3D11` exposes decoder-independent
  `D3D11HardwareFrameInterop` and `D3D11TextureFrame` interfaces without
  depending on `QtAV::HWD3D11VA`;
- an interop object identifies the retained `D3D11DeviceAccess` whose shared
  recursive guard protects its immediate/video-context work;
- an imported texture frame reports its dimensions and packed pixel format
  and keeps its borrowed `ID3D11Texture2D` and
  `ID3D11ShaderResourceView` valid for the texture-frame lifetime;
- deterministic WARP coverage proves capability/source-device reporting,
  import dispatch, shared device-access identity, and COM resource retention
  after the original texture and view references are released.

Default platform order after the contracts are stable:

1. Apple reference path on the current macOS host.
2. Windows reference path.
3. Linux path.
4. Android path.

The user may override this order.

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

### Audio and hardware decode

- [x] `qtav_audio_wasapi`.
- [x] Shared-mode PCM negotiation and audio clock.
- [x] Complete the D3D11VA device/frame/interop design checkpoint.
- [x] `qtav_hw_d3d11va`.
- [ ] `qtav_interop_d3d11` for zero-copy decoder textures.

Acceptance:

- Windows native example plays A/V without Qt;
- software and D3D11 hardware decode both work;
- device-loss and surface-recreation paths are tested;
- no Windows type leaks into core public headers.

## Milestone 6 — Linux and Android

### Linux

- [ ] OpenGL renderer.
- [ ] Vulkan renderer.
- [ ] ALSA and/or PulseAudio sink.
- [ ] VAAPI hardware decode and interop.

### Android

- [ ] OpenGL ES and/or Vulkan renderer.
- [ ] AAudio sink, with OpenSL fallback only if required.
- [ ] MediaCodec hardware decode and surface/texture interop.

## Milestone 7 — Playback feature parity

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

## Milestone 8 — Dolby and HDR scope

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
