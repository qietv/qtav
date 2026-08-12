# QtAVCore

QtAVCore is the Qt-free evolution path for QtAV. It keeps FFmpeg as the media
engine, replaces Qt types and the Qt event model with standard C++17, and uses
high-level platform outputs for ordinary presentation while retaining an
application-owned rendering callback, inspired by the public shape of
`mdk-sdk`, for custom graphics integration.

It is not source- or binary-compatible with `mdk-sdk`, and no `mdk-sdk` source
code is used.

The current system structure is documented in
[`ARCHITECTURE.md`](ARCHITECTURE.md). Durable architecture choices and their
consequences are recorded in [`DECISIONS.md`](DECISIONS.md), while milestone
status and the active next task live in [`PLAN.md`](PLAN.md). Completed
checklists, investigation narratives, device matrices, and historical
validation evidence are retained in
[`PLAN_HISTORY_2026-08-10.md`](PLAN_HISTORY_2026-08-10.md).
The current CI scope, runner contract, local reproduction commands, cache
boundary, and device-only exclusions are documented in
[`CI.md`](CI.md).

## Supported targets

QtAVCore is maintained for Windows, Android, and OHOS targets only. The former
macOS and iOS backends, tests, and integration notes were moved to
[`../archived_apple/`](../archived_apple/) and are no longer maintained,
built, tested, packaged, or installed. A macOS machine may still be used as a
cross-compilation host for Android/OHOS, and 64-bit Windows may cross-compile
OHOS with the DevEco native SDK; neither host role adds another supported
QtAVCore target. Linux is not part of the active target matrix or roadmap.

## Current scope

- no Qt headers, libraries, meta-object compiler, or event loop;
- asynchronous `qtav::Player` state machine;
- FFmpeg 8+ send/receive decoding API;
- local files and FFmpeg-supported network protocols, with bounded HTTP(S)
  protocol recovery plus a Player-level, observable, interruptible input-reopen
  policy after protocol recovery returns an error;
- bounded time/byte compressed-packet buffering with initial, seek/track, and
  underflow fill gates plus reason-aware progress/status callbacks;
- an opt-in live-playback video policy that keeps a caller-bounded newest-frame
  window and uses a caller-selected late threshold without dropping compressed
  packets, audio, or subtitles;
- one optional external audio input and one optional external subtitle input,
  merged with the main input on normalized media time and exposed through the
  same asynchronous track-selection contract;
- audio, video, and presentation-timed subtitle callbacks with reference-
  counted frame lifetime, normalized UTF-8 text, and preserved ASS/SSA packet
  data when supplied by FFmpeg;
- structured video range, primaries, transfer, matrix, chroma-location, HDR10
  mastering-display, and content-light metadata;
- libplacebo as the sole semantic color/shader authority for the Windows
  D3D11, Vulkan, and OpenGL ES GPU renderers, including color conversion,
  Dolby Vision reshaping, tone/gamut mapping, scaling, and output encoding;
- `prepare`, `seek`, pause/resume/stop, playback rate, A-B range, and loop;
- explicit frame-accurate video seek plus asynchronous forward/backward frame
  stepping that leaves playback paused;
- media/track information, asynchronous post-load audio/video/subtitle track
  switching, `avformat.*` property forwarding, and an
  `avcodec.video.threads` software-video decoder override;
- decoder-driven `setRenderCallback()` plus reason-aware render-thread
  `renderVideoDetailed()`, per-renderer exact-frame retention across
  backend-redraw deferral, and the compatibility `renderVideo()` wrapper;
- compile-time `VideoRenderAPI`, `AudioSink`, and hardware-frame interop
  contracts;
- optional libswscale CPU renderer for application-owned image buffers;
- optional libass text/ASS subtitle rasterizer that returns ordered, owning
  coverage bitmaps for application composition without entering the core ABI;
- optional Windows D3D11 renderer for a retained application-selected device
  and immediate context plus borrowed current render-target/swap-chain views,
  using libplacebo for FFmpeg color metadata, Dolby Vision RPU reshaping, HDR
  tone/gamut mapping, scaling, and Windows Advanced Color SDR, FP16 scRGB, or
  RGB10 HDR10 output;
- optional high-level Windows D3D11 composition output that owns the device,
  HDR-aware FP16 scRGB or SDR swap chain, render target, display tracking,
  redraw-coalescing thread, D3D11VA/interop wiring, reason-aware rendering,
  `Present()`, resize, and teardown;
- optional libswresample converter for negotiated interleaved PCM output;
- optional pitch-preserving streaming audio time stretch after format
  conversion, with an FFmpeg `atempo` reference backend and no mandatory DSP
  dependency in the core;
- optional RIFF/WAVE PCM diagnostic file sink;
- optional Windows WASAPI shared-mode device sink with native playback timing;
- optional Android AAudio device sink with callback-safe bounded buffering,
  device timing, latency reporting, and disconnect recovery;
- optional OHOS OHAudio device sink with negotiated Float32 PCM,
  callback-safe bounded buffering, hardware presentation timing, lifecycle
  control, and non-callback route/error recovery;
- optional OHOS OHCodec H.264/HEVC plus capability-gated VVC/H.266 direct-
  surface hardware decoding through a retained, versioned application-
  supplied `OHNativeWindow`, with move-only present/drop/timed-presentation
  tokens and native FFmpeg software fallback;
- optional D3D11VA hardware decoding on an application-selected retained
  D3D11 device, with reference-counted NV12/P010 decoder texture-array slices,
  a default visible-region GPU-copy render policy, optional direct decoder-
  texture sampling, and explicit software fallback;
- optional Android MediaCodec H.264/HEVC hardware decoding into an
  application-supplied, versioned `ANativeWindow`, with explicit present,
  monotonic-time present, drop, stale-generation rejection, and software
  fallback policy;
- optional Android MediaCodec-to-Vulkan zero-CPU-copy texture interop through
  a private GPU-sampled `AImageReader`, retained `AHardwareBuffer` external
  memory, native YCbCr sampling, and acquire/release fence bridging;
- optional Android MediaCodec-to-OpenGL ES zero-CPU-copy texture interop
  through a private GPU-sampled `AImageReader`, retained `AHardwareBuffer`,
  EGLImage import, raw Y/Cb/Cr sampling, and acquire/release fence bridging;
- optional platform-neutral Vulkan software-frame renderer using borrowed
  application-selected device/queue and current-image resources, with
  libplacebo color conversion, tone mapping, Dolby Vision RPU reshaping, and
  SDR, HDR10/PQ, HDR10/HLG, or extended-linear output contracts;
- optional Android Vulkan surface adapter that retains the current
  `ANativeWindow` generation, selects a supported SDR/native-HDR swapchain,
  publishes its output color space, and owns surface/swapchain synchronization;
- optional OHOS Vulkan surface adapter that retains an ArkUI/XComponent
  `OHNativeWindow`, owns `VK_OHOS_surface` swapchain synchronization, and
  verifies the selected native-window SDR/HDR color space before delegating
  rendering to the same platform-neutral Vulkan engine;
- optional platform-neutral OpenGL ES 3.x renderer using libplacebo for
  software and hardware-frame color conversion, Dolby Vision RPU reshaping,
  tone mapping, gamut mapping, and output encoding, plus separate Android and
  OHOS EGL adapters. Android selects native 10-bit BT.2020/PQ or BT.2020/HLG
  when available and preserves an explicit RGBA8/sRGB SDR fallback. OHOS now
  applies the same exact RGB10_A2 BT.2020/PQ or BT.2020/HLG candidate policy,
  verifies the `OHNativeWindow` color space, and falls back to verified
  RGBA8/sRGB unless HDR is required; native-HDR validation on a capable OHOS
  window/device remains a separate gate;
- optional platform-neutral mobile renderer selector that defaults to
  Vulkan-preferred startup, exposes an OpenGL ES preference, performs bounded
  same-API recovery and fatal one-way fallback, and has an explicit
  no-renderer state;
- explicit cross-API hardware-frame fallback decisions that rebind subsequent
  decoder output to compatible OpenGL ES native interop or select direct
  surface, software decode, or no video without retrying or mapping a frame
  produced for the retired Vulkan surface;
- an accepted Android/OHOS mobile rendering policy that prefers Vulkan by
  default, permits a user-selected OpenGL ES preference, and uses the separate
  backend after Vulkan is unavailable or fails fatally, while keeping
  recoverable surface recreation within the active API;
- a reproducible Android arm64 cross-build and connected-device
  NativeActivity harness for QtAVCore plus pinned FFmpeg 8.1.2 software
  decoding, Vulkan presentation, OpenGL ES/EGL native-HDR plus SDR fallback,
  AAudio output, MediaCodec H.264/HEVC direct-surface validation, and
  H.264/HEVC private-AImageReader Vulkan plus AHardwareBuffer/EGLImage OpenGL
  ES texture paths;
- a minimal OHOS ArkUI/XComponent HAP shell and connected-device harness for
  generated-media software decode, Vulkan and OpenGL ES presentation, forced
  initial OpenGL ES selection, and fatal one-way Vulkan-to-OpenGL ES fallback
  without reopening media, plus OHAudio output/device-master timing and a
  complete H.264/HEVC OHCodec direct-surface lifecycle matrix with bounded
  output retention and stale-generation rejection, native Vulkan-to-OpenGL ES
  OHCodec surface rebind, and an independent software-decode fallback, through
  the repository arm64/API 23 dependency package;
- a separate user-facing OHOS ArkUI player demo with local document and direct
  URL opening, full-screen and picture-in-picture surface transfer, accurate
  seek, pitch-preserving playback rates, post-load audio/subtitle switching,
  presentation-timed text subtitles, selectable software/OHCodec decode and
  Vulkan/OpenGL ES rendering, explicit HDR policy/diagnostics, a closeable
  1 Hz media/FPS overlay, and a separate lightweight progress snapshot;
- standalone CMake package and headless integration tests.

The core does not open a platform audio device by default. Applications can
keep consuming decoded frames through `onAudioFrame()` and can optionally bind
an `AudioSink`; the Windows WASAPI, Android AAudio, and OHOS OHAudio
implementations remain separate backend targets so the core acquires no Qt or
platform dependency.

Current backend integration boundary:

- `VideoRenderAPI` is connected to `Player` and supports multiple renderer
  instances keyed by application-owned opaque pointers;
- `AudioSink` is connected through `Player::setAudioSink()`, follows playback
  lifecycle changes, and supplies the playback master when its device clock is
  supported and valid; decoded PCM crosses a bounded queue to a dedicated
  audio-output worker; queued audio is drained at completed playback segments,
  including loop boundaries, and before final close; seek/underrun buffering
  freezes `position()` until the device clock re-anchors (or callback-only
  output is actually delivered);
- `AudioFrameConverter` is connected through
  `Player::setAudioFrameConverter()` when a sink negotiates different PCM;
  `QtAV::AudioResample` supplies the portable libswresample implementation;
- `AudioTimeStretcher` is connected through
  `Player::setAudioTimeStretcher()` after PCM conversion and before the sink;
  `QtAV::AudioTimeStretch` supplies the FFmpeg `atempo` implementation for
  pitch-preserving rates other than 1.0, while 1.0 bypasses the stage;
- `AudioFrameProcessor` adds an optional streaming PCM-effect stage after time
  stretch, while `VideoFrameProcessor` adds an optional synchronous software-
  frame transform after direct scheduling declines a frame and before ordinary
  presentation; `QtAV::AudioFilter` supplies the narrow FFmpeg-backed constant-
  gain reference implementation;
- `QtAV::AudioFile` writes negotiated interleaved PCM to a standard RIFF/WAVE
  file for diagnostics without becoming a playback clock;
- `QtAV::SubtitleLibass` consumes the ASS header/events retained by
  `SubtitleFrame`, resets its track on presentation-generation or track
  changes, and returns ordered owning coverage bitmaps at a caller-selected
  media position;
- `QtAV::AudioWASAPI` negotiates shared-mode Float32 mono/stereo PCM against a
  Windows render endpoint, owns an event-driven queue on a dedicated COM
  thread, and supplies an `IAudioClock`-backed playback clock and latency;
- `QtAV::AudioAAudio` negotiates Float32 mono/stereo PCM against the current
  Android output route, feeds AAudio's real-time callback from a bounded
  lock-free queue, maps AAudio presentation timestamps to the media timeline,
  and rebuilds a disconnected default-route stream on a non-callback thread;
- `QtAV::AudioOHAudio` negotiates 48 kHz Float32 mono/stereo PCM against the
  current OHOS output route, feeds OHAudio's real-time callback from the same
  portable bounded SPSC queue, maps hardware-committed frame timestamps to the
  media timeline, and rebuilds route-changed or failed streams on a dedicated
  backend worker;
- `HardwareDecodeConfig` selects an optional hardware device for video decode;
  its optional reference-counted `HardwareDecodeDevice` lets an in-tree
  backend supply a pre-created native device without exposing FFmpeg or
  platform SDK types;
  `QtAV::HWMediaCodec` explicitly selects FFmpeg's MediaCodec wrapper decoder,
  binds it to a versioned application `ANativeWindow`, and turns each decoded
  output into a single-decision direct-surface present/drop token;
- `QtAV::HWOHCodec` retains and versions an application-supplied
  `OHNativeWindow`, creates FFmpeg's `AV_HWDEVICE_TYPE_OHCODEC` device, and
  explicitly selects the H.264/HEVC or capability-gated VVC `*_ohcodec`
  wrapper decoder. Each surface output becomes a single-decision
  present/drop/timed-presentation token;
- `QtAV::InteropMediaCodecVulkan` owns a private GPU-sampled `AImageReader`,
  supplies its surface to `QtAV::HWMediaCodec`, correlates codec and image
  timestamps, imports retained `AHardwareBuffer` images and fences into the
  application-owned Vulkan device, and returns a release sync fd after GPU
  submission without a decoded-pixel map, software transfer, staging copy, or
  renderer upload;
- `QtAV::InteropMediaCodecOpenGL` owns a private GPU-sampled Android
  `AImageReader`, supplies its producer window to `QtAV::HWMediaCodec`,
  correlates codec and image timestamps, imports retained `AHardwareBuffer`
  images as EGLImages, and returns a release sync fd after GL submission and
  platform presentation
  without a decoded-pixel map, software transfer, staging copy, or upload;
- `QtAV::RenderCPU` converts and scales decoded software frames into packed
  RGB/BGR/RGBA/BGRA/ARGB or Gray8 buffers;
- `QtAV::RenderD3D11` uploads software YUV420/422/444, NV12/NV21, P010,
  RGB/BGR/RGBA/BGRA/ARGB, or Gray8 frames and renders through an
  application-selected D3D11 device and immediate context plus the current
  application-owned render-target view. libplacebo's D3D11 backend is the sole
  Windows authority for color conversion, Dolby Vision, HDR tone mapping,
  gamut mapping, scaling, and output encoding; QtAVCore does not build its
  Vulkan or OpenGL render targets on Windows;
- `QtAV::PlatformWindows` retains an application-selected D3D11 device and
  its verified immediate context behind a shared recursive context guard;
- `QtAV::HWD3D11VA` creates FFmpeg's hardware device on that selected D3D11
  device, shares the same context lock, and exposes retained NV12/P010 decoder
  texture-array slices through a Windows-only strong frame view. Decoder
  resources are shader-readable only when direct sampling is explicitly
  enabled;
- `QtAV::InteropD3D11` consumes same-device D3D11VA NV12/P010 texture-array
  slices without mapping, transferring, staging, or uploading decoded pixels
  through CPU memory. The renderer defaults to a same-format GPU copy of only
  the even-aligned visible region, then exposes its raw luma/chroma planes to
  libplacebo without a D3D11 Video Processor RGB conversion. Strict no-
  intermediate source zero-copy is available only through the explicit direct-
  sampling option;
- `QtAV::OutputD3D11` combines the Windows device, renderer, decoder, and
  interop targets into a composition-surface output for ordinary applications;
  the application supplies a swap-chain binding callback, surface size, and
  normally its hosting HWND, then the output owns display/HDR tracking,
  render scheduling, and presentation;
- `QtAV::RenderVulkan` maps supported FFmpeg software frames into libplacebo
  textures and renders into an application-supplied current image. libplacebo
  owns color conversion, scaling, tone mapping, Dolby Vision reshaping, and
  final SDR/HDR encoding while QtAVCore supplies the common
  viewport/aspect/rotation contract. The target's `VkColorSpaceKHR` selects
  SDR, native HDR10/PQ, HDR10/HLG, or extended-linear output;
- `QtAV::RenderVulkanAndroid` owns Android surface, swapchain, image-view, and
  acquire/present resources for a retained active `ANativeWindow`, prefers
  native HDR format/color-space pairs when available, and submits HDR10 static
  metadata when the application enabled `VK_EXT_hdr_metadata`; the application
  owns the Vulkan instance, device, queue, and NativeActivity;
- `QtAV::RenderVulkanOHOS` owns the OHOS surface, swapchain, image views, and
  acquire/present resources for a retained XComponent `OHNativeWindow`. The
  ArkUI application owns the XComponent and borrowed Vulkan instance, device,
  queue, and their lifetime;
- `QtAV::RenderOpenGL` uploads the same software YUV420/422/444, NV12/NV21,
  P010, RGB/BGR/RGBA/BGRA/ARGB, and Gray8 families into OpenGL ES textures,
  maps their structured metadata through the shared FFmpeg/libplacebo bridge,
  applies the common viewport/aspect/rotation contract, and asks libplacebo's
  OpenGL backend to generate the shaders for explicit SDR sRGB, HDR10/PQ, or
  HDR10/HLG output into a caller-supplied current framebuffer; an optional
  `OpenGLPresentCallback` submits that framebuffer before a hardware source
  image receives its producer release fence;
- `QtAV::RenderOpenGLAndroid` retains the current `ANativeWindow` generation
  and owns its EGL display, OpenGL ES 3.x context, window surface, and swap,
  prefers an exact RGB10_A2 BT.2020/PQ surface, considers BT.2020/HLG, and
  falls back explicitly to RGBA8/sRGB while keeping Android and EGL types
  outside core public headers;
- `QtAV::RenderOpenGLOHOS` retains the current XComponent `OHNativeWindow`
  generation, owns its EGL display, OpenGL ES 3.x context, window surface, and
  swap, prefers exact RGB10_A2 BT.2020/PQ then BT.2020/HLG when the required
  EGL extensions are exposed, verifies the matching OHOS native-window color
  space, and falls back to exact RGBA8/sRGB unless HDR is required;
- `QtAV::RenderMobile` owns no graphics or platform resources. Applications
  supply Vulkan and OpenGL ES renderer factories for the current native-window
  generation; the selector keeps one stable `VideoRenderAPI` attached to
  `Player` across same-API recreation and one-way fallback. A synchronous
  hardware-frame fallback callback selects OpenGL ES native interop,
  direct-surface presentation, software decode, or no video for subsequent
  output; cross-API fallback never retries or maps the retired native frame;
- OHOS OHCodec decoder selection, explicit direct-surface presentation tokens,
  and the H.264/HEVC lifecycle matrix are implemented and device-validated.
  Native hardware-frame texture interop is implemented with retained
  `OH_NativeBuffer` import through Vulkan and raw-component OpenGL ES. The
  OpenGL ES fallback
  requires raw `GL_EXT_YUV_target` sampling rather than implicit external-OES
  YUV-to-RGB conversion.

Mobile renderer creation remains in the application or thin platform layer
that owns the native window and graphics devices, while
`MobileVideoRendererSelector` implements the accepted shared selection policy.
A new session probes `MobileRendererSelectorConfig::preferredAPI` first
(Vulkan by default) and selects the other configured API when the preferred
backend is unavailable or cannot open its initial surface. `SurfaceLost`
causes bounded same-API recreation; a fatal error or exhausted Vulkan recovery retires Vulkan
for that session and retries the retained frame through OpenGL ES without
reopening media. `suspendSurface()` and `recreateSurface()` preserve the
selected API across an application-led native-window replacement. If both
APIs fail, video presentation reports unavailable while playback, audio, and
decoded-frame callbacks remain usable. Decoder, direct-surface, interop, and
renderer fallback policies remain independent. When the current frame is
hardware-backed, `setHardwareFrameFallbackCallback()` runs synchronously
after the OpenGL ES candidate is prepared and before any cross-API retry. The
application reconfigures subsequent decoder output and returns the chosen
route. If OpenGL ES hardware interop later fails, the callback runs again so
the application can disable hardware decode and continue with software frames.
Late frames from a retired native surface are discarded without mapping; no
callback or a `None` decision reports presentation unavailable.
The accepted design is
specified in [`MOBILE.md`](MOBILE.md).

The Android Vulkan and OpenGL ES zero-CPU-copy paths are separate backends.
Their platform interop responsibility stops at native import, format/plane
exposure, timestamp/generation correlation, synchronization, and retained
source lifetime; libplacebo owns all semantic processing.
Vulkan consumes private GPU-sampled `AImageReader` images by importing retained
`AHardwareBuffer` allocations, native YCbCr/external formats, and
acquire/release fences. OpenGL ES also owns a private GPU-sampled
`AImageReader`, imports each retained `AHardwareBuffer` as an EGLImage, waits
its acquire fence, and returns a release fence after submission.
`GL_EXT_YUV_target` exposes raw Y, Cb, and Cr; a crop-aware GPU normalization
pass stores those components in RGBA16F without performing color conversion,
after which libplacebo applies the complete semantic color pipeline.

Here, zero-CPU-copy means zero decoded-source map, software transfer, CPU
staging, and re-upload calls plus verified native-buffer lifetime and fence
ordering. It permits a GPU normalization texture. The narrower strict
no-intermediate source zero-copy claim is allowed only when an explicit native
format and plane mapping let libplacebo wrap the retained decoder allocation
directly, with no normalization draw or intermediate source texture. Android
OpenGL ES raw RGBA16F normalization and opaque-external-format Vulkan
normalization are therefore zero-CPU-copy, but not strict source zero-copy.

OHOS must use the same responsibility boundary. The preferred future route
retains the exact `OH_AVBuffer`/`OH_NativeBuffer` and imports it through
`VK_OHOS_external_memory`. Only an explicit `VkFormat`/plane mapping can
qualify that route as strict
source zero-copy. An opaque external format that needs GPU normalization keeps
only the zero-CPU-copy claim. The OpenGL ES fallback requires raw
`GL_EXT_YUV_target` sampling followed by RGBA16F GPU normalization; implicit
external-OES YUV-to-RGB conversion is not a target. OHCodec/NativeImage may
propagate the codec PTS unchanged in microseconds, so the interop compares the
observed value and its microsecond-to-nanosecond candidate against the exact
queued-frame PTS set, then stores and correlates the selected value in
nanoseconds. The current FFmpeg 8 OHCodec buffer branch performs
`OH_AVBuffer_GetAddr()` plus `av_image_copy2()`
and cannot satisfy either native-buffer route as-is. Unsupported imports are
reported rather than silently mapped, and a renderer switch does not itself
authorize a CPU copy.

## Build

Requirements:

- CMake 3.20 or newer;
- a C++17 compiler;
- FFmpeg 8.0 or newer development libraries;
- libplacebo 7.351.0 or newer for the Vulkan/OpenGL ES renderers or the Windows
  D3D11 renderer;
- `pkg-config` is required for a libplacebo renderer and recommended for
  resolving static FFmpeg dependency closures.

The GitHub Actions workflow currently runs the Windows shared/static build,
CTest, install, and package-consumer gate only. The checked-in Android and
OHOS PowerShell drivers remain available for explicit local cross-builds, but
their Actions jobs are temporarily disabled. Cross-built Android/OHOS tests
are never executed on Windows; signing, installation, native HDR/audio, codec,
and physical-device validation remain separate gates and are never reported as
CI passes. See [`CI.md`](CI.md) for the exact active and suspended boundaries.

### Version and package compatibility

QtAVCore 2.0.0 is the first formal release version for the Qt-free rewrite.
The CMake project version is the single source for the installed package,
public version header, and library metadata. Applications can query it without
including FFmpeg, CMake-generated platform declarations, or a native SDK:

```cpp
#include <qtav/version.h>

static_assert(QTAV_CORE_VERSION_MAJOR == 2);
static_assert(qtav::coreVersion == qtav::Version { 2, 0, 0 });
static_assert(qtav::coreVersionString == "2.0.0");
```

Installed CMake packages always define `QtAVCore_VERSION` and its
`_MAJOR`, `_MINOR`, and `_PATCH` components. Request an exact release when the
application requires one, or the oldest acceptable release within major 2:

```cmake
find_package(QtAVCore 2.0.0 EXACT CONFIG REQUIRED)
# or
find_package(QtAVCore 2.0 CONFIG REQUIRED)
target_link_libraries(my_player PRIVATE QtAV::Core)
```

Package discovery uses CMake `SameMajorVersion` compatibility: an installed
package must be at least the requested version and have the same major version.
That selection rule is not, by itself, a C++ binary-compatibility proof.
Compatibility claims are deliberately separate:

- patch releases preserve public source and shared-library interfaces and are
  reserved for compatible fixes;
- minor releases may add public headers, APIs, or exported targets while
  preserving existing source contracts and the shared ABI within one supported
  ABI domain;
- major releases may remove or change public C++ contracts, structure layouts,
  virtual interfaces, exported target names, or other shared ABI boundaries;
- a shared ABI domain requires the same target architecture, compiler ABI and
  compatible toolset, C++ standard library/runtime mode, relevant build mode,
  and dependency ABI. QtAVCore does not promise cross-compiler, cross-runtime,
  or cross-architecture C++ ABI compatibility, and static consumers rebuild;
- existing exported CMake target names remain available through a major
  release. Adding an optional target is compatible; removing or incompatibly
  redefining one requires a major release.

Every supported shared target carries CMake `VERSION` equal to the complete
QtAVCore release and `SOVERSION` equal to its major version. This supplies the
PE image version on Windows and the ELF major soname plus versioned library
names on OHOS. Android's NDK intentionally emits the platform-standard
unversioned `.so` filename and soname even when those target properties are
set, so the public header and CMake package remain the Android release-version
authority. None of this introduces a runtime plugin ABI: optional backends
remain compile-time C++ targets, and a future loader still requires a
separately designed versioned C ABI.

```sh
cmake -S modern -B build/modern -DQTAV_CORE_BUILD_TESTS=ON
cmake --build build/modern
ctest --test-dir build/modern --output-on-failure
```

### OHOS arm64/API 23 on Windows

On 64-bit Windows with DevEco Studio and the OpenHarmony native SDK installed,
the supported PowerShell entry point builds the repository FFmpeg dependency
closure locally, then builds and installs a shared Release QtAVCore SDK:

```powershell
./modern/scripts/build-ohos.ps1
```

Use `-SkipDependencies` after the local dependency package has already been
verified, or `-LibraryType Static` for static QtAVCore archives. The scripts,
output layout, SDK discovery, space-free SDK junction, shared-link policy, and
HAP/signing boundary are documented in
[`OHOS_WINDOWS.md`](OHOS_WINDOWS.md).

To build the OHOS XComponent example and stage it into an existing signed
DevEco project, use:

```powershell
./modern/examples/ohos/build-ohos-hap.ps1 `
  -ProjectRoot C:/path/to/signed-project
```

The independent manual player demo builds an unsigned HAP when no local DevEco
signing configuration is present and never deploys it:

```powershell
./modern/examples/ohos/build-ohos-player-hap.ps1
```

After configuring signing in a DevEco project, pass that project through
`-ProjectRoot`; the script preserves its root signing profile. See
[`examples/ohos/PLAYER_DEMO.md`](examples/ohos/PLAYER_DEMO.md) for the feature
and signed-device acceptance matrix. The 2026-08-11 signed-device run covered
Vulkan HTTPS playback, FPS, rate/skip, full screen, live PiP, dual-audio and
subtitle switching, and the safe-access document picker.

With one HDC target connected, `run-connected-device.ps1` installs the signed
HAP, starts its `EntryAbility`, and collects the native PASS/FAIL result. See
[`examples/ohos/README.md`](examples/ohos/README.md) for the bundle-name and
generated-media options.

Backend switches are cache strings with `AUTO`, `ON`, and `OFF` values. `AUTO`
enables a backend when its implementation and host requirements are available,
`OFF` always disables it, and `ON` requires it or stops configuration with a
clear error. Current switches are:

- render: `QTAV_RENDER_CPU`, `QTAV_RENDER_MOBILE`, `QTAV_RENDER_OPENGL`,
  `QTAV_RENDER_VULKAN`, and `QTAV_RENDER_D3D11`;
- audio: `QTAV_AUDIO_WASAPI`, `QTAV_AUDIO_AAUDIO`,
  `QTAV_AUDIO_OHAUDIO`, `QTAV_AUDIO_RESAMPLE`,
  `QTAV_AUDIO_TIMESTRETCH`, `QTAV_AUDIO_FILTER`, and `QTAV_AUDIO_FILE`;
- subtitle: `QTAV_SUBTITLE_LIBASS`;
- hardware decode: `QTAV_HW_D3D11VA`, `QTAV_HW_MEDIACODEC`, and
  `QTAV_HW_OHCODEC`;
- interop: `QTAV_INTEROP_D3D11`,
  `QTAV_INTEROP_MEDIACODEC_VULKAN`, and
  `QTAV_INTEROP_MEDIACODEC_OPENGL`;
- output: `QTAV_OUTPUT_D3D11`.

`QTAV_RENDER_CPU=AUTO` builds the CPU renderer when libswscale is available,
`QTAV_RENDER_MOBILE=AUTO` builds the dependency-free mobile renderer selector,
`QTAV_RENDER_OPENGL=AUTO` builds the OpenGL ES renderer when GLES 3 headers
and libraries are available and adds the native EGL adapter on Android
(`QtAV::RenderOpenGLAndroid`) or OHOS (`QtAV::RenderOpenGLOHOS`),
`QTAV_RENDER_VULKAN=AUTO` builds the Vulkan renderer when a Vulkan loader,
libplacebo 7.351.0 or newer, and (on Android/OHOS) `glslc` are available. It adds
the native surface adapter target on Android (`QtAV::RenderVulkanAndroid`) or
OHOS (`QtAV::RenderVulkanOHOS`). The Android and OHOS harnesses require this
backend explicitly,
`QTAV_RENDER_D3D11=AUTO` builds the native software-frame renderer on Windows,
`QTAV_AUDIO_RESAMPLE=AUTO` builds the PCM converter when libswresample is
available, `QTAV_AUDIO_TIMESTRETCH=AUTO` builds the pitch-preserving
`QtAV::AudioTimeStretch` backend when libavfilter is available,
`QTAV_AUDIO_FILTER=AUTO` builds the format-preserving `QtAV::AudioFilter`
reference backend when libavfilter is available,
`QTAV_AUDIO_FILE=AUTO` builds the dependency-free diagnostic sink,
`QTAV_SUBTITLE_LIBASS=AUTO` builds `QtAV::SubtitleLibass` when libass 0.17.0
or newer is available,
`QTAV_AUDIO_WASAPI=AUTO` builds the shared-mode device sink on Windows,
`QTAV_HW_D3D11VA=AUTO` builds the Windows hardware-decode
selection and native-frame access target. `QTAV_INTEROP_D3D11=AUTO` builds the
Windows raw NV12/P010 plane adapter when the D3D11 renderer and D3D11VA
decoder targets are available. `QTAV_OUTPUT_D3D11=AUTO` builds the high-level Windows
composition output when the D3D11 renderer, D3D11VA decoder, and interop
targets are available. `QTAV_AUDIO_AAUDIO=AUTO` builds the AAudio sink on
Android API 26 or newer; the current Android harness targets API 28 and does
not require OpenSL ES fallback. `QTAV_AUDIO_OHAUDIO=AUTO` builds the OHAudio
sink for OHOS/OpenHarmony targets when `libohaudio` is available.
`QTAV_HW_MEDIACODEC=AUTO` builds the Android
MediaCodec direct-surface backend when the NDK Media APIs and FFmpeg's
MediaCodec hardware context are available.
`QTAV_HW_OHCODEC=AUTO` builds the OHOS decoder-selection backend when
`libnative_window` and FFmpeg's OHCodec hardware context are available.
`QTAV_INTEROP_MEDIACODEC_VULKAN=AUTO` builds the private-AImageReader Vulkan
interop on Android API 26 or newer when the MediaCodec and Vulkan targets are
both available.
`QTAV_INTEROP_MEDIACODEC_OPENGL=AUTO` builds the private-AImageReader
AHardwareBuffer/EGLImage interop on Android API 28 or newer when the
MediaCodec and OpenGL ES targets are both available.
`QTAV_INTEROP_OHCODEC_VULKAN=AUTO` builds the OHCodec/ConsumerSurface
Vulkan interop on OHOS when the OHCodec and Vulkan targets plus
`libnative_buffer`, `libnative_image`, and `libnative_window` are available.
Backend implementations not otherwise described remain disabled under
`AUTO`, and explicitly requesting one with `ON` is an error.

Run the headless example:

```sh
build/modern/examples/qtav_core_console /path/to/media.mp4
```

Visual Studio multi-config builds place executables and project DLLs together
under `build/modern/bin/<Config>`, for example
`build/modern/bin/Release/qtav_core_console.exe`.

On Windows the console example sends decoded audio through
`QtAV::AudioWASAPI` and, when the D3D11
targets are available, exercises D3D11VA plus `QtAV::InteropD3D11` into an
offscreen D3D11 render target. Android and OHOS applications provide their
own platform shells rather than using this headless native-device example.

Windows CTest runs the example against generated H.264/AAC media with
`QTAV_CORE_REQUIRE_NATIVE_WINDOWS_AV=1`. In that strict mode the example
requires hardware video frames, successful D3D11 rendering, decoded audio, and
a usable WASAPI endpoint. A session without an active render endpoint returns
CTest skip code 77, so unavailable device validation is not reported as a
pass. The strict test has also been exercised with an active endpoint, where
H.264/AAC playback passed and produced audible output.

### Android arm64 foundation harness

The initial Android production-path harness is under
`examples/android/`. It uses SDK CMake/Ninja and NDK r29 to consume the
repository-local arm64/API 28 FFmpeg 8.1.2 vcpkg dependency package and
cross-build QtAVCore for `arm64-v8a`, then uses AAPT2, zipalign, and apksigner
to package a NativeActivity without Qt or a Gradle dependency:

```sh
modern/examples/android/build-android.sh
modern/examples/android/run-connected-device.sh
```

Generated inputs and outputs remain under `build/android/`. The connected
device script requires exactly one authorized device, records ABI/API and
Vulkan facts, installs once, launches the generated-media playback test, and
collects its pass/fail log. If installation or replacement fails because the
device may be waiting for user authorization, stop and approve the prompt
manually before retrying. The Vulkan-enabled Android 16/arm64 device run now
decodes and presents 180 MPEG-4 video frames through an Adreno 830 Vulkan
1.3.284 native HDR10/PQ swapchain, decodes 282 PCM audio frames, submits
`VK_EXT_hdr_metadata`, and recreates the HDR surface/swapchain across a
background/foreground transition.

This is a toolchain, packaging, software-decode, Vulkan-rendering, and
OpenGL ES presentation checkpoint. The Vulkan renderer uses a bounded
three-frame resource ring and retains
each source frame until its fence completes. Platform-neutral offscreen
readback goldens run in the Android harness and cover YUV color conversion,
limited/full range, BT.601/BT.709 matrices, viewport, rotation, target
recreation, ring reuse, and P010/BT.2020 PQ/HLG input with mastering-display,
MaxCLL, and default-luminance selection. The same vectors verify both SDR
BGRA8 luminance compression and 10-bit native HDR10/PQ plus HDR10/HLG target
encoding, HLG-to-PQ conversion, and FP16 extended-linear/BT.2020-linear output
above reference white. The real-device harness requires an HDR swapchain,
records its format/color space, requires the Android compositor to report an
active HDR layer, presents a synthetic P010/BT.2020/PQ frame with mastering
and MaxCLL metadata, and verifies that the swapchain survives surface
recreation. The same device run now compiles the OpenGL ES 3 shaders, validates
offscreen uploads for YUV420/422/444, NV12/NV21, P010, RGB/BGR,
RGBA/BGRA/ARGB, and Gray8, checks viewport, rotation, and target-generation
replacement, and presents a P010/PQ-to-SDR frame through the real Android EGL
window adapter after the Vulkan lifecycle completes. It then requires an
RGB10_A2 BT.2020/PQ-or-HLG EGL surface, renders the same P010/PQ source without
SDR tone mapping, and independently verifies that Android reports the EGL
surface as an active HDR layer. The harness keeps one
`MobileVideoRendererSelector` attached to the player, recreates Vulkan across
the activity background/foreground window replacement, and separately proves
forced Vulkan-unavailable startup through both the explicit SDR and required
HDR OpenGL ES policies. The same run converts decoded PCM through
`QtAV::AudioResample`, presents it through `QtAV::AudioAAudio`, and validates
a monotonic device clock plus non-negative queued/device latency across the
pause/resume transition. It then explicitly selects FFmpeg's H.264 and HEVC
MediaCodec wrapper decoders, presents or drops their direct-surface outputs,
seeks, replaces media, stops explicitly, rejects an output token against the
old surface generation, and reopens against a replacement `ANativeWindow`
without mapping decoded pixels. A final texture-interoperable MediaCodec phase
runs through the private `AImageReader` target for both H.264 and HEVC:
the device imports external-format `AHardwareBuffer` images, samples them
through Vulkan YCbCr conversion, returns release sync fds, and requires all
decoded-source CPU-map, software-transfer, staging-copy, and renderer-upload
counters to stay zero. Separate H.264 and HEVC phases then use private
`AImageReader` producers, correlate MediaCodec output timestamps, import
AHardwareBuffer-backed EGLImages as raw YCbCr, cover seek/flush plus EGL window
suspension/recreation, return release fences, and keep the same decoded-source
counters at zero. A connected fallback phase keeps the same H.264 media
session, injects a fatal Vulkan renderer error after 30 successful
native-buffer presentations, creates the compatible AImageReader/OpenGL ES
candidate, and rebinds MediaCodec to its new producer through the explicit
policy callback. The recorded run continued with 32 Vulkan-generation and 180
OpenGL ES-generation decoded frames, matching raw imports and release fences,
and zero decoded-source map/transfer/staging/upload calls. The
accepted shared Android/OHOS responsibility and lifecycle design is documented
in [`MOBILE.md`](MOBILE.md).

### Android user player demo

The manual final-test and user-facing player is under
[`examples/android_player/`](examples/android_player/). It is separate from
the automated NativeActivity harness and provides a standard Android UI with
an upper `SurfaceView`, current time/seek/duration controls, local document
selection, direct FFmpeg HTTP/HTTPS URL opening, play/pause/stop, and live
Vulkan, HDR, ZeroCopy, hardware-decode, and debug-overlay switches. The
top-left diagnostics include a rolling successful-presentation FPS alongside
the source-rate hint and requested display refresh rate. They also show the
actual Vulkan/EGL output color space (or the MediaCodec buffer color metadata
on direct-Surface output), the active HDR policy, the current HDR/SDR headroom
ratio, and the panel's desired maximum HDR content luminance. In MediaCodec
direct-Surface mode the HDR switch changes the Android 15+
`SurfaceView` headroom request but does not convert a PQ/HLG codec buffer to
SDR; application-rendered paths use the switch as their `PreferHdr`/`SdrOnly`
output policy. A full-screen button or landscape rotation overlays all
controls on the video; five seconds without touch input hides only the
controls, leaving the Debug window independent.
ZeroCopy and hardware decode occupy their own second option row so the full
controls remain touchable on a portrait phone.

The option matrix exercises software decode through Vulkan or OpenGL ES,
MediaCodec direct-Surface output, MediaCodec/AImageReader Vulkan ZeroCopy, and
MediaCodec/AImageReader/AHardwareBuffer/EGLImage OpenGL ES ZeroCopy. Changing
an option rebuilds the affected native pipeline at the current position.
Remote URLs go directly to
QtAVCore/FFmpeg networking; the example uses OpenSSL from the same repository
vcpkg dependency package and explicitly
verifies HTTPS peers and host names against an app-private PEM bundle assembled
from Android's public system trust store, and does not download through Java
or the Android networking stack. This is remote-file playback, not adaptive
streaming.

Hardware decode with Vulkan ZeroCopy is the example default so application
color processing, including Dolby Vision, reaches libplacebo. Disabling
ZeroCopy explicitly selects direct-Surface presentation. The core paces
MediaCodec packets by DTS before decode, independently
of audio decode and device submission, and retains only a small bounded window
of decoded Surface outputs for the presentation worker. This avoids both
demux starvation and exhaustion of the codec output pool. The dedicated
Vulkan render thread reserves a
bounded slot before releasing each ZeroCopy output and waits only for the
private AImageReader ownership transfer, preventing producer bursts from
silently coalescing images without waiting for the render deadline. Its
diagnostics separate core queue/late drops, application render drops,
frames-in-flight, AHardwareBuffer cache reuse, and interop queue/acquire/import
progress. Both private AImageReader paths accept zero as a valid timestamp.

Build and sign the arm64 debug APK without installing it:

```sh
modern/examples/android_player/build-android-player.sh
```

The output is `build/android-player/qtav-core-player.apk`. The connected-device
launcher has an explicit pre-install confirmation gate because modern Android
releases may require manual approval on the physical device:

```sh
modern/examples/android_player/run-connected-device.sh
QTAV_ANDROID_INSTALL_CONFIRMED=1 \
  modern/examples/android_player/run-connected-device.sh
```

The first command stops before `adb install`; run the second only after the
device is unlocked and ready for the manual prompt. See the example's
[`README.md`](examples/android_player/README.md) for codec scope, exact option
semantics, diagnostics, and manual test guidance.

## API shape

```cpp
#include <qtav/player.h>
#include <qtav/atempo_audio_time_stretcher.h>
#include <qtav/swresample_audio_converter.h>
#include <qtav/volume_audio_frame_processor.h>

qtav::Player player;
auto converter = std::make_shared<qtav::SwresampleAudioConverter>();
auto timeStretcher =
    std::make_shared<qtav::AtempoAudioTimeStretcher>();
auto audioProcessor =
    std::make_shared<qtav::VolumeAudioFrameProcessor>(0.75);

player
    .setAudioFrameConverter(converter)
    .setAudioTimeStretcher(timeStretcher)
    .setAudioFrameProcessor(audioProcessor)
    .setAudioSink(audioSink)
    .onVideoFrame([](const qtav::VideoFrame& frame, int track) {
        // Inspect, filter, or forward the decoded frame.
    })
    .onSubtitleFrame([](const qtav::SubtitleFrame& frame, int track) {
        show_subtitle(frame.text(), frame.timestamp(), frame.duration());
    })
    .setVideoRenderer([](const qtav::VideoFrame& frame, void* surface) {
        // Upload/draw on the application's render thread.
    })
    .setRenderCallback([&](void*) {
        schedule_on_render_thread([&] { player.renderVideo(); });
    });

player.setExternalMedia(qtav::MediaType::Audio, "commentary.opus");
player.setExternalMedia(qtav::MediaType::Subtitle, "captions.srt");
player.setMedia("movie.mkv");
player.setState(qtav::State::Playing);
```

Frame-accurate seek is explicit, so existing demux-level seek behavior remains
unchanged:

```cpp
player.seek(
    12'345,
    qtav::SeekFlag::Accurate,
    [](std::int64_t actualFrameTimestamp) {
        // The selected video frame has now been published. The timestamp is
        // the first decoded frame at or after 12,345 ms, or -1 on failure.
    });

player.stepForward();
player.stepBackward();
```

`SeekFlag::Accurate` can be passed to `seek()` or `prepare()` and takes
precedence over `KeyFrame` and `AnyFrame`. For an active video track, Player
seeks to a preceding keyframe, decodes without
publishing video frames before the target, and publishes the first frame whose
timestamp is at or after it. Audio and subtitle frames below the same target
remain suppressed for that presentation generation. A playing accurate seek
retains play intent and the normal `Buffering -> Loaded` output re-anchor. In
both playing and paused states, the selected frame is submitted immediately as
the new presentation anchor and is not eligible for ordinary late-frame or
queue-capacity dropping; paused playback remains paused. The callback runs on
the playback worker after the selected frame has been handed to
`setVideoFrameScheduler()` or the presentation worker, and receives the actual
frame timestamp. With no active video track, accurate seek falls back to the
completed demux seek because there is no video frame to refine.

`stepForward()` and `stepBackward()` require loaded, seekable media with an
active video track. They invalidate the old presentation generation, keep the
audio device paused, publish exactly one adjacent video frame, and leave Player
paused. Backward stepping reuses the retained predecessor when available;
otherwise it asynchronously decodes from the active range start to identify
the exact previous frame. The step callback receives the published timestamp,
or `-1` at the corresponding media/range boundary.

`setExternalMedia()` accepts `MediaType::Audio` or `MediaType::Subtitle`; an
empty URL removes that sidecar. One input of each type may be configured, and
each input may itself contain multiple tracks. The configuration persists
across main-media replacement. Changing it while media is loaded
asynchronously reopens the main input at the current position and preserves
play/pause intent. The main input remains the authority for the playback
duration, range, and end of media.

After `MediaStatus::Loaded`, `MediaInfo::tracks` contains tracks from the main
input and configured sidecars plus the three `active*Track` fields. Main-input
`TrackInfo::index` values retain their FFmpeg stream indices. External tracks
receive non-overlapping selector values; `TrackInfo::streamIndex` records the
actual stream within `sourceUrl`, and `external` identifies a sidecar. Pass a
matching
`TrackInfo::index` to `setActiveTrack(MediaType, index)`, or `-1` to disable
that media type. The method validates and queues the request synchronously;
decoder replacement, queue invalidation, and position restoration are
asynchronous. A playing switch reports `Buffering` until output from the new
generation arrives, emits `track.changed` on success, preserves play/pause
intent, and reopens the audio sink so a new native audio format is negotiated.
Seekable inputs return to the request position; non-seekable inputs continue
from the next packet of the selected stream.

Packet timestamps from each active input are normalized against that input's
own start time before demux ordering and frame presentation. If the main input
does not contain audio or subtitles, the best matching external track becomes
active automatically. Otherwise the main input keeps best-stream priority and
the application can select a sidecar with `setActiveTrack()`.

### Software-video decoder threads

FFmpeg software video decoding defaults to automatic frame/slice thread
selection. An application that needs a repeatable device experiment may set
the decoder thread count before opening media:

```cpp
player.setProperty("avcodec.video.threads", "4");
player.setMedia("movie.mkv");
```

The property is passed as FFmpeg's `threads` decoder option only when opening a
software video decoder. It does not affect audio or hardware decoders, and a
change takes effect on the next decoder open. QtAVCore emits the informational
`decoder.software.configuration` event after a successful open with the actual
thread count and active FFmpeg thread type. Keep the default automatic value
for ordinary playback unless device measurements justify a fixed count.

### Packet buffering

`PacketBufferPolicy` controls a bounded compressed-packet reservoir in front
of the independent audio/video decoders. The default waits for 500 ms from
each active A/V stream before initial playback, refills to 750 ms after a
confirmed 120 ms underflow, and caps the in-memory portion of each stream at
five seconds, the combined in-memory compressed payload at 32 MiB, and each
in-memory decoder queue at 128 packets. Both memory limits are public through
`maximumBufferMilliseconds` and `maximumBufferBytes`. The setter normalizes
negative durations to zero and restores the default byte limit when it is
zero. Set `enabled = false`, or set the applicable fill target to zero, to
bypass the corresponding fill gate.

```cpp
qtav::PacketBufferPolicy buffering;
buffering.initialBufferMilliseconds = 1'000;
buffering.rebufferMilliseconds = 1'500;
buffering.maximumBufferMilliseconds = 8'000;
buffering.maximumBufferBytes = 48U * 1024U * 1024U;
buffering.diskCache.enabled = true;
buffering.diskCache.maximumCacheMilliseconds = 60'000;
buffering.diskCache.maximumCacheBytes = 256U * 1024U * 1024U;

player
    .setPacketBufferPolicy(buffering)
    .onPacketBufferStatus([](const qtav::PacketBufferStatus& status) {
        update_buffer_ui(
            status.buffering,
            status.progress,
            status.bufferedMilliseconds,
            status.bufferedBytes);
    });
```

The optional `PacketDiskCachePolicy` is disabled by default. When enabled,
packets that no longer fit in the memory duration, byte, or 128-packet limits
spill into one bounded file under a player-specific directory in the system
temporary folder. Its defaults are 60 seconds per stream and 256 MiB combined.
The on-disk log compacts live entries before its configured byte boundary, and
the file plus its directory are removed automatically when the disk-backed
queue becomes empty, on seek, stop, media replacement, or player destruction.
This is volatile prefetch, not a persistent download or offline-media cache;
the temporary payload is not encrypted by QtAVCore.

If a fill target is larger than the configured memory duration, enabling the
disk cache raises its normalized duration enough for the combined reservoir to
reach that target. With disk caching disabled, the memory duration retains the
previous behavior and is raised instead. A zero disk byte limit restores the
256 MiB default.

`PacketBufferStatus::bufferedMilliseconds` is the minimum usable duration
across selected audio/video streams, not their sum; `bufferedBytes` is the
combined compressed size across both storage tiers. `memoryBufferedBytes` and
`diskBufferedBytes` split that total, while `diskCachePath` and
`packetDiskCachePath()` expose the current volatile file path. `reason`
distinguishes initial playback, seek, track switch, runtime underflow, and
network recovery, and `presentationGeneration` rejects stale UI updates.
Playback is released when
every active stream reaches its target or EOF. If the fixed packet, duration,
or byte capacity is reached first, the completed snapshot sets
`capacityLimited` instead of deadlocking the interleaved demuxer behind one
full stream. Progress callbacks are coalesced at approximately one-percentage-
point changes and always report start, completion, and cancellation.

`clearPacketDiskCache()` removes the cache synchronously. During seekable
playback it also discards the in-memory compressed prefetch and requests a
keyframe seek from the current position, because the demuxer may already be
ahead of packets just removed from disk. It returns false without changing an
active non-seekable input; stop that input first if its cache must be cleared.

Packet buffering changes `MediaStatus` to `Buffering` and freezes playback
time until output from the released generation really resumes. The same media
status is also used for audio-device clock re-anchoring, so use the packet
status callback when a UI needs packet-specific progress. This policy does not
drop compressed packets or decoded frames or choose an adaptive bitrate. The
network policy below reuses the reservoir only after an input has reopened;
the decoded-video policy remains separate.

### Recoverable network I/O

Network recovery has two bounded layers. FFmpeg's HTTP(S) protocol still uses
the default 15-second `rw_timeout`, early-disconnect reconnect, connect-error
reconnect, five retries, two-second maximum delay, and ten-second total delay.
Applications may override that protocol layer through `avformat.*` properties.
If open or `av_read_frame()` nevertheless returns a recoverable error,
`NetworkRecoveryPolicy` controls a fresh Player-level input open:

```cpp
qtav::NetworkRecoveryPolicy recovery;
recovery.enabled = true;
recovery.maximumAttempts = 3;
recovery.initialRetryDelayMilliseconds = 250;
recovery.maximumRetryDelayMilliseconds = 2'000;

player
    .setNetworkRecoveryPolicy(recovery)
    .onNetworkRecoveryStatus(
        [](const qtav::NetworkRecoveryStatus& status) {
            update_network_ui(
                status.state,
                status.attempt,
                status.retryDelayMilliseconds,
                status.error);
        });
```

Player-level recovery is enabled by default for recognized HTTP(S), FTP,
TCP/TLS, UDP/RTP/RTSP, RTMP, RIST, and SRT URLs. `maximumAttempts` bounds fresh
opens within one continuity interval and is normalized to 1–32. Negative
delays become zero; the maximum delay is raised to at least the initial delay.
Backoff is exponential and capped, so the policy is bounded without a separate
total-time setting. Read failures that recur before the input's selected demux
timestamps advance by 500 ms continue consuming the same attempt budget, even
if an intervening reopen returned an early usable packet. Seek, track change,
or media replacement starts a new continuity interval. Disabling
`NetworkRecoveryPolicy` leaves only the selected FFmpeg protocol behavior. The
common permanent HTTP 400, 401, 403, and 404 errors and local
allocation/configuration errors are not retried.

`NetworkRecoveryStatus` reports `Waiting`, `Reopening`, `Recovered`, `Failed`,
or `Idle`, the failed operation and input, URL, one-based attempt, delay, error,
resume position, and presentation generation. A stop, media replacement, seek,
pause, prepare, or track change cancels a read-recovery backoff through the
normal control worker instead of waiting for its timer. An in-progress FFmpeg
I/O call retains the existing interrupt rules; stop, seek, and media changes
advance the interrupt epoch immediately. The recovery callback runs on the
playback worker and follows the ordinary rule that it may request control but
must not destroy the Player inline.

After a read failure, a replacement input must expose compatible selected
stream indices, media types, and codec IDs. Seekable inputs reopen at the
current media position. Non-seekable inputs resume from the newly connected
server edge and offset their normalized timestamps from the frozen position.
The fresh open remains `Reopening` until it returns the first selected,
non-corrupt packet or a clean EOF; an immediate post-open read failure consumes
the same bounded attempt budget instead of starting an unlimited sequence of
nominally successful reopens. That first usable packet is retained for the
replacement generation. Installing the replacement invalidates the old
presentation generation, flushes decoder/device queues, realigns other
seekable active inputs, and enters
`PacketBufferingReason::NetworkRecovery` until the selected A/V reservoir and
actual output restart. An unrecoverable error or exhausted policy emits
`network.recovery.failed`, transitions the media to `Invalid`, and preserves
the terminal recovery snapshot for diagnostics.

`PlaybackStatistics::networkRecoveryAttempts`,
`successfulNetworkRecoveries`, and `failedNetworkRecoveries` count Player-level
work only; retries hidden inside a protocol are intentionally not double-
counted. This is input continuity, not adaptive rendition selection, a
persistent download/cache, or packet-history repair.

### Low-latency live video

`LivePlaybackPolicy` is disabled by default, preserving the ordinary playback
policy: software and hardware frames use their existing hard queue bounds, a
full queue rejects a farther-future arrival, and a video frame more than 250 ms
late is discarded only when a timely newer frame can replace it. Enable the
policy for live or other latency-sensitive playback to keep a smaller newest-
frame window instead. When that window is full, an incoming newer frame
supersedes its oldest queued video frame; once a frame is late by the configured
threshold, the presentation worker skips it whenever any newer video frame for
the same generation is waiting.

```cpp
qtav::LivePlaybackPolicy live;
live.enabled = true;
live.maximumQueuedVideoFrames = 2;
live.lateVideoFrameThresholdMilliseconds = 80;
player.setLivePlaybackPolicy(live);
```

The queue depth is normalized to 1–24 and a negative late threshold becomes
zero. Policy changes are thread-safe and affect subsequent queue/presentation
decisions without reopening media. `PlaybackStatistics::videoQueueOverflowDrops`
continues to count every bounded video-queue drop, while
`lowLatencyVideoQueueDrops` identifies the subset made by this latest-window
policy; `lateVideoDrops` remains the presentation-time count.

The policy intentionally drops only decoded video. It does not discard
compressed packets, audio, or subtitles, alter packet-buffer accounting, seek
the input, or select an adaptive rendition. Network input recovery is handled
by the separate policy above. This keeps audio/device time as the master while
video catches up without corrupting decoder reference chains. If
`setVideoFrameScheduler()` accepts a frame, the
application has taken over scheduling before the presentation queue and must
apply any direct-surface/native-buffer drop policy itself.

`onSubtitleFrame()` publishes decoded UTF-8 plain text for FFmpeg text and
ASS/SSA subtitle decoders. ASS event fields and override blocks are removed
from `text()`, and `\\N`/`\\n` line breaks plus `\\h` spaces are normalized.
The same reference-counted `SubtitleFrame` preserves FFmpeg's ASS packet
events and codec-private header through `assEvents()` and `assHeader()` and
also carries the cue timestamp, duration, forced flag, track identity, and
presentation generation. Public core headers still expose no FFmpeg or
libass types.

For styled text subtitles, link `QtAV::SubtitleLibass`, configure its output
and source-video sizes, add every callback frame once, then render at the
current media position from the application's composition path:

```cpp
#include <qtav/libass_subtitle_renderer.h>

qtav::LibassSubtitleRenderer subtitles;
qtav::LibassSubtitleRendererConfig subtitleConfig;
subtitleConfig.frameWidth = surfaceWidth;
subtitleConfig.frameHeight = surfaceHeight;
subtitleConfig.storageWidth = videoWidth;
subtitleConfig.storageHeight = videoHeight;
subtitles.configure(subtitleConfig);

player.onSubtitleFrame(
    [&](const qtav::SubtitleFrame& frame, int) {
        if (subtitles.add(frame)) {
            schedule_video_redraw();
            schedule_redraw_at(frame.timestamp() + frame.duration());
        }
    });

const auto overlay = subtitles.render(player.position());
for (const auto& image : overlay.images) {
    // Composite in vector order. Per-pixel source alpha is
    // image.bitmap[y * image.stride + x] * image.opacity / 255.
    composite_subtitle_coverage(image);
}
```

The rasterizer is thread-safe, copies libass's transient image list into
owning bitmaps, and automatically starts a new internal track when the next
frame has a different track identity or presentation generation. Call `flush()`
immediately when the application initiates a seek or media replacement so no
old cue is drawn while waiting for the first new-generation frame. Render on
video redraws (and timer ticks for animated ASS content), preserve image order,
and schedule a redraw at cue end so static/paused composition clears on time.
Bitmap subtitle rectangles remain decoded but are not exposed or rendered by
this text/ASS module; graphical subtitle delivery is separate work.

To capture decoded audio as inspectable PCM, link `QtAV::AudioFile` and
`QtAV::AudioResample`:

```cpp
#include <qtav/swresample_audio_converter.h>
#include <qtav/wav_audio_sink.h>

auto sink = std::make_shared<qtav::WavAudioSink>(
    qtav::WavAudioSinkConfig {
        "capture.wav",
        48'000,
        2,
        qtav::SampleFormat::S16,
        "stereo",
    });

player
    .setAudioFrameConverter(
        std::make_shared<qtav::SwresampleAudioConverter>())
    .setAudioSink(sink);
```

### Pitch-preserving playback rate

Link `QtAV::AudioTimeStretch` and inject its streaming processor when device
audio must follow `setPlaybackRate()` without changing pitch:

```cpp
#include <qtav/atempo_audio_time_stretcher.h>

player.setAudioTimeStretcher(
    std::make_shared<qtav::AtempoAudioTimeStretcher>());
player.setPlaybackRate(1.5F);
```

The processor sits after `AudioFrameConverter` and before `AudioSink`. It
keeps the negotiated PCM format, changes the physical sample count, and keeps
each output buffer's timestamp and duration on the media timeline. Player
maps the sink's physical device-clock delta back to media time by the active
rate, so audio remains the A/V master. A rate change on seekable loaded media
flushes the old device queue and accurately re-decodes from the current
position before reopening the processor at the new rate.

Rate 1.0 is an exact Player bypass: the processor is not opened or called.
For a non-1.0 rate without a configured processor, decoded frame callbacks
continue, but device audio is disabled and Player reports
`audio.time_stretch.unavailable`; this avoids presenting unstretched PCM as an
incorrect A/V master. `AtempoAudioTimeStretcher` accepts interleaved U8, S16,
S32, Float32, and Float64 PCM and composes bounded `atempo` stages for rates
outside one filter's native range.

Pause/resume preserves processor and queued-device state. Prepare, seek,
track/external-audio changes, loop or range transitions, media replacement,
and stop discard buffered processor state; natural segment end drains the
converter, then the time stretcher, then the sink. Calls are serialized with
conversion and sink writes on the existing audio-output/lifecycle boundary.

### General audio and video processing

`AudioFrameProcessor` is the first general audio-effect boundary. Player opens
it with the sink's negotiated PCM format after conversion and time stretch, and
calls it before `AudioSink::write()`. A processor may buffer input and return
zero or more processor-owned `AudioBufferView` objects. Output must preserve the
opened format, media-timeline order, and the completed segment's physical sample
count. This supports gain, equalization, channel balance, and analysis with
passthrough output without assigning device ownership or playback-rate control
to the effect. Decoded `onAudioFrame()` callbacks remain upstream and receive
the original decoder PCM.

`VideoFrameProcessor` is intentionally narrower: it is synchronous and
one-input/one-output on the video-decode worker. Player calls an accepted
`setVideoFrameScheduler()` first because direct codec-surface presentation owns
that frame. If the scheduler declines it, the processor runs before the normal
presentation queue, `onVideoFrame()`, and renderers. A non-bypass result may
change pixels, software format, geometry, and color metadata, but it must return
one valid frame with exactly the input timestamp and duration. Queued cadence
conversion is not part of this contract, and graphics-context effects remain a
`VideoRenderAPI` responsibility on the application's render thread.

Both contracts are optional and their public headers contain no FFmpeg,
graphics, Qt, or platform SDK type. Pause/resume preserves state. Prepare,
seek, loop/range discontinuities, and stop reset buffered state; track/media or
live processor replacement closes and reopens the affected stage at a clean
generation boundary. Natural completion drains downstream in order. Format-
level and per-frame video bypass are explicit. Contract violations and backend
failures report `audio.processor.*` or `video.processor.*` events and close the
affected output path instead of silently forwarding partially processed data.

For a minimal reference implementation, link `QtAV::AudioFilter`:

```cpp
#include <qtav/volume_audio_frame_processor.h>

player.setAudioFrameProcessor(
    std::make_shared<qtav::VolumeAudioFrameProcessor>(0.5));
```

`VolumeAudioFrameProcessor` owns an FFmpeg `volume` graph, accepts interleaved
U8, S16, S32, Float32, or Float64 PCM, and applies a fixed non-negative linear
gain without changing format, sample count, timestamps, or duration. It does
not expose arbitrary FFmpeg graph strings; applications needing a different
effect implement the core contract or provide another optional backend.

### WASAPI device sink

Link `QtAV::AudioWASAPI` and `QtAV::AudioResample`, then bind the Windows
shared-mode device sink:

```cpp
#include <qtav/swresample_audio_converter.h>
#include <qtav/wasapi_audio_sink.h>

player
    .setAudioFrameConverter(
        std::make_shared<qtav::SwresampleAudioConverter>())
    .setAudioSink(
        std::make_shared<qtav::WasapiAudioSink>());
```

The default configuration resolves the current default multimedia render
endpoint whenever the sink opens. `WasapiEndpointId` can select an explicit
endpoint without passing an apartment-bound COM interface across threads.
The initial implementation negotiates one or two interleaved Float32 channels
at the endpoint mix rate and relies on `SwresampleAudioConverter` for decoded
format conversion. A dedicated multimedia-class thread owns COM, the
event-driven `IAudioClient`, copied PCM queue, pause/flush/drain lifecycle, and
the `IAudioClock` media-timeline anchor. A device-clock underrun temporarily
returns an invalid clock so `Player` can use its monotonic fallback until the
next buffer establishes a new anchor.

### AAudio device sink

Link `QtAV::AudioAAudio` and `QtAV::AudioResample` in an Android application
whose minimum API is 26 or newer:

```cpp
#include <qtav/aaudio_audio_sink.h>
#include <qtav/swresample_audio_converter.h>

player
    .setAudioFrameConverter(
        std::make_shared<qtav::SwresampleAudioConverter>())
    .setAudioSink(
        std::make_shared<qtav::AAudioAudioSink>());
```

The default configuration follows the current Android output route, requests
low-latency shared mode, and negotiates mono/stereo interleaved Float32 PCM.
Decoded PCM is copied on the audio-output worker into a fixed-capacity
single-producer/single-consumer queue; AAudio's real-time data callback only
performs lock-free queue reads, bounded memory copies, silence fill, and atomic
state updates. It never allocates, locks, sleeps, or opens/closes a stream.

`clock()` maps `AAudioStream_getTimestamp(CLOCK_MONOTONIC)` frame positions to
the media timestamp of the corresponding queued frame. Reported latency
combines callback/device pipeline frames with backend-queued PCM. Pause,
flush, drain, underrun re-anchoring, and stream teardown follow the generic
`AudioSink` lifecycle. If Android disconnects the stream during a default
route change, a separate backend worker closes and reopens it with the same
negotiated PCM format; successful recovery emits an underrun event so
`Player` buffers until a new device-clock anchor is available. A failed
restart emits `DeviceLost`.

`AAudioStreamInfo` exposes the current device identifier, burst/capacity,
xrun count, transparent route-change count, and disconnect-restart count for
diagnostics. The backend requires no OpenSL ES fallback at the current API 28
baseline.

### OHAudio device sink

Link `QtAV::AudioOHAudio` and `QtAV::AudioResample` in an OHOS application
whose minimum native platform is API 23 or newer:

```cpp
#include <qtav/ohaudio_audio_sink.h>
#include <qtav/swresample_audio_converter.h>

player
    .setAudioFrameConverter(
        std::make_shared<qtav::SwresampleAudioConverter>())
    .setAudioSink(
        std::make_shared<qtav::OHAudioAudioSink>());
```

The default configuration requests the current route in fast mode and
negotiates 48 kHz mono/stereo interleaved Float32 PCM, falling back to normal
latency mode if fast renderer construction is unavailable. Accepted PCM enters
the same preallocated SPSC implementation used by the Android backend. The
OHAudio write callback performs only bounded copies, silence fill, and atomic
updates; stream construction, teardown, route recovery, and event delivery
remain on non-callback threads.

`clock()` maps `OH_AudioRenderer_GetAudioTimestampInfo()` hardware-committed
frame positions to media timestamps. Its latency combines frames submitted to
OHAudio but not yet committed with backend-queued PCM. Pause, flush, natural-
end drain, underrun re-anchoring, forced audio interruptions, and teardown
follow the generic `AudioSink` lifecycle. Output-device changes and native
errors rebuild the negotiated stream on the backend worker, discard the old
queued generation, and emit `Underrun` after successful recovery or
`DeviceLost` if reconstruction fails.

`OHAudioStreamInfo` exposes callback size, native frames/underflows, accepted
PCM, lifecycle counters, route changes, and stream restarts for diagnostics.
The connected Mate 60 Pro harness has validated native 440 Hz AAC output,
device-master clock samples, non-negative combined latency, pause/resume,
seek/flush, loop-boundary drain, and clean continuation through the existing
Vulkan/OpenGL ES fallback scenario. Automated counters establish delivery and
hardware timing; subjective audibility still requires a human listening check.

### OHCodec direct-surface hardware decode

Link `QtAV::HWOHCodec` in an OHOS application. Create a new
`OHCodecSurface` for every ArkUI-published `OHNativeWindow` generation, set
the returned configuration before opening H.264, HEVC, or VVC media, and consume
each surface output from the decode-worker scheduler:

```cpp
#include <qtav/ohcodec_hardware_decoder.h>

qtav::OHCodecSurface surface(nativeWindow);
qtav::OHCodecHardwareDecodeOptions options;
options.allowSoftwareFallback = false;

player
    .setHardwareDecodeConfig(
        qtav::ohCodecHardwareDecodeConfig(surface, options))
    .setVideoFrameScheduler(
        [&surface](
            const qtav::VideoFrame& frame,
            int,
            std::int64_t monotonicNanoseconds) {
            auto output = qtav::ohCodecFrame(frame, surface);
            return output
                && output.presentAt(monotonicNanoseconds);
        });
player.setMedia(path);
player.setState(qtav::State::Playing);
```

The backend retains the native window independently of ArkUI, creates an
FFmpeg `AV_HWDEVICE_TYPE_OHCODEC` device for it, selects the explicit
`*_ohcodec` wrapper, and tags decoded hardware frames with the exact surface
generation. `OHCodecFrame` is a move-only, single-decision token. `present()`
releases immediately to the configured window, `presentAt()` uses a
`CLOCK_MONOTONIC` nanosecond timestamp, and `drop()` releases without display.
Destroying an undecided token drops it. If an output instead reaches its final
retained frame release without an explicit token decision, the FFmpeg overlay
also unconditionally drops/frees it; abandonment never implies presentation.
`ohCodecFrame()` rejects stale or foreign window generations.

For VVC, the repository FFmpeg overlay registers `vvc_ohcodec`, maps the OHOS
VVC MIME, and applies `vvc_mp4toannexb` to MP4 `vvc1` input. The wrapper first
queries the hardware capability category and never silently selects an
OHCodec software implementation. Leave `allowSoftwareFallback` enabled when
the application should reopen unsupported or failed hardware sessions with
FFmpeg's native software VVC decoder.

The repository FFmpeg overlay supplies the narrow opaque release API required
to make that decision without copying FFmpeg's private decoder state. It is a
surface-output API only: it does not expose an `OH_AVBuffer` or establish
Vulkan/OpenGL ES native-buffer interop. The Player applies the same bounded
packet-feed and no-deep-output-queue scheduling policy used by Android
MediaCodec. On surface loss, pause playback, clear the scheduler and hardware
configuration, let retained tokens finish or drop, publish a new
`OHCodecSurface`, and resume with the replacement configuration.

The complete signed HAP passed on 2026-08-05 on a Mate 60 Pro (`ALN-AL80`),
HarmonyOS 6.1.0.135 / OpenHarmony 6.1.1.120 API 24. With software fallback
disabled, H.264 presented 48 outputs and dropped 5; HEVC presented 40 and
dropped 5. The run passed pause/resume, a 2000 ms target/callback seek, media
replacement, explicit stop, background and foreground transitions, one
surface recreation, and one stale-generation rejection. Output retention was
bounded at `maxPending=2`; `pendingAtStop=1` drained to `pendingEnd=0`, and
`maxQueued=0`. This completes the direct-surface lifecycle matrix. The
separate `QtAV::InteropOHCodecVulkan` target now implements strict retained
`OH_NativeBuffer` import, while `QtAV::InteropOHCodecOpenGL` provides the
raw-component OpenGL ES route. The latter exposes raw components through
`GL_EXT_YUV_target` and does not rely on implicit external-OES conversion.

On 2026-08-08 the signed Pura X Max VVC mode selected
`OMX.hisi.video.decoder.vvc` and presented the complete 600-frame
1280x720/60 `vvc1` sample. The lifecycle accumulated 694 VVC hardware frames
across EOS, pause/resume, a 4000 ms seek/flush, explicit stop, XComponent
surface recreation, and stale-generation rejection, with `maxPending=1` and
`maxQueued=0`. A forced missing supplied device then reopened the same media
through FFmpeg software VVC, delivered 30 frames, emitted exactly one hardware-
fallback event, and accepted no stale hardware frame.

### OHCodec OpenGL ES raw-component interop

Link `QtAV::HWOHCodec`, `QtAV::RenderOpenGLOHOS`, and
`QtAV::InteropOHCodecOpenGL`. The interop supplies an `OH_NativeImage`
producer window to OHCodec, retains the exact scheduled `VideoFrame`, and
matches each image timestamp against the queued presentation timestamp after
checking the device's observed microsecond and nanosecond forms. It samples
raw Y/Cb/Cr through `GL_EXT_YUV_target` into an RGBA16F representation texture;
libplacebo remains responsible for Dolby Vision reshaping, color conversion,
tone/gamut mapping, scaling, and output encoding. Because that representation
texture is rendered into an OpenGL framebuffer, its libplacebo input plane is
marked vertically flipped; the EGL output framebuffer keeps normal OpenGL
orientation semantics.

The repository FFmpeg overlay enables the HEVC parser and built-in RPU decoder
for `hevc_ohcodec`. It parses RPU NAL units before packet submission, keys the
result by the exact microsecond PTS passed to OHCodec, and attaches
`AV_FRAME_DATA_DOVI_METADATA` only to the output carrying that PTS. Seek,
decoder flush, media replacement, and close clear both the parser state and
the bounded pending-metadata queue.

`OHCodecOpenGLInteropStatistics` reports
`dolbyVisionFramesQueued`, `dolbyVisionTimestampMatches`, and
`dolbyVisionFramesReleased`. These counters prove that the same metadata-
bearing frame survived native-image association and stayed alive until image
release; a profile validation fails when any rendered HEVC frame lacks that
chain. On 2026-08-06 the signed Mate 60 Pro HAP passed Profile 5 and the
checksum-pinned FFmpeg FATE Profile 8.4 sample with 45 rendered HEVC frames per
run and `45/45/45` queued/matched/released RPU counts. Both runs used raw YCbCr,
zero implicit-RGB images, and zero decoded-source CPU map, software transfer,
staging, or renderer upload calls. The Profile 8.4 MMR path also exercises the
repository libplacebo GLES shader-index correction.

### OHCodec Vulkan native-buffer interop

Link `QtAV::HWOHCodec`, `QtAV::RenderVulkanOHOS`, and
`QtAV::InteropOHCodecVulkan`. The application-created Vulkan device must
enable `VK_OHOS_external_memory`, `VK_EXT_queue_family_foreign`, and
`VK_KHR_external_semaphore_fd`, enable `samplerYcbcrConversion`, then declare
those facts in the interop
configuration:

```cpp
#include <qtav/ohcodec_vulkan_interop.h>

qtav::OHCodecVulkanInteropConfig interopConfig;
interopConfig.width = 1920;
interopConfig.height = 1080;
interopConfig.ohosExternalMemoryEnabled = true;
interopConfig.foreignQueueFamilyEnabled = true;
interopConfig.syncFdSemaphoreEnabled = true;
interopConfig.samplerYcbcrConversionEnabled = true;
// Leave externalFormatWorkaroundEnabled at false. Numeric reinterpretation is
// retained only for explicit driver diagnostics.

auto interop = std::make_shared<qtav::OHCodecVulkanInterop>(
    borrowedVulkanDevice,
    interopConfig);
renderer->setHardwareFrameInterop(interop);
player.setHardwareDecodeConfig(
    qtav::ohCodecHardwareDecodeConfig(
        interop->surface(),
        { false, 8 }));
```

`nativeBufferObservation()` returns the most recently acquired decoder
allocation's dimensions, stride, usage, NativeBuffer color-space query result,
Vulkan `formatFeatures`, optimal-tiling features, allocation size, and memory-
type bits. These fields are diagnostic inputs to the strict explicit-plane
gate; they do not reinterpret an opaque external ID or map the decoded buffer.
OHOS's public NativeBuffer/Vulkan import contract does not expose the vendor
compression mode or an allocation modifier, so the connected-device marker
reports both as `not-exposed` instead of inventing a value. The existing
`OHCodecVulkanInteropStatistics` layout remains unchanged.

The interop owns a private `OH_ConsumerSurface`. Surface-mode OHCodec outputs
do not expose native memory through their `OH_AVBuffer`; the interop therefore
presents exactly one retained output into that surface, waits for its frame
callback, acquires and retains the corresponding `OHNativeWindowBuffer`, and
converts it to `OH_NativeBuffer`. An acquire sync fd is imported into a Vulkan
semaphore. Direct-plane buffers remain retained through renderer GPU
completion. For opaque input, normalization completes first and the consumer
buffer is then returned immediately; immutable sampler/conversion state has an
independent lifetime and cannot keep a one-buffer consumer queue exhausted.
If seek, stop, or media replacement invalidates an output while it is awaiting
that callback, the interop acquires and returns the invalidated consumer buffer
as soon as it becomes available. A newer OHCodec output is never correlated
against or blocked permanently behind that retired frame.

Explicit sampled multi-planar `VkFormat` values are passed directly to
`pl_vulkan_wrap` only when the OH native format has the same raw component and
plane order. In particular, YCrCb/VU buffers are not silently accepted as
YCbCr/UV direct-plane inputs. For `VK_FORMAT_UNDEFINED` plus an external-format
ID, the production path keeps the ID opaque and uses `VkExternalFormatOHOS`.
Ordinary input is sampled with the driver-suggested model, range, component
mapping, chroma offsets, and filter. Opaque Dolby Vision input additionally
requires an `RGB_IDENTITY` conversion with the same driver component mapping.
Vulkan exposes raw `(Cr, Y, Cb)` in sampled `(R, G, B)` order, so the GPU
normalizer stores `.gbr` as `(Y, Cb, Cr)` only for that identity route before
libplacebo performs Dolby Vision and color processing. Suggested-conversion RGB
samples remain `.rgb`.

`externalFormatWorkaroundEnabled` defaults to `false`. Setting it to `true`
requests the historical closed-allowlist numeric reinterpretation and is
diagnostic compatibility only. The forced native/libplacebo modes remain
available for the same purpose. Opaque production sampling avoids decoded-
source mapping, software transfer, staging, and upload, but its GPU
normalization image is not strict no-intermediate source zero copy.

The connected Mate 60 Pro probe on 2026-08-06 exercised real H.264/NV12,
HEVC/NV12, and HEVC Main10/P010 outputs. The queried Vulkan format remained
`VK_FORMAT_UNDEFINED`; NV12 exposed external format `1000156003` and P010
exposed `1000156013`. A raw Vulkan object probe succeeded through YCbCr
conversion, sampler, image/memory import, image view, and immutable descriptor
layout. The full HAP then emitted
`QTAV_OHOS_OHCODEC_VULKAN_RESULT PASS mode=opaque-ycbcr-normalized` after 30
H.264 and 30 HEVC shader-sampled frames: `acquired=60`, `imported=60`,
`opaqueImports=60`, `normalization=60`, with zero CPU map, software transfer,
staging, or upload.

Two diagnostic-only modes then tested Huawei's proposed numeric
reinterpretation. They omit `VkExternalFormatOHOS` from image creation and set
`VkImageCreateInfo::format` to
`VK_FORMAT_G8_B8R8_2PLANE_420_UNORM` for `1000156003`, or
`VK_FORMAT_G10X6_B10X6R10X6_2PLANE_420_UNORM_3PACK16` for `1000156013`.
Application-owned Vulkan YCbCr sampling passed 30 H.264/NV12 plus 30 HEVC
Main10/P010 frames. Direct `pl_vulkan_wrap` with libplacebo 7.351.0 also passed
all 60 frames with `directPlanes=60`, `normalization=0`, and zero CPU map,
transfer, staging, or upload. This proves only that the named driver and
libplacebo can consume the forced explicit formats. Huawei's formal 2026-08-12
reply identifies the numerical equality as an internal implementation detail,
confirms that the tested driver has no explicit-format mode or switch, and
directs applications to `VkExternalFormatOHOS`. It also confirms identity
raw-component sampling, P010 10-bit preservation, and the queried range/chroma
properties. Production therefore uses the opaque path; strict direct-plane use
remains a separate device gate.

The final signed 2026-08-12 Mate 60 Pro player run kept `legend.mkv` on
OHCodec/Vulkan at 25.0 FPS and kept Dolby Vision `wednesday.mp4` on
OHCodec/Vulkan at 24.1 FPS. Forced-SDR captures first verified correct color
for both suggested-RGB and raw-identity sampling. The same-process snapshot
ended with 1,988 opaque imports, normalization passes, releases, and frame
callbacks, zero numeric-workaround imports,
`lastVulkanFormat=VK_FORMAT_UNDEFINED`, and zero Player drops.

The same connected harness now exercises the shared selector with real
OHCodec frames. After eight successful opaque Vulkan imports it injects a
bounded fatal result, retires the Vulkan consumer surface, prepares an
`OH_NativeImage` OpenGL ES candidate, and synchronously
rebinds subsequent decoder output to the new surface without reopening the
media. The 2026-08-06 run changed generation 5 to 6 and rendered 30 raw-YCbCr
OpenGL ES frames. A separate session selected the independent software-decode
route: it cleared `HardwareDecodeConfig`, discarded the retired hardware frame,
then rendered 30 software frames through OpenGL ES. Both paths kept decoded-
source CPU map, transfer, staging, and upload counters at zero. Direct-surface
and no-video remain explicit application alternatives covered by deterministic
selector tests.

### MediaCodec direct-surface hardware decode

Link `QtAV::HWMediaCodec` in an Android application. Create a new
`MediaCodecSurface` token for every application-published `ANativeWindow`
generation, then select the explicit FFmpeg wrapper decoder before playing:

```cpp
#include <qtav/mediacodec_hardware_decoder.h>

qtav::MediaCodecSurface surface(nativeWindow);
qtav::MediaCodecHardwareDecodeOptions options;
options.allowSoftwareFallback = false;

player
    .setHardwareDecodeConfig(
        qtav::mediaCodecHardwareDecodeConfig(surface, options))
    .setVideoFrameScheduler(
        [&surface](
            const qtav::VideoFrame& frame,
            int,
            std::int64_t monotonicNanoseconds) {
            auto output = qtav::mediaCodecFrame(frame, surface);
            return output
                && output.presentAt(monotonicNanoseconds);
        });
player.setMedia(path);
player.setState(qtav::State::Playing);
```

The helper selects FFmpeg's `*_mediacodec` wrapper by wrapper identity instead
of relying on the default software decoder lookup. `MediaCodecFrame` is a
move-only, single-decision token. `present()` releases immediately to the
configured surface, `presentAt()` uses a `CLOCK_MONOTONIC` nanosecond
timestamp, and `drop()` releases without display. Destroying an undecided last
frame copy lets FFmpeg drop it. Hardware-frame storage retains the decoder
context until all copied outputs are released, including across seek, stop,
media replacement, and asynchronous decoder reopen.

`setVideoFrameScheduler()` runs on the video-decode worker once a decoded frame
is inside its bounded lead window and supplies the target monotonic presentation
time. Returning `true` means the application consumed that frame and suppresses
the later `onVideoFrame()`/renderer path. This is the preferred direct-Surface
integration because the codec owns only a small output pool.

On surface loss, pause playback, publish an empty hardware-decode
configuration, and release the old token only after application-held frame
copies are no longer being used. Create a new `MediaCodecSurface` for the new
window, publish its configuration, and resume. The generation and native
window identity are copied into each output; `mediaCodecFrame()` rejects a
stale or foreign token. The direct-surface path does not expose a
shader-readable texture and does not imply Vulkan/OpenGL ES interop.

### MediaCodec Vulkan zero-CPU-copy texture interop

Link `QtAV::HWMediaCodec`, `QtAV::RenderVulkanAndroid`, and
`QtAV::InteropMediaCodecVulkan`. Before creating the application-owned
logical device, enable
`VK_ANDROID_external_memory_android_hardware_buffer`,
`VK_KHR_external_semaphore_fd`, `VK_EXT_queue_family_foreign`, and the
sampler-YCbCr-conversion feature. Construct the interop, give its private
surface to MediaCodec, and attach the same interop object to the Vulkan
renderer:

```cpp
#include <qtav/android_vulkan_video_renderer.h>
#include <qtav/mediacodec_vulkan_interop.h>

qtav::MediaCodecVulkanInteropConfig interopConfig;
interopConfig.maximumImages = 5;
interopConfig.androidHardwareBufferExternalMemoryEnabled = true;
interopConfig.externalSemaphoreFdEnabled = true;
interopConfig.samplerYcbcrConversionEnabled = true;
interopConfig.foreignQueueFamilyEnabled = true;

auto interop = std::make_shared<qtav::MediaCodecVulkanInterop>(
    borrowedVulkanDevice,
    interopConfig);
auto renderer = std::make_shared<qtav::AndroidVulkanVideoRenderer>(
    borrowedAndroidVulkanContext,
    qtav::VulkanOutputPreference::PreferHdr);
renderer->setHardwareFrameInterop(interop);
renderer->setWindow(nativeWindow);

player
    .setVideoRenderAPI(renderer)
    .setVideoFrameScheduler(
        [interop](
            const qtav::VideoFrame& frame,
            int,
            std::int64_t monotonicNanoseconds) {
            std::string detail;
            if (!interop->queueFrame(frame, detail)) {
                return false;
            }
            retain_and_schedule_render(
                frame,
                monotonicNanoseconds);
            return true;
        })
    .setHardwareDecodeConfig(
        qtav::mediaCodecHardwareDecodeConfig(interop->surface()))
    .setState(qtav::State::Playing);
```

The interop creates an `AIMAGE_FORMAT_PRIVATE` reader with
`AHARDWAREBUFFER_USAGE_GPU_SAMPLED_IMAGE`. Releasing a MediaCodec token to that
surface produces an asynchronous `AImage`; codec and image timestamps are
matched before its retained `AHardwareBuffer` is imported. Timestamp zero is
valid and is correlated normally; only negative image timestamps are rejected.
The config's optional `width` and `height` are only the reader's default
dimensions; when omitted, the interop uses `1x1`. MediaCodec overrides that
default with its decoded output size, and every import uses the actual
`AImage` dimensions and crop, so applications do not need a separate media
probe before creating the zero-CPU-copy path.
`queueFrame()` performs a bounded wait for the matching asynchronous image to
be acquired, serializing producer ownership transfer without waiting for the
playback deadline or Vulkan submission. Applications should reserve their own
bounded render slot before calling it, retain the exact `VideoFrame`, and
render at the scheduler-provided deadline.
Vulkan uses the driver-reported explicit format or opaque external format. The
renderer also applies the `AImage` crop rectangle, so codec-aligned native
allocations may be larger than the visible decoded frame.

When the driver reports an explicit `VkFormat` and plane mapping, the retained
decoder allocation can be wrapped directly for libplacebo; only that direct
case qualifies as strict no-intermediate source zero-copy. Android private
MediaCodec buffers on tested devices instead expose an opaque external format,
so the interop performs one GPU-only sampling pass into an FP16 Vulkan texture,
applying only raw representation normalization and the visible crop. Dolby
Vision requires an identity/raw-component sampling contract so libplacebo
receives the base-layer signals associated with FFmpeg's RPU metadata. No
matrix, transfer, gamut mapping, tone mapping, HDR output encoding, or RPU
processing belongs in this normalization pass; imports that cannot preserve
raw components must be rejected for this path. The opaque-format path performs
no decoded-source CPU map, transfer, staging, or upload, but its intermediate
texture means it is zero-CPU-copy rather than strict source zero-copy.

When an acquire sync fd is present it is imported into a temporary Vulkan
semaphore. Submission waits on that semaphore and transfers ownership from
the foreign queue family; completion signals an exportable semaphore whose
sync fd is returned with `AImage_deleteAsync()`. The imported object retains
the image and decoder output through the submission fence. Vulkan image,
memory, view, and YCbCr-conversion resources are cached by retained
`AHardwareBuffer` identity and retired by the AImageReader buffer-removed
callback; per-frame acquire/release semaphores remain synchronized with the
individual image. When this interop is attached through `Player` and
`VulkanVideoRenderer`, seek, decoder/media replacement, and explicit stop
automatically invalidate queued images and timestamp associations that have not
entered a submission. Direct standalone interop users can still call `flush()`
at their own lifecycle boundary.

`statistics()` exposes queue depth, import/fence counts, last native/Vulkan
format, decoded-source map/transfer/staging/upload counters, and persistent
hardware-buffer import/cache-hit/removal/high-water counters. Unsupported or
protected images fail explicitly. The implementation never calls
`AHardwareBuffer_lock*()` and has no implicit software-frame fallback; decoder
fallback and renderer/API fallback remain application policies.

### MediaCodec OpenGL ES zero-CPU-copy texture interop

Link `QtAV::HWMediaCodec`, `QtAV::RenderOpenGLAndroid`, and
`QtAV::InteropMediaCodecOpenGL`. Supply the NativeActivity's `JavaVM`, create
the interop before opening the renderer, bind MediaCodec to the interop's
private producer surface, and route renderer `RedrawRequested` events back to
the application's normal `renderVideo()` scheduling:

```cpp
#include <qtav/android_opengl_video_renderer.h>
#include <qtav/mediacodec_opengl_interop.h>

qtav::MediaCodecOpenGLInteropConfig interopConfig;
interopConfig.javaVM = nativeActivity->vm;

auto interop = std::make_shared<qtav::MediaCodecOpenGLInterop>(
    interopConfig);
auto renderer = std::make_shared<qtav::AndroidOpenGLVideoRenderer>(
    qtav::OpenGLOutputPreference::SdrOnly);
renderer->setHardwareFrameInterop(interop);
renderer->setWindow(nativeWindow);
qtav::VideoRenderConfig renderConfig;
renderConfig.surfaceSize = renderer->surfaceSize();
renderer->open(renderConfig);

player
    .setVideoRenderAPI(renderer)
    .setHardwareDecodeConfig(
        qtav::mediaCodecHardwareDecodeConfig(interop->surface()))
    .setState(qtav::State::Playing);
```

The interop creates a private GPU-sampled `AImageReader` and retains its
producer `ANativeWindow`. MediaCodec output timestamps are correlated with
acquired PRIVATE images. On the renderer thread each retained AHardwareBuffer
is imported with `eglGetNativeClientBufferANDROID` and
`eglCreateImageKHR(EGL_NATIVE_BUFFER_ANDROID)`, then bound to
`GL_TEXTURE_EXTERNAL_OES`. Native-fence EGL sync waits the image acquire fd;
applications that accept MediaCodec frames through
`setVideoFrameScheduler()` can call the non-blocking `queueFrame()` after
reserving bounded render capacity. Image acquisition then completes
asynchronously and raises `RedrawRequested` for the retained exact frame. The
reader keeps acquisition slack outside the correlation window so a coalesced
listener callback cannot strand an already-queued producer image at
`MAX_IMAGES_ACQUIRED`.

After libplacebo submission, `OpenGLVideoRenderer` invokes its optional
`OpenGLPresentCallback` with the context current and only then asks the interop
to export a release sync fd and return the image with
`AImage_deleteAsync()`. Android's adapter uses this hook for
`eglSwapBuffers()`, ensuring the fence covers default-framebuffer presentation
instead of remaining deferred until a later swap.

The external sampler uses `GL_EXT_YUV_target`, so R/G/B contain normalized
Y/Cb/Cr rather than an Android-selected RGB conversion. A small crop-aware GPU
pass writes those raw components into an RGBA16F texture. It performs no color
matrix, transfer, gamut, tone mapping, or output encoding; libplacebo performs
all of those operations after applying any FFmpeg `AVDOVIMetadata`. A
configured `width` and `height` only set the AImageReader's initial size;
MediaCodec output supplies each acquired image's actual dimensions and crop.
This route is zero-CPU-copy, but the RGBA16F normalization texture means it is
not strict no-intermediate source zero-copy.

Call `flush()` before seek, loop, decoder/media replacement, explicit stop, or
an interop-policy switch. Close the renderer while its context can still be
made current so pending EGLImage and GL resources are released safely.
Same-context Android window loss/recreation does not replace the decoder
surface or map the native frame.

The raw hardware contract requires `GL_OES_EGL_image_external_essl3`,
`GL_EXT_YUV_target`, AHardwareBuffer EGLImage import, and Android native-fence
sync. An unsupported result remains rejected so the application can select
direct-Surface or software fallback without mapping the hardware frame. The
old SurfaceTexture HDR trust switches remain source-compatible fields but are
ignored. `statistics()` reports raw imports, AHardwareBuffer format,
acquire/release fences, fallback synchronization, redraws, pending depth,
timestamps, and the zero CPU-map/transfer/staging/upload counters.

Multiple `VideoRenderAPI` instances can be associated with one player by using
an application-owned opaque key:

```cpp
player
    .setVideoRenderAPI(mainRenderer, mainSurfaceKey)
    .setVideoRenderAPI(previewRenderer, previewSurfaceKey)
    .setRenderCallback([&](void* key) {
        schedule_on_render_thread([&, key] {
            const auto result = player.renderVideoDetailed(key);
            if (result.status
                == qtav::VideoRenderStatus::RendererBusy) {
                schedule_bounded_retry(
                    key,
                    result.retryAfterMilliseconds);
            } else if (result.status
                       == qtav::VideoRenderStatus::RendererDeferred) {
                // The backend will request redraw. Player retains the exact
                // deferred frame for this renderer key.
            } else if (result.status
                       == qtav::VideoRenderStatus::SurfaceLost) {
                recreate_native_surface(key);
            }
        });
    });
```

Passing an empty `std::shared_ptr` removes the renderer for that key. The
existing `setVideoRenderer()` callback remains available and is used when no
`VideoRenderAPI` is registered for the requested key.

State/status callbacks are normally invoked from the playback worker; a
confirmed packet underflow can also publish buffering status from the starving
audio/video decode worker. The packet-buffer callback follows the same rule.
The playback worker demuxes selected packets and decodes the comparatively
light subtitle packets; independent audio- and video-decode workers prevent codec
work or output backpressure on one A/V stream from starving the other. Decoded
audio/video/subtitle frame notifications and `setRenderCallback()` run on a
separate presentation worker, so a slow application redraw path cannot stall
demux, decode, or device audio submission. The video presentation queue is bounded;
when the application falls behind, it preserves the imminent queued frame and
discards an incoming farther-future frame instead of accumulating unbounded
latency or repeatedly replacing the next presentable frame.
`renderVideoDetailed()` runs on the caller's thread, so an OpenGL, Vulkan, or
D3D integration can keep ownership of its native context and surface. Its
immutable frame and renderer-binding snapshots are atomically published; the
hot render path does not take the Player control mutex. The backend-level
`VideoRenderAttemptResult` distinguishes `Presented`,
`DeferredUntilRedraw`, `RetryAfterBackoff`, `Discarded`, `SurfaceLost`, and
`FatalError`. `renderVideoDetailed()` maps those to `Rendered`,
`RendererDeferred`, `RendererBusy`, `FrameDiscarded`, `SurfaceLost`, and
`RendererError`, while retaining `NoFrame` and the reserved
`PlayerStateBusy`. It also carries the frame sequence, presentation generation,
optional retry delay, structured `VideoRenderRetryReason`, and diagnostic
detail. The retry reason distinguishes state, serialization, reservation-aware
or unreserved device-context contention, and in-flight capacity without using
optional statistics as a control channel. Player retains the exact frame per
renderer key for `RendererDeferred`; the application schedules the backend
redraw and does not copy or track that frame. `RendererBusy` keeps the existing
bounded timer/latest-frame policy so high-level outputs may supersede a busy
frame. After a retained deferred frame is consumed, Player requests another redraw
when a newer frame was already published. Terminal discarded/no-frame results
wait for a newly published frame. Player rechecks the generation after the
backend call, so a seek, stop, or media replacement that overlaps rendering
returns `FrameDiscarded` for that stale completion. The same generation change
calls the renderer's non-blocking `invalidatePendingFrames()` hook, canceling
producer associations that have not entered a GPU submission without waiting
for submitted work. Existing custom renderers inherit a no-op implementation,
but consumers must rebuild because the public C++ vtable changed. The compatibility
`renderVideo()` returns the rendered timestamp in seconds and collapses every
other result to a negative value. Existing boolean-only `VideoRenderAPI`
implementations remain source-compatible: their `false` result maps to a
one-millisecond `RetryAfterBackoff` attempt until they override
`renderDetailed()`.
A/V startup and playing seeks use a bounded video preroll before releasing
device audio, avoiding an audio-first clock sprint while the first video
frames are still being decoded.

`setVideoFrameScheduler()` is the earlier, opt-in hardware-presentation hook.
It runs on the video-decode worker inside the bounded video decode window and
receives a steady-clock target in nanoseconds. Returning `true` makes the
application responsible for that exact frame and suppresses normal video
delivery; returning `false` keeps the presentation-worker path. Use
`playbackStatistics()` to distinguish decoded/delivered frames, bounded queue
overflow drops, late drops, the video-queue high-water mark, and presentation
queue starvation count/maximum duration.

`OpenGLPresentCallback` runs synchronously inside
`OpenGLVideoRenderer::render()` on the graphics-owner thread, after libplacebo
submits the framebuffer and before
`OpenGLHardwareFrameInterop::releaseFrame()`. It must perform only the
platform's bounded presentation operation and must not transfer ownership of
the current context to another thread.

An accepted seek while playing changes the media status to `Buffering` and
holds `position()` at the requested position until output really resumes. A
clock-capable audio sink must publish a valid post-flush device-clock sample;
callback-only playback resumes when the first item from the new presentation
generation is delivered. Audio-device underruns return to `Buffering` and
freeze the fallback clock until the device clock re-anchors. Queue invalidation
in the public `seek()` call does not wait for the presentation or audio workers
to release their queue locks. Accurate seek completion interrupts any
end-of-stream drain already running on the playback worker, delivers the
`SeekCallback` there, and then resumes draining or playback. Paused accurate
seek and frame stepping temporarily admit only the target video decode path;
audio stays paused and the selected frame is still published by the normal
video scheduler/presentation worker. Accepted seek, prepare, track-switch, or
state requests win over concurrent natural-end teardown, so a request cannot
report acceptance and then lose its callback to end-of-media cleanup. HTTP(S)
inputs additionally use a 15-second read timeout and bounded FFmpeg reconnect
defaults, all overridable through
`avformat.*`. If the protocol still returns a recoverable error,
`NetworkRecoveryPolicy` performs bounded fresh opens on the playback worker.
Its timer wait is interruptible, and a successful read recovery invalidates the
old generation before packet refill and output re-anchoring.

### CPU image-buffer renderer

Link `QtAV::RenderCPU`, include `<qtav/cpu_video_renderer.h>`, and provide a
writable buffer owned by the application:

```cpp
std::vector<std::uint8_t> pixels(width * height * 4);
auto renderer = std::make_shared<qtav::CpuVideoRenderer>();

renderer->setTarget({
    pixels.data(),
    width,
    height,
    width * 4,
    qtav::PixelFormat::BGRA,
});

qtav::VideoRenderConfig config;
config.surfaceSize = { width, height };
config.aspectRatio = qtav::VideoAspectRatioMode::Stretch;
renderer->open(config);
player.setVideoRenderAPI(renderer);
```

The buffer remains application-owned and must stay valid until rendering has
finished or another target is set. The initial reference backend accepts a
positive stride, a full-surface borrowed target, `Stretch`, and `Rotate0`.
Packed RGB24, BGR24, RGBA, BGRA, ARGB, and Gray8 are supported as destination
formats. `setTarget()` and `render()` are synchronized internally, but the
application remains responsible for coordinating its own reads of the pixel
memory with rendering.

### Vulkan renderer and Android surface adapter

Link `QtAV::RenderVulkan` and construct the engine with an
application-selected Vulkan instance, physical device, logical device,
graphics queue, and queue-family index. Vulkan 1.2 timeline semaphores and
host query reset must be enabled when the borrowed logical device is created
and declared in `BorrowedVulkanDevice`. The application supplies the current
image/view, extent, format, `VkColorSpaceKHR`, wait/signal semaphores, final
layout, and surface generation through `VulkanCurrentTargetCallback`. These
Vulkan objects remain borrowed and must survive the submission fence.
Supported targets are
RGBA8/BGRA8 with sRGB/BT.709 nonlinear output, 10-bit packed RGB with
HDR10/PQ or HDR10/HLG output, and RGBA16F with extended-sRGB-linear or
BT.2020-linear output. PQ/HLG sources preserve BT.2020 and native HDR
luminance on HDR targets; SDR targets retain the documented BT.709/sRGB tone
mapping. Linear HDR targets use `1.0` as the renderer's 100-nit reference
white and preserve brighter values above `1.0`.

Software frames are mapped with libplacebo's FFmpeg bridge. When FFmpeg has
parsed a Dolby Vision RPU into `AV_FRAME_DATA_DOVI_METADATA`, libplacebo maps
only residual-disabled base-layer metadata, applies base-layer reshaping, and
tone maps to the selected target. The repository FFmpeg overlay makes the HEVC
MediaCodec wrapper run FFmpeg's own RPU parser before submitting each access
unit, correlates the result with the reordered hardware output timestamp, and
attaches the same frame side data used by the software decoder. The OHCodec
wrapper uses the corresponding exact microsecond-PTS queue for OHOS hardware
outputs. MediaCodec and OHCodec frames then enter the same libplacebo DOVI path
after zero-CPU-copy external-image normalization.
This does not require libdovi: the optional raw-RPU parser remains disabled.
RPUs that require a residual enhancement layer are rejected by this
base-layer-only path; compressed passthrough, Dolby certification, and
licensing are outside QtAVCore's scope.

The renderer can also accept an optional `VulkanHardwareFrameInterop`.
`prepareFrame()` starts or polls asynchronous producer release before a
render target is acquired, while `importFrame()` returns a retained sampled
image/view, immutable YCbCr sampler, acquire/release semaphores, native image
layouts, foreign queue-family identity, and normalized source crop. The
renderer retains a directly sampled `VulkanTextureFrame` until its submission
fence completes. A normalized external source may override
`samplerLifetime()` with ownership independent of the decoded allocation; the
renderer can then return that allocation as soon as the normalization copy
finishes while retaining the immutable sampler/conversion token. Platform
interop remains a separate target; `QtAV::RenderVulkan` does not depend on a
platform decoder.

On Android, `QtAV::RenderVulkanAndroid` implements that target protocol and
the `VideoRenderAPI` facade together. The application creates a Vulkan
instance/device/graphics-present queue, then publishes the current
`ANativeWindow`:

```cpp
auto renderer =
    std::make_shared<qtav::AndroidVulkanVideoRenderer>(
        qtav::BorrowedAndroidVulkanContext {
            instance,
            {
                instance,
                physicalDevice,
                device,
                queue,
                queueFamilyIndex,
                true, // timelineSemaphoreEnabled
                true, // hostQueryResetEnabled
            },
            hdrMetadataExtensionWasEnabled,
        },
        qtav::VulkanOutputPreference::PreferHdr);
renderer->setWindow(nativeWindow);

qtav::VideoRenderConfig config;
config.surfaceSize = renderer->surfaceSize();
renderer->open(config);
player.setVideoRenderAPI(renderer);
```

The adapter acquires its own window reference and owns the associated
`VkSurfaceKHR`, swapchain, image views, and per-frame acquire/present
semaphores. Passing `nullptr` to `setWindow()` invalidates that generation.
Publishing a new window while the renderer remains open rebuilds the
surface/swapchain and resumes presentation without reopening media. Publishing
the same window again after its buffer geometry changes refreshes the
swapchain extent and Fit/Fill viewport in place. The Vulkan
instance, device, and queue remain application-owned and must outlive the
renderer. `PreferHdr` chooses HDR10/PQ first, then native HLG or
extended-linear output, and falls back to SDR; `RequireHdr` fails explicitly
when no implemented HDR pair is exposed, while `SdrOnly` preserves an
application-selected SDR policy. The application must enable
`VK_EXT_swapchain_colorspace` while creating its instance to expose extended
surface color spaces. If it also enables `VK_EXT_hdr_metadata` on the logical
device and reports that fact in `BorrowedAndroidVulkanContext`, the adapter
submits frame-derived mastering-display and content-light metadata before
presentation. `surfaceFormat()` and `hdrOutputActive()` expose the selected
contract for diagnostics and tests.

### OpenGL ES renderer and native EGL adapters

Link `QtAV::RenderOpenGLAndroid` for the Android OpenGL ES fallback. The
adapter owns its EGL display, OpenGL ES 3.x context, window surface, and swap
operations while retaining only the active `ANativeWindow` generation:

```cpp
#include <qtav/android_opengl_video_renderer.h>

auto renderer =
    std::make_shared<qtav::AndroidOpenGLVideoRenderer>(
        qtav::OpenGLOutputPreference::PreferHdr);
renderer->setWindow(nativeWindow);

qtav::VideoRenderConfig config;
config.surfaceSize = renderer->surfaceSize();
config.aspectRatio = qtav::VideoAspectRatioMode::Fit;
renderer->open(config);
player.setVideoRenderAPI(renderer);

if (renderer->hdrOutputActive()) {
    recordHdrSurface(
        renderer->outputColorSpace(),
        renderer->colorComponentBits());
}
```

`PreferHdr` tries an exact RGB10_A2 BT.2020/PQ EGLConfig and colorspace first,
then BT.2020/HLG when the display exposes it, and finally RGBA8/sRGB.
`RequireHdr` rejects an SDR-only display, while `SdrOnly` always selects the
RGBA8 path and preserves deterministic HDR-to-SDR tone mapping.
`outputColorSpace()`, `hdrOutputActive()`, and `colorComponentBits()` expose
the selected contract. The adapter verifies the EGL surface color space and
the `ANativeWindow` dataspace used by Android composition.

Passing `nullptr` to `setWindow()` invalidates the active EGL surface; a later
window recreates it and reconfigures the still-open renderer generation.
Publishing the same window after an in-place buffer-geometry change recreates
the EGL surface and refreshes the renderer target size.
`render()` makes the owned context current for the call, presents with
`eglSwapBuffers()`, and then releases it from the calling thread. The adapter
does not create or select Vulkan and is not itself the Vulkan-to-OpenGL ES
policy layer.

On OHOS, link `QtAV::RenderOpenGLOHOS` and pass the current XComponent window
to `OHOSOpenGLVideoRenderer::setWindow()`. The adapter takes an OHOS native-
object reference and owns EGL surface/context recreation for that window
generation. `PreferHdr` first tries exact RGB10_A2 BT.2020/PQ and BT.2020/HLG
candidates, verifies the selected `OHNativeWindow` color space, and then falls
back to verified RGBA8/sRGB. `RequireHdr` rejects that SDR fallback. On the
2026-08-11 connected `XComponentType.SURFACE`, OpenGL ES passed required-HDR
with exact RGB10_A2 BT.2020/PQ. The matching Vulkan adapter passed with
A2B10G10R10 BT.2020/PQ even though the OHOS surface-format query omitted the
HDR pair: when the application declares that `VK_EXT_swapchain_colorspace` was
enabled, the adapter performs a bounded OHOS-only creation attempt, verifies
the NativeWindow color space, and retains a normal SDR retry for `PreferHdr`.
Both adapters publish video-source, HDR-white-point, and per-frame HDR metadata
to the NativeWindow. The player also aligns ArkUI's `hdrBrightness()` hint with
the selected HDR/SDR policy, and RenderService reported an active HDR
composition algorithm during both renderer runs.

`QtAV::RenderOpenGL` is the reusable engine for applications or future
platform adapters that already own an OpenGL ES 3.x context. Its
`OpenGLCurrentTargetCallback` returns the current framebuffer number, size,
generation, and `OpenGLOutputColorSpace`; framebuffer zero is the default
framebuffer. The context must be current for `open()`, `render()`, and
`close()`. The engine accepts
YUV420/422/444, NV12/NV21, little-endian P010, RGB/BGR,
RGBA/BGRA/ARGB, and Gray8 software frames. It maps their FFmpeg storage and
structured range/matrix/transfer/primaries metadata into libplacebo, which
owns color conversion, scaling, Dolby Vision reshaping, tone mapping, gamut
mapping, and final output encoding. The renderer supplies Fit/Fill/Stretch,
custom viewports, and right-angle rotations through libplacebo frame geometry.
If the OpenGL ES GPU cannot directly upload a high-bit-depth software plane,
the renderer uses libswscale to reduce it to a directly uploadable 8-bit
representation. Y/Cb/Cr sources remain planar Y/Cb/Cr with their Dolby Vision
metadata intact; RGB sources become full-range RGBA and discard any
representation-specific Dolby Vision side data. This prevents converted RGB
components from being interpreted a second time as Dolby Vision Y/Cb/Cr. This
is a CPU software-decode compatibility fallback and may lose source precision;
hardware-frame interop remains on its separate zero-CPU-map path.
For `SdrSrgb`, it queries the bound framebuffer attachment's color encoding so
fixed-function sRGB conversion occurs exactly once. BT.2020 `HDR10PQ` and
`HDR10HLG` targets preserve HDR through libplacebo's corresponding output
color contract. Android hardware interop supplies an RGBA16F raw-YCbCr
normalization texture to this same pipeline; it does not run a competing color
shader.

### Mobile renderer selector

Link `QtAV::RenderMobile` and provide factories that create a fully prepared
platform adapter for the current native-window generation. The Android
application continues to own its activity, window reference, and borrowed
Vulkan device:

```cpp
#include <qtav/mobile_video_renderer.h>

qtav::MobileRendererSelectorConfig selectorConfig;
selectorConfig.preferredAPI = settings.preferOpenGLES
    ? qtav::MobileRenderAPI::OpenGLES
    : qtav::MobileRenderAPI::Vulkan;
selectorConfig.vulkan = [&] {
    return createPreparedAndroidVulkanRenderer(currentWindow);
};
selectorConfig.openGLES = [&] {
    return createPreparedAndroidOpenGLRenderer(currentWindow);
};

auto selector =
    std::make_shared<qtav::MobileVideoRendererSelector>(
        std::move(selectorConfig));
selector->setSelectionCallback([](const auto& event) {
    recordRendererSelection(
        qtav::mobileRenderAPIName(event.selectedAPI),
        event.detail);
});
selector->setHardwareFrameFallbackCallback(
    [&](const qtav::MobileHardwareFrameFallbackEvent& event) {
        if (event.previousAPI == qtav::MobileRenderAPI::Vulkan
            && event.sourceDevice
                == qtav::HardwareDeviceType::MediaCodec
            && preparedOpenGLInterop) {
            player.setHardwareDecodeConfig(
                qtav::mediaCodecHardwareDecodeConfig(
                    preparedOpenGLInterop->surface()));
            return qtav::MobileHardwareFrameFallbackDecision {
                qtav::MobileHardwareFrameFallbackRoute::
                    OpenGLESInterop,
                "MediaCodec rebound to the OpenGL AImageReader producer",
            };
        }
        player.setHardwareDecodeConfig({});
        return qtav::MobileHardwareFrameFallbackDecision {
            qtav::MobileHardwareFrameFallbackRoute::SoftwareDecode,
            "Hardware interop failed; continuing with software decode",
        };
    });

selector->open(config);
player.setVideoRenderAPI(selector);
```

Each successful factory result contains an already window-bound
`VideoRenderAPI`; an unavailable result carries its diagnostic reason. The
selector tries `preferredAPI` on every new `open()` session. A candidate
`SurfaceLost` event triggers at most `maximumRecoveryAttempts` complete
same-API recreations. A candidate `Error` is fatal: Vulkan switches one-way to
OpenGL ES. When the failing OpenGL ES input is a hardware frame, the hardware
fallback callback runs again and may select `SoftwareDecode`; non-hardware
fatal OpenGL ES failures report video unavailable. The
current frame is retained and retried after successful recovery or fallback,
and the selector object remains attached to `Player`, so media is not reopened.
The exception is a cross-API hardware frame: it belongs to the retired native
surface and is discarded rather than retried. The hardware fallback callback
runs synchronously inside `renderVideo()` and must choose
`OpenGLESInterop`, `DirectSurface`, `SoftwareDecode`, or `NoVideo`; returning
`None` or omitting the callback is an explicit unavailable result. It may
publish a new `HardwareDecodeConfig`, but must not destroy the selector.
OpenGL ES interop also requires the prepared candidate to advertise the
source hardware device. Software decode keeps the OpenGL ES renderer for later
software frames, while direct-surface and no-video routes retire it.

A renderer may return `false` without emitting `SurfaceLost` or `Error` while
asynchronous native interop is pending. The selector treats that as retryable
and does not change graphics APIs. Late frames from the retired native surface
and hardware frames arriving while a software-decode transition completes are
discarded without calling `HardwareFrame::map()`.

Call `suspendSurface()` before releasing a platform window, publish the new
window to the factory's application state, and call `recreateSurface()`.
Selection notifications distinguish initial selection, recovery, fallback,
and no-renderer results. `selectedAPI()`, `usingFallback()`,
`presentationAvailable()`, `hardwareFrameFallbackRoute()`, and `lastError()`
provide synchronous diagnostics.

### D3D11 composition output

For ordinary Windows presentation, link `QtAV::OutputD3D11` and provide the
native composition control's swap-chain binding operation. The output owns
 the D3D11 device, HDR-aware flip-model swap chain, target, display tracking,
 render scheduling thread, reason-aware rendering, `Present()`, D3D11VA
configuration, raw-plane libplacebo interop, resize, and teardown:

```cpp
#include <qtav/d3d11_video_output.h>
#include <qtav/player.h>

qtav::Player player;
qtav::D3D11VideoOutput output;
qtav::D3D11CompositionSurface surface;
surface.size = { width, height };
surface.compositionScaleX = compositionScaleX;
surface.compositionScaleY = compositionScaleY;
surface.window = hwnd;
surface.bindSwapChain = [panelNative](IDXGISwapChain1* swapChain) {
    return panelNative->SetSwapChain(swapChain);
};

if (!output.open(std::move(surface)) || !output.attach(player)) {
    throw std::runtime_error(output.lastError());
}

player.setMedia(url).setState(qtav::State::Playing);

// On native surface size/scale changes:
output.resize({ newWidth, newHeight }, newScaleX, newScaleY);

// Before destroying either the Player or native surface:
output.detach();
output.close();
```

The default `D3D11OutputPreference::PreferHdr` creates an FP16 scRGB
composition swap chain when `surface.window` or `surface.currentMonitor` can
identify the active display. Before every presentation, the renderer resolves
that monitor to `IDXGIOutput6`, observes the current Windows HDR setting,
system SDR reference white, and panel luminance, and configures
`DXGI_COLOR_SPACE_RGB_FULL_G10_NONE_P709`. PQ/HLG material therefore remains
HDR through the final DWM layer while Advanced Color is active; on an SDR
display or with Windows HDR disabled, the same path tone maps into the SDR
range. Moving the window between displays takes effect on the next frame.

Opaque video surfaces may set
`D3D11VideoOutputOptions::hdrPresentationMode` to
`D3D11HdrPresentationMode::HDR10` together with
`DXGI_ALPHA_MODE_IGNORE`. This selects an RGB10 swap chain and
`DXGI_COLOR_SPACE_RGB_FULL_G2084_NONE_P2020` while Advanced Color is active,
avoiding the DWM scRGB conversion for video-player or fullscreen presentation.
Selecting RGB10/PQ with a blending alpha mode is rejected; the general-purpose
FP16 scRGB default remains appropriate for transparent composition surfaces.

`D3D11OutputPreference::RequireHdr` refuses to present while the current
display is not in Advanced Color HDR mode.
`D3D11OutputPreference::SdrOnly` creates a BGRA8 SDR swap chain. `PreferHdr`
also falls back to BGRA8 when no HWND or monitor provider is supplied, keeping
non-windowed composition hosts usable without claiming HDR. `colorInfo()`
reports the selected format/color space, active display, system SDR white,
luminance, and whether the presented layer is currently HDR.

`attach()` exclusively owns the player's default render slot and render
callback until `detach()`. The player and native surface must therefore
outlive the attachment. Output event and frame-presented callbacks may run on
the output render thread and must not destroy or detach the output from inside
the callback. Toolkit-specific UI changes should be posted to that toolkit's
dispatcher.

The current WinUI 3 example uses only its HWND, this surface binding, and
`attach()`/`resize()`; it no longer implements a graphics device, swap chain,
render thread, HDR policy, or presentation loop in application code.

The composition output caps DXGI frame latency at one, uses non-blocking
`Present()` and a bounded waitable-object wait on its private render thread,
and retries when the compositor is busy. It never waits for presentation
capacity on the UI thread. Retryable Player/backend contention enters a
latest-frame mailbox rather than becoming an immediate drop. A newer frame
supersedes an older pending frame. Before each output pass makes its first
non-blocking D3D11 context attempt, it creates a render-thread reservation. An
uncontended pass proceeds immediately; a contended pass waits for at most 8 ms
for a decode-side owner to yield. Only a failed handoff enters the one-frame
retry mailbox and bounded 1/2/4/8/16 ms backoff. There is no busy spin and the
WinUI thread does not wait for this exchange.

`D3D11StatisticsMode` controls collection for both the low-level renderer and
high-level output. `Off` removes continuous counter and clock work, `Counters`
is the low-cost default, and `Timing` additionally enables per-frame cadence,
Present, gap, and coarse color/interop/buffer/draw clocks. Runtime changes use
`D3D11VideoOutput::setStatisticsMode()` or
`D3D11VideoRenderer::setStatisticsMode()` and never change retry or lifetime
behavior. The high-level output consumes the structured render retry reason and
reads renderer statistics only when the application calls `takeStatistics()`;
it no longer clears and re-aggregates renderer atomics after every frame.

`takeStatistics()` returns and resets render requests/passes, presented frames,
coalesced requests, busy presents, reason-level no-frame/Player/renderer-busy
attempts, retry wakeups, superseded and terminal frames, renderer lock-stage
contention, reservation-aware versus unreserved context ownership, handoff
waits/timeouts, the retained decoder-surface-copy diagnostic counter, long
gaps, render/present maxima, and the renderer's color, interop, buffer-update,
and draw-stage maxima. Investigation-only completion-query, clear,
`pl_render_image()`, retained-resource, and asynchronous libplacebo pass/GPU/
callback probes are not part of the stable statistics surface.
`skippedRenders` is retained for compatibility and now mirrors
`terminalRenderDrops`; a recovered retry is not a skipped frame. The copy
counter remains zero under explicit direct decoder-texture sampling and
increments for the default visible-region GPU-copy policy.

### D3D11 renderer (advanced external-context path)

On Windows, link `QtAV::RenderD3D11`, include
`<qtav/d3d11_video_renderer.h>`, create shared access to the selected device
and its immediate context, and pass a callback that returns the current
application-owned render target:

```cpp
auto deviceAccess = qtav::D3D11DeviceAccess::create(
    qtav::BorrowedD3D11Device(device),
    qtav::BorrowedD3D11DeviceContext(immediateContext));

auto renderer = std::make_shared<qtav::D3D11VideoRenderer>(
    deviceAccess,
    [&] {
        return qtav::D3D11RenderTarget {
            currentRenderTargetView,
            currentSwapChain3,
        };
    });

qtav::VideoRenderConfig config;
config.surfaceSize = { width, height };
renderer->open(config);
player.setVideoRenderAPI(renderer);
```

`D3D11DeviceAccess::create()` rejects null, foreign-device, and deferred
contexts, enables native D3D11 multithread protection on the verified
immediate context, retains the selected device and context, and provides the
recursive lock shared by the renderer and D3D11VA/interop backends. Creation
also fails if the context cannot expose or enable native multithread
protection. `contextGuard()` waits for ownership; `tryContextGuard()` is its
non-blocking real-time form and returns a false guard when the context is
busy. A false guard reports whether its current owner uses the reservation-aware
FFmpeg/internal acquisition path. `reserveContext()` gives its creating thread
priority over new reservation-aware acquisitions without taking the context;
`tryContextGuardFor()` then performs the bounded acquisition. Ordinary public
`contextGuard()` calls remain independent of reservations so application
surface operations do not create a graphics-lock/context-lock cycle. The
native guard covers context calls made inside FFmpeg and libplacebo before
driver dispatch; it does not replace the explicit guard required around
application context calls. The older renderer constructor taking the two
borrowed wrappers remains a convenience path and creates the same retained
access internally.
The callback and returned `ID3D11RenderTargetView`/`IDXGISwapChain3` remain
application-owned. The swap chain is optional for offscreen rendering, but it
is required for native Advanced Color presentation. Composition swap chains
also return the hosting window's current `HMONITOR` in
`D3D11RenderTarget::monitor` because `GetContainingOutput()` is unsupported
for that swap-chain kind. The callback runs synchronously inside `render()`
and is queried for every frame, so swap-chain resize, display moves, or other
surface recreation can replace the current values before calling
`configure()` with the new size.

Submitted frames retain their decoder slice, imported texture wrappers, and
borrowed target texture until a D3D11 completion event reports that the GPU is
done. At most three submissions remain in flight. Call `flush()` before
resizing, destroying, or replacing a target; it completes queued GPU work and
releases those retained references. `D3D11VideoOutput` performs this drain
automatically before `ResizeBuffers()`.

The renderer uses `tryContextGuard()` and a non-blocking render lock while
issuing immediate-context calls. If either is busy, `renderDetailed()` returns
timer backoff plus the exact `VideoRenderRetryReason` without emitting a backend
error. `Player::renderVideoDetailed()` preserves that reason with
`RendererBusy`, allowing a custom scheduler to retry it without consulting
statistics or confusing it with `NoFrame`; the compatibility `render()` and
`renderVideo()` wrappers retain their boolean/negative behavior. Applications
using that immediate context from another thread must
acquire `contextGuard()` (or provide equivalent external serialization):

```cpp
{
    auto guard = deviceAccess->contextGuard();
    immediateContext->CopyResource(destination, source);
}
```

The renderer does not preserve D3D11 pipeline state; applications sharing that
context must restore their state after `renderVideo()`.

The software path uploads YUV420P, YUV422P, YUV444P, NV12, NV21, P010,
RGB24, BGR24, RGBA, BGRA, ARGB, and Gray8 frames. It renders to single-sample
BGRA8/RGBA8 UNORM, `R16G16B16A16_FLOAT`, or
`R10G10B10A2_UNORM` 2D targets. libplacebo generates the D3D11 shaders and is
the sole authority for limited/full-range YUV conversion, transfer functions,
primaries conversion, Dolby Vision reshaping, display-aware tone/gamut mapping,
scaling, and output encoding. The renderer supplies custom viewports,
Fit/Fill/Stretch, all right-angle rotations, resize, surface recreation, and
surface/device-loss events; it does not maintain a competing semantic shader.

For general-purpose Windows presentation, create a flip-model
`R16G16B16A16_FLOAT` swap chain. The renderer sets its color space to
`DXGI_COLOR_SPACE_RGB_FULL_G10_NONE_P709`; scRGB `1.0` represents 80 nits and
HDR highlights may exceed `1.0`. An `R10G10B10A2_UNORM` flip-model swap chain
is also accepted: while Windows HDR is active on the current display, the
renderer selects `DXGI_COLOR_SPACE_RGB_FULL_G2084_NONE_P2020` and writes
BT.2020/PQ HDR10 values; while HDR is inactive, it switches the same target to
SDR G22/P709 instead. Eight-bit targets remain SDR.

When a swap chain is supplied, every `render()` obtains its containing output
and `IDXGIOutput6::GetDesc1()` data. This makes Windows HDR setting changes and
moving the window between displays take effect on the next frame. The renderer
also reads `DISPLAYCONFIG_SDR_WHITE_LEVEL`; SDR material composed into an HDR
surface is scaled to the user's current Windows SDR reference-white setting.
HDR material is mapped against the current display maximum luminance. On an
SDR display, FP16 output is mapped into `[0, 1]` before DWM composition rather
than relying on numeric clipping. `advancedColorInfo()` exposes the most
recent monitor, active display/swap-chain color spaces, bit depth, SDR white,
whether that white value came from the system query, and luminance limits for
diagnostics. Explicit swap-chain HDR metadata is not set; the renderer maps
pixels into the reported display range.

`QtAV::RenderD3D11` also defines the decoder-independent
`D3D11HardwareFrameInterop` and `D3D11TextureFrame` contracts implemented by
`QtAV::InteropD3D11`. The interop binds to the same retained
`D3D11DeviceAccess`, performs no CPU map during `importFrame()`, and returns
borrowed texture/SRV pointers whose COM resources remain valid while the
returned texture-frame object is alive. Bind it with
`setHardwareFrameInterop()`; the renderer advertises D3D11 hardware-frame
support only when both objects use the same `D3D11DeviceAccess`. Imported
textures report their DXGI format and color space so the final
viewport/aspect/rotation/color pass can preserve or convert SDR, linear scRGB,
and RGB10/PQ data correctly.

The active source resources are retained through GPU completion of the
libplacebo draw. Decode, interop, and rendering use the same
native-multithread-protected immediate context, with an additional shared
recursive guard around QtAVCore-owned context sequences. The renderer keeps a
bounded three-frame completion-query queue and a matching ring of reusable
NV12/P010 copy textures. Normal flush, resize, media replacement, and teardown
paths explicitly drain both. By default every imported D3D11VA frame is copied
with its even-aligned visible source box into a same-format, single-slice
shader-readable texture. D3D11.1 uses `CopySubresourceRegion1()` with
`D3D11_COPY_DISCARD`; older contexts use the same visible box with
`CopySubresourceRegion()`. The decoder slice and copied texture remain retained
until completion. Renderer-owned wrappers are destroyed on the render thread,
then the source frame and interop wrapper move to a fixed-capacity recycler for
final FFmpeg/D3D11VA release. This keeps vendor allocation teardown off the
latency-sensitive output/render thread without adding a per-frame GPU wait.

Every successfully imported D3D11VA frame uses libplacebo's fast sampling
policy without the optional GPU histogram peak-detection pass. Successful
per-frame submission remains asynchronous and does not call `pl_gpu_finish()`.
Software frames retain the default render parameters. The previous direct-
sampling policy remains available as an explicit option, not an automatic
vendor rule. It avoids the GPU copy but keeps the scarce decoder slice alive
through draw completion and depends on shader-readable decoder allocations.
Fresh NVIDIA, AMD, and Intel seek, shutdown, cadence, and stage-timing
regression is required for changes to this policy.

Hardware-frame import and decoder fallback are independent policies. The
renderer does not map a hardware frame by default. Applications may explicitly
enable `setAllowSoftwareMappingFallback(true)` to call
`HardwareFrame::map(Read)` and use the existing software upload path when
interop is unavailable or import fails. A successful mapped fallback emits an
error/detail event containing `software-mapping fallback` so the CPU transfer
is observable; a disabled or failed mapping makes `render()` fail.

### D3D11VA hardware decode

Link `QtAV::HWD3D11VA` and pass the same retained device access used by the
renderer before opening media:

```cpp
#include <qtav/d3d11va_hardware_decoder.h>

auto deviceAccess = qtav::D3D11DeviceAccess::create(
    qtav::BorrowedD3D11Device(device),
    qtav::BorrowedD3D11DeviceContext(immediateContext));

player
    .setHardwareDecodeConfig(
        qtav::d3d11vaHardwareDecodeConfig(deviceAccess))
    .onVideoFrame([](const qtav::VideoFrame& frame, int) {
        const auto native =
            qtav::d3d11vaFrame(frame.hardwareFrame());
        if (!native) {
            return; // Explicit software fallback may be active.
        }
        ID3D11Texture2D* texture = native.texture();
        const UINT slice = native.arraySlice();
        // Both remain valid while native or its source frame is alive.
    });
```

The helper creates and initializes FFmpeg's D3D11VA device context on the
selected device, retains its verified immediate context, and installs lock
callbacks backed by the same recursive guard used by the renderer. Its default
pool allowance is four extra hardware frames and is bounded to 64. If selected
device initialization, codec capability, pixel-format negotiation, or decoder
open fails, the default policy reports `decoder.hardware.fallback` and
continues in software; disabling fallback makes the failure terminal.

The helper also enables the compatible decoder cache supplied by this
repository's Windows FFmpeg package. Player retains an initialized D3D11
hardware-frames context across repeated format selection when its device,
formats, dimensions, and pool capacity still match. FFmpeg then reuses the
matching `ID3D11VideoDecoder` and output views only after checking the texture,
array size, decoder descriptor, and configuration. This keeps repeated HEVC
SPS format callbacks from tearing down an unchanged decoder through the shared
Intel D3D11 device. A Windows QtAVCore build must therefore use the paired
repository FFmpeg package rather than a stock binary.

Decoded D3D11 frames expose NV12 or P010 `ID3D11Texture2D` array slices through
`D3D11VAFrame`. A copied view retains the underlying FFmpeg frame, pool,
hardware device, COM resources, and lock state. `HardwareFrame::map()` remains
the explicit CPU-copy path and can be used after seek, media replacement,
stop, or player shutdown while the copied frame is alive. Feeding decoder
slices to the renderer without a CPU copy uses the separate
`QtAV::InteropD3D11` target:

```cpp
#include <qtav/d3d11_frame_interop.h>

auto interop = std::make_shared<qtav::D3D11FrameInterop>(
    deviceAccess);
renderer->setHardwareFrameInterop(interop);
player
    .setHardwareDecodeConfig(
        qtav::d3d11vaHardwareDecodeConfig(deviceAccess))
    .setVideoRenderAPI(renderer);
```

`D3D11FrameInterop` validates the decoder texture, array slice, source format,
device health, allocation dimensions, and exact COM device identity. Each
import retains the original D3D11VA NV12/P010 texture array slice without
creating an RGB intermediate or submitting a Video Processor conversion. The
default renderer copies only the decoded visible rectangle, rounded to the
NV12/P010 chroma block, into its bounded same-format ring and wraps those plane
views with libplacebo. Structured frame color and Dolby Vision metadata still
enter the same raw-plane color/render pipeline. The final renderer performs
display-specific tone and gamut mapping without a CPU map.

Direct decoder-texture sampling is intentionally opt-in. Advanced integrations
must set both `D3D11VAHardwareDecodeOptions::directDecoderTextureSampling` and
`D3D11VideoRenderer::setDirectDecoderTextureSamplingEnabled(true)` before
decoder creation; `D3D11VideoOutputOptions::directDecoderTextureSampling`
configures both for the high-level output. Direct mode requires
`D3D11_BIND_SHADER_RESOURCE` on the decoder array, reports zero
`decoderSurfaceCopies`, and retains the decoder frame and transient plane
wrappers through completion. The default reports one decoder-surface copy for
each successfully submitted raw D3D11VA frame.

The WinUI Debug field `decoder-copies` displays
`D3D11VideoOutputStatistics::decoderSurfaceCopies`; it is a reset-on-read
same-GPU copy counter, not a CPU-copy counter or an independent switch. The
actual high-level mode switch is
`D3D11VideoOutputOptions::directDecoderTextureSampling`. Set it to `false` for
the off/default visible-region GPU-copy policy:

```cpp
qtav::D3D11VideoOutputOptions options;
options.directDecoderTextureSampling = false; // default GPU-copy mode
output.open(std::move(surface), options);
output.attach(player);
```

Set the same switch to `true` to turn on explicit direct decoder-texture
sampling:

```cpp
qtav::D3D11VideoOutputOptions options;
options.directDecoderTextureSampling = true; // direct-sampling mode
output.open(std::move(surface), options);
output.attach(player);
```

Use the default for normal cross-vendor playback. Treat direct mode as an
application opt-in after adapter/driver qualification, or as an A/B diagnostic
when isolating copy cost from decoder-surface lifetime behavior. A zero count is
valid evidence only while D3D11VA input and rendered-frame progress are also
confirmed. See [D3D11VA](D3D11VA.md#directdecodertexturesampling-modes-and-copy-diagnostics) for
the low-level two-ended configuration and validation caveats.

`VideoFrame::colorSpaceInfo()` returns structured range, primaries, transfer,
matrix, and chroma location. `masteringDisplayMetadata()` and
`contentLightMetadata()` return copied HDR10 static metadata, so their values
remain valid with any copied frame after decoder progress or player shutdown.
`hasDolbyVisionMetadata()` reports whether FFmpeg attached parsed RPU metadata
without exposing FFmpeg structures. The existing `colorSpace()` string remains
available for diagnostics.

## Backend contracts

`VideoRenderAPI` defines surface size, viewport, supported aspect modes,
rotation, native resource ownership, capability reporting, redraw/surface
events, and explicit open/configure/render/close lifecycle. Generic headers
contain no graphics API types; typed native constructors and handle helpers
belong in a backend's public header.

`AudioSink` separates the decoded input format passed to `open()` from the
negotiated device format returned by it. It defines close, pause, flush, write,
drain, event, latency, and device-clock operations. `write()` consumes a
synchronous non-owning `AudioBufferView` in the negotiated device format.
`drain()` waits until accepted buffers have been presented and is called after
each completed playback segment, including a loop boundary, and before
`close()` at final natural end; its default implementation is a no-op for
synchronous or non-queuing sinks. The `audioBufferView()` helper creates a
view when no conversion is required. `Player` opens an injected
`AudioFrameConverter` when the negotiated format differs, resets it on
flush/seek, drains the converter and sink at each completed segment, and closes
them at final natural end. Without an injected converter, a different device
format reports
`audio.sink.format` while `onAudioFrame()` continues normally.

`QtAV::AudioResample` implements `SwresampleAudioConverter`. It converts sample
format, sample rate, and channel layout to interleaved U8, S16, S32, float, or
double PCM. Conversion result memory belongs to the converter until its next
operation; `Player` writes it to the sink synchronously from its audio-output
worker. Converter and sink calls are serialized without the player mutex held;
control-driven lifecycle calls run on the playback worker, while conversion
and ordinary writes run on the audio-output worker.

`AudioTimeStretcher` is an optional format-preserving streaming stage between
the converter and sink. `open()` fixes one PCM format and playback rate;
`process()` may buffer input and return zero or more processor-owned output
views; `drain()` is repeated until it returns no output; `reset()` discards a
discontinuity without renegotiating the format. Output sample counts describe
physical device PCM, while timestamps and durations remain media time.
`QtAV::AudioTimeStretch` implements this contract with FFmpeg's `atempo`
filter. The core links no mandatory DSP library and bypasses the stage exactly
at rate 1.0.

`AudioFrameProcessor` follows time stretch and precedes the sink. Its output
storage remains valid through the synchronous sink write, and it may buffer PCM
until `drain()`. Player verifies format, monotonic timestamps, bounded output,
and equal completed-segment sample counts. `QtAV::AudioFilter` implements a
constant-gain reference processor with an internal FFmpeg `volume` graph.

`VideoFrameProcessor` runs synchronously after an optional direct
`VideoFrameScheduler` declines the frame and before the presentation queue. It
has explicit format/per-frame bypass and preserves timestamp and duration in a
strict one-to-one result. Reset/drain/close are serialized with processing;
render-thread effects and delayed cadence transforms are separate contracts.

`QtAV::AudioFile` implements `WavAudioSink`. It negotiates interleaved U8, S16,
S32, float, or double PCM, writes a little-endian RIFF/WAVE stream, and
finalizes its size fields on close. Its configured zero sample-rate or channel
count inherits the decoded value. It has no device clock and does not pace
playback; applications normally inject `QtAV::AudioResample` when decoded PCM
is planar or otherwise differs from the requested file format. Seek flushes
the stream but preserves already captured samples, so subsequent playback is
appended to the same diagnostic timeline.

`QtAV::AudioWASAPI` implements `WasapiAudioSink` on Windows. Its
backend-specific public header uses an owning `WasapiEndpointId` value rather
than exposing `IMMDevice` through the generic contract. The sink follows the
default multimedia render endpoint unless an explicit endpoint is selected,
negotiates shared-mode interleaved Float32 PCM at the engine mix rate, copies
accepted buffers into a bounded backend queue, and implements event-driven
pause/flush/drain. COM and native WASAPI interfaces stay on a dedicated
multimedia-class thread; callers can query a cached media-timeline
`IAudioClock` position and combined engine/stream latency from any thread.

`QtAV::AudioAAudio` implements `AAudioAudioSink` on Android API 26 or newer.
Its backend-specific header represents an optional device selection as an
owning integer `AAudioDeviceId`, so no AAudio declaration enters the generic
contract or core headers. It negotiates interleaved Float32 mono/stereo PCM,
copies accepted buffers into a preallocated SPSC ring, and feeds the native
data callback without allocation or locks. AAudio presentation timestamps
supply the media-timeline clock; latency includes queued PCM and frames already
submitted to the native stream. Pause, flush, drain, xrun detection, transparent
route observation, and disconnect-triggered stream reconstruction remain
inside the Android target.

`QtAV::AudioOHAudio` implements `OHAudioAudioSink` on OHOS API 23 or newer.
Its backend header exposes only QtAVCore value types and diagnostics; OHAudio
declarations remain private to the implementation. It negotiates interleaved
Float32 mono/stereo PCM, shares the allocation-free SPSC queue implementation
with AAudio, anchors playback to hardware-committed native frame timestamps,
and reports live queued/native latency. Pause, flush, drain, interruption,
underrun, route-change rebuilding, and error recovery stay inside the OHOS
target.

`QtAV::PlatformWindows` owns the Windows-only `D3D11DeviceAccess` helper and
strong non-owning wrappers for `ID3D11Device` and `ID3D11DeviceContext`; no
Windows SDK type reaches the core headers. `QtAV::RenderD3D11` retains that
shared access, compiles shaders for its device, uploads software-frame planes
into per-frame shader resources, and obtains the current borrowed render
target immediately before drawing.

`QtAV::HWD3D11VA` also retains the shared device access while its FFmpeg
hardware-device token, decoder pools, or copied frames remain alive. The
backend-specific `D3D11VAFrame` validates the decoded texture format, array
slice, dimensions, and source device before exposing borrowed native pointers.
Its decoder and mapping calls use FFmpeg lock callbacks connected to the same
recursive context lock as rendering.

`AudioSinkClock` fields are measured in milliseconds on the sink's physical
PCM timeline, anchored to the media timestamp of the first accepted buffer
after open or flush. `latencyMilliseconds` is informational and must not
already be folded into `positionMilliseconds`. At rate 1.0 that clock is also
media time. With time stretching, Player multiplies only the physical delta
from the anchor by the active rate before using it as playback master. A sink
that advertises a device clock becomes the master whenever `clock()` returns a
valid value; otherwise the player falls back to its monotonic software clock.
Ordinary sink writes and their clock samples run on the dedicated audio-output
worker. When a playing seek or underrun recovery is waiting for the first valid
post-flush timestamp, that same worker polls the sink until it becomes valid;
presentation never calls the backend. A blocking backend write is therefore
never allowed to stall video scheduling. Repeated device samples are
monotonically extrapolated at the active rate, bounded by submitted media time,
and published as a generation-checked cache, so
`Player::position()` never waits for a sink write or calls into a platform
backend. Sink lifecycle and segment-end `drain()` run on the playback worker,
serialized with writes and without the player mutex held; `drain()` may block
until the backend queue is presented. Shutdown synchronizes the quitting
predicate with each worker condition-variable mutex before notification so
worker joins cannot lose the wake-up. On initial playback, a clock-capable sink
whose first sample is invalid falls back after the first delivered buffer.

`HardwareDecodeConfig` is copied by `Player` and applied the next time the
video decoder opens. Changing it while media is loaded interrupts the current
open/read operation and asynchronously reopens the media. The generic core
maps `HardwareDeviceType` to FFmpeg's internal hardware-device selection,
checks the codec's advertised hardware pixel format, and keeps the platform
types private. A backend can additionally request a registered FFmpeg decoder
by wrapper identity and attach an application surface generation; both values
participate in asynchronous configuration replacement. An unknown device type
selects the ordinary software path.

An in-tree hardware backend may attach a `HardwareDecodeDevice` to the config.
The public token reports only its generic device type and opaque native
identity; it is a cheap reference-counted value and keeps the backend-created
FFmpeg device context alive. Its FFmpeg bridge is a private, uninstalled core
header. `Player` takes its own device-context reference before decoder open and
rejects a token whose device type differs from
`HardwareDecodeConfig::deviceType`, using the configured software-fallback
policy. Replacing a supplied token while media is loaded also reopens the
decoder. If no token is supplied, core retains its existing FFmpeg-created
device behavior.

`HardwareFrame` is a cheap reference-counted view over backend-owned frame
storage. A native handle is an opaque integer tagged by its role and remains
valid while the frame is alive. CPU mapping returns a reference-counted
`HardwareFrameMapping`, whose destructor ends the mapping. Backend-specific
headers are responsible for converting opaque handles to strong D3D, Vulkan,
OpenGL, Android, or OHOS types. `HardwareFrameInterop` describes source/target
support and imports frames without committing to a runtime plugin ABI.

These are compile-time C++ contracts for targets built with a compatible
toolchain. They are not a stable cross-compiler dynamic-plugin boundary.

The contract implementation is covered by mock backend tests. Deterministic
audio-clock tests cover pause, seek, media replacement, stop, and shutdown.
The test-only simulated sink adds configurable format negotiation, capacity,
latency, explicit or query-driven buffer consumption, underrun, flush, and
drain behavior without a wall clock. Its player tests cover resampling, A/V
device-master timing, seek, loop, media replacement, natural-end drain, and
monotonic fallback. Its physical-PCM mode also verifies that a time-stretched
device clock is mapped back to media time.
The playback test also verifies that two keyed `VideoRenderAPI` instances and
the legacy `setVideoRenderer()` callback can render the same decoded frame
without replacing one another. CPU-renderer tests decode a lossless RGB frame
and verify scaled BGRA, RGBA, and Gray8 output plus padded-stride safety.
Audio-resample tests convert deterministic 8 kHz mono PCM to 16 kHz stereo S16
and verify channel data, drain timing, sample counts, and seek reset behavior.
Audio-time-stretch tests verify 0.75x and 1.5x output duration, 440 Hz pitch,
timestamp discontinuities, reset/drain behavior, mid-playback rate changes,
physical-device-clock mapping, and the exact 1.0 Player bypass.
Audio-file tests verify RIFF/WAVE headers and little-endian samples, then run
the player and converter to produce an exact 64,000-byte 16 kHz stereo S16
payload from deterministic 8 kHz mono input.
The portable SPSC queue shared by AAudio and OHAudio is tested for capacity,
wraparound, timestamp continuity, rejection, reset, and concurrent
single-producer/single-consumer access without a device. The connected
Android harness additionally verifies Float32 negotiation, real-time
presentation, monotonic device-clock samples, non-negative combined latency,
pause/resume, background/foreground recovery, drain, and clean close on an
AAudio-capable device.
The connected OHOS HAP additionally verifies 48 kHz Float32 negotiation,
real-time OHAudio presentation, device-master clock samples, combined latency,
pause/resume, seek/flush, loop-boundary drain, and clean close while preserving
the software Vulkan/OpenGL ES selector result.
The OHOS HAP then verifies OHCodec H.264/HEVC direct-surface outputs with
explicit timed present/drop, seek/flush, media replacement, stop, real
background/foreground surface-generation replacement, stale-token rejection,
bounded retained output, and decoder/output lifetime through shutdown.
Its raw OpenGL ES phase additionally validates exact normalized-PTS Dolby
Vision attachment for Profile 5 and Profile 8.4 with per-frame
queued/matched/released counters and no implicit-RGB input.
The connected Android harness separately verifies MediaCodec H.264/HEVC
direct-surface outputs with explicit present/drop, seek/flush, media
replacement, stop, surface-generation replacement, stale-token rejection,
decoder/output lifetime, and NativeActivity shutdown.
It then decodes H.264 and HEVC into separate private GPU-sampled
`AImageReader` surfaces, imports external-format `AHardwareBuffer` images into
Vulkan, samples aligned native allocations using their visible crop, and
returns one release sync fd per import. The device result requires bounded
pending images and zero decoded-source CPU map, software transfer, staging
copy, and renderer upload counters.
The same path decodes the checksum-pinned FFmpeg FATE profile 8.4 sample with
MediaCodec: all 100 decoded frames retain FFmpeg-parsed RPU metadata, 97 are
rendered through libplacebo from raw external-format Y/Cb/Cr, and all 97
imports return release fences with zero CPU map/transfer/staging/upload.
Independent OpenGL ES phases import 99 H.264 and 180 HEVC
AHardwareBuffer/EGLImages through the raw-YCbCr path, return a release fence
for every imported image, survive seek plus EGL surface recreation, and retain
the same zero-CPU-copy counters. A forced Vulkan failure rebinds MediaCodec to
a new OpenGL AImageReader and presents another 180 raw frames without retrying
the retired Vulkan image. Manual Profile 5 validation is recorded in
[`DECISIONS.md`](DECISIONS.md).
Windows D3D11 tests use the WARP device for deterministic offscreen rendering
and cover RGB, YUV420P, NV12, and synthetic P010 PQ/HLG input; SDR tone
mapping; FP16 scRGB and RGB10/PQ numeric output; viewport, aspect ratio,
rotation, resize, target recreation, foreign-device rejection, and
missing-surface events. Native HWND and composition flip-model tests exercise
`IDXGIOutput6`, `SetColorSpace1`, SDR-white lookup, the current Windows HDR
state, composition-monitor resolution, and display switching when two outputs
share the adapter. With Windows HDR enabled on a PHL 27B1U7903, the native
test verifies a 10-bit
G2084/P2020 output, system SDR white, nonzero panel luminance, and FP16 scRGB
highlights above `1.0`. D3D11VA tests cover selected-device creation, shared
locking, bounded
extra frames, native texture/slice validation, and invalid handles; an H.264
hardware integration test covers mapping, pause/resume, seek, media
replacement, stop, target recreation, and retained source/import access after
player shutdown, with an explicit software fallback result when the adapter
has no matching decoder profile. A capability-gated D3D12-generated HEVC
  Main10 test uses PQ/BT.2020 metadata and verifies P010 D3D11VA decode,
  default visible-region copying, explicit direct decoder sampling, raw-plane
  libplacebo rendering, FP16 scRGB output near the Windows absolute luminance
  encoding for its 1000-nit sample, zero CPU mapping, and pixel readback.
  WASAPI device and strict native H.264/AAC example tests pass with an
active render endpoint and are explicitly skipped when a Windows session
exposes no endpoint.

## Architecture summary

See [`ARCHITECTURE.md`](ARCHITECTURE.md) for the complete ownership, threading,
libplacebo color-pipeline, Android zero-CPU-copy, HDR, and packaging boundaries.

```text
Player facade
  ├─ async control + FFmpeg demux worker
  ├─ bounded compressed-packet queues
  │    ├─ audio-decode worker
  │    └─ video-decode worker
  ├─ bounded decoded-audio queue
  │    └─ audio-output worker
  │         ├─ optional PCM conversion + pitch-preserving time stretch
  │         └─ physical device-clock snapshots mapped to media time
  ├─ bounded timestamp-ordered presentation queue
  │    └─ video/frame notification worker with late-frame dropping
  ├─ audio-device master clock with monotonic fallback
  ├─ reference-counted AudioFrame/VideoFrame/SubtitleFrame
  ├─ application render scheduling
  │    └─ keyed VideoRenderAPI instances on native render threads
  └─ optional backend contracts
       ├─ libswscale CPU image-buffer renderer
       ├─ D3D11 software-frame renderer with borrowed Windows resources
       ├─ mobile Vulkan/OpenGL ES selection and bounded recovery policy
       ├─ libswresample interleaved PCM converter
       ├─ FFmpeg atempo pitch-preserving audio time stretcher
       ├─ libass text/ASS owning coverage-bitmap rasterizer
       ├─ RIFF/WAVE diagnostic PCM file sink
       ├─ WASAPI shared-mode device sink with IAudioClock clocking
       ├─ AAudio device sink with callback-safe SPSC buffering
       ├─ D3D11VA decoder producing retained texture-array slices
       ├─ MediaCodec decoder producing direct-surface present/drop tokens
       ├─ MediaCodec/AImageReader Vulkan hardware-buffer interop
       ├─ lifecycle-connected AudioSink
       └─ HardwareFrame + HardwareFrameInterop
```

The legacy QtAV sources remain unchanged while capabilities are migrated. New
platform backends should target this module rather than adding more Qt
dependencies to the legacy library.

See [MIGRATION.md](MIGRATION.md) for the QtAV API mapping, current limitations,
and threading contract. See [PLAN.md](PLAN.md) for active milestone status,
the next task, and backend implementation order. The accepted Windows
D3D11VA device, frame-lifetime, and zero-CPU-copy interop design is recorded in
[D3D11VA.md](D3D11VA.md).
The shared Android/OHOS mobile renderer, native lifecycle, hardware-output,
audio, and connected-device test boundaries are recorded in
[`MOBILE.md`](MOBILE.md).
