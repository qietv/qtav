# QtAV to QtAVCore migration

QtAVCore is an incremental replacement, not a compatibility wrapper. Legacy
QtAV remains buildable while callers move to the new API one integration at a
time.

QtAVCore requires FFmpeg 8.0 or newer. Compatibility code for FFmpeg 5–7 is
intentionally not carried in `modern/`; this does not change the dependency
range of the legacy root QtAV implementation.

## API mapping

| Legacy QtAV | QtAVCore |
| --- | --- |
| `QtAV::AVPlayer` | `qtav::Player` |
| `setFile(QString)` | `setMedia(std::string)` |
| `play()`, `pause()`, `stop()` | `setState(qtav::State)` |
| Qt signals | `std::function` callbacks |
| `QVariant` properties | string properties |
| `VideoRenderer::receive()` | `onVideoFrame()` |
| Qt paint/update events | normally owned by a high-level output such as `D3D11VideoOutput`; `setRenderCallback()` for external-context integration |
| renderer paint method | normally owned by a high-level output; `renderVideo()` and `VideoRenderAPI` for external-context integration |
| `AudioOutput` | `onAudioFrame()` and optional `setAudioSink()` |
| `QThread` playback workers | standard C++ demux/decode, audio-output, and presentation workers with bounded queues |
| `QString`, `QList`, `QImage` frame API | STL values and reference-counted frame views |

For ordinary Windows composition presentation, `QtAV::OutputD3D11` owns the
D3D11 device, swap chain, render target, redraw coalescing, render thread,
`renderVideo()`, `Present()`, and Advanced Color policy. The application
supplies only its hosting HWND, a native surface-binding callback, attaches a
`Player`, and forwards surface size or composition-scale changes. The default
prefers an FP16 scRGB HDR layer and automatically tracks display moves and the
Windows HDR setting.

The lower-level application-owned contract remains available for engines that
already own a graphics context or require multiple/custom render targets:

1. decoding makes a frame current;
2. `setRenderCallback()` asks the application to schedule a redraw;
3. the application calls `renderVideo()` on its native render thread;
4. the configured renderer consumes the reference-counted frame.

## Implemented

- asynchronous load and playback state;
- prepare, pause/resume, seek, stop, playback rate;
- A-B range and finite/infinite looping;
- FFmpeg protocol and demux support;
- best-stream audio/video selection;
- FFmpeg send/receive software decoding;
- decoded video and audio frame callbacks;
- structured video color-space and HDR10 static metadata on `VideoFrame`;
- compile-time video-render, audio-sink, and hardware-frame interop contracts;
- optional audio-sink playback output with device-master clock fallback;
- optional libswresample conversion to negotiated interleaved PCM;
- optional `WavAudioSink` diagnostic output through `QtAV::AudioFile`;
- optional macOS `CoreAudioAudioSink` device output through
  `QtAV::AudioCoreAudio`;
- optional Windows `WasapiAudioSink` shared-mode device output through
  `QtAV::AudioWASAPI`;
- optional Android `AAudioAudioSink` output through `QtAV::AudioAAudio`, with
  bounded callback-safe PCM buffering, presentation timestamps, latency, and
  disconnect recovery;
- optional high-level Windows composition presentation through
  `QtAV::OutputD3D11`, including owned D3D11 resources, render scheduling,
  D3D11VA configuration, zero-CPU-map interop, HDR/SDR output selection,
  per-frame display tracking, resize, presentation, and teardown;
- optional D3D11VA hardware decode through `QtAV::HWD3D11VA`, using the
  application-selected `D3D11DeviceAccess` and retained decoder texture-array
  slices;
- optional VideoToolbox hardware decode through `QtAV::HWVideoToolbox`, with
  reference-counted `CVPixelBuffer` frames and explicit software fallback;
- optional Android H.264/HEVC MediaCodec hardware decode through
  `QtAV::HWMediaCodec`, using an application-supplied versioned
  `ANativeWindow` and move-only direct-surface present/drop tokens;
- optional Android MediaCodec/Vulkan texture interop through
  `QtAV::InteropMediaCodecVulkan`, using a private GPU-sampled `AImageReader`,
  retained `AHardwareBuffer` external-format import, and acquire/release
  synchronization without mapping or re-uploading decoded pixels;
- optional `QtAV::InteropCVMetal` import of limited/full-range VideoToolbox
  NV12/P010 pixel-buffer planes into Metal textures without a CPU map or copy;
- optional platform-neutral `QtAV::RenderVulkan` software-frame rendering and
  Android `QtAV::RenderVulkanAndroid` surface/swapchain adaptation using
  application-owned Vulkan context objects and NativeActivity lifecycle;
- optional platform-neutral `QtAV::RenderOpenGL` OpenGL ES 3.x software-frame
  rendering with explicit SDR/PQ/HLG target encoding and Android
  `QtAV::RenderOpenGLAndroid` EGL/window adaptation for native RGB10_A2 HDR or
  explicit RGBA8/sRGB fallback;
- optional platform-neutral `QtAV::RenderMobile` policy that keeps one
  `VideoRenderAPI` attached across Vulkan-preferred startup, bounded same-API
  recovery, one-way OpenGL ES fallback, and the no-renderer state;
- multiple video renderer instances keyed by an application opaque pointer;
- libswscale CPU rendering into application-owned packed image buffers;
- D3D11 rendering of decoded software frames into an application-provided
  current render-target view, with optional swap-chain-driven Windows
  Advanced Color SDR, FP16 scRGB, and RGB10 HDR10 presentation;
- shared retained D3D11 device/immediate-context access and recursive
  synchronization through `QtAV::PlatformWindows`;
- Metal rendering of decoded software frames into an application-provided
  current texture or drawable;
- media and track information;
- interruptible FFmpeg I/O when media changes or playback stops;
- standalone static/shared CMake builds and installable package metadata;
- a macOS-hosted Android arm64 cross-build and NativeActivity
  connected-device harness proving QtAVCore/FFmpeg 8 software A/V decode,
  Vulkan presentation, OpenGL ES/EGL native-HDR plus SDR fallback, AAudio
  output, MediaCodec H.264/HEVC direct-surface decode, and private-AImageReader
  Vulkan texture import without Qt.

The `VideoRenderAPI` and `AudioSink` contracts are connected to `Player`.
The default decode path remains software-only. Applications can pass the
backend-provided VideoToolbox `HardwareDecodeConfig` before opening media to
select FFmpeg's VideoToolbox hardware path. Hardware video frames attach a
reference-counted `HardwareFrame`; the Apple-specific accessor returns its
borrowed `CVPixelBufferRef`, while the generic contract can perform an
explicit read mapping to CPU memory. Device creation and pixel-format
negotiation failures either report `decoder.hardware.fallback` and continue
in software or report `decoder.hardware.error`, according to the selected
fallback policy.
Android applications can instead create a backend-specific
`MediaCodecSurface`, pass `mediaCodecHardwareDecodeConfig()` before playback,
and turn each MediaCodec hardware frame into a `MediaCodecFrame`.
`present()`, monotonic-time `presentAt()`, and `drop()` are mutually exclusive
decisions for that output. A new native-window generation requires a new
surface token and asynchronous decoder reopen; foreign or stale generations
are rejected when the application validates the output against its current
token. Retained MediaCodec frames keep their FFmpeg decoder context alive so
queue invalidation, seek, stop, media replacement, and surface recreation
cannot free the codec before its output buffers are released.
Applications that link `QtAV::InteropMediaCodecVulkan` can instead construct
a `MediaCodecVulkanInterop` against their Vulkan device, pass its private
surface to `mediaCodecHardwareDecodeConfig()`, and bind the same interop to
`AndroidVulkanVideoRenderer`. The interop correlates MediaCodec outputs with
asynchronously acquired private `AImage` timestamps, imports retained
`AHardwareBuffer` memory with driver-provided YCbCr/external-format sampling,
and returns a Vulkan release sync fd through asynchronous image deletion.
Visible crop coordinates are preserved when native codec allocations have
padded dimensions. Pending images are bounded and the path has no implicit
CPU mapping, software transfer, staging, or renderer-upload fallback.
`HardwareDecodeConfig` can also carry a copied `HardwareDecodeDevice` token
created by an in-tree backend. The token exposes only a generic device type
and opaque native identity in the installed core API while privately retaining
the backend's FFmpeg hardware-device context. This is the common bridge for
decoding on an application-selected native device; changing the token while
media is open causes the same asynchronous decoder reopen as changing the
requested hardware type.
Applications that link `QtAV::InteropCVMetal` can bind a
`CVMetalFrameInterop` to `MetalVideoRenderer`. Supported limited- and
full-range bi-planar NV12 and P010 `CVPixelBuffer` planes are exposed to the
render command as retained Metal textures, without calling
`HardwareFrame::map()` or staging through CPU memory. Imported frame resources
remain alive until asynchronous Metal execution completes.
`VideoFrame::colorSpaceInfo()` replaces string parsing for range, primaries,
transfer, matrix, and chroma location. HDR10 mastering-display and content
light side data are copied into toolkit-independent value types. Metal uses
that metadata for limited/full-range YUV conversion, BT.601/BT.709/BT.2020
matrix selection, PQ/HLG handling, and source-primary conversion. The complete
Apple EDR path accepts an application-owned `CAMetalLayer` and active
`NSScreen`/`UIScreen`, configures `RGBA16Float`, extended-linear BT.2020,
`wantsExtendedDynamicRangeContent`, and HDR10/HLG `CAEDRMetadata` before
acquiring each drawable, and preserves BT.2020 primaries plus linear HDR
values above reference white. System tone mapping and renderer-controlled
live-headroom adaptation are explicit modes. The older
`ExtendedLinearSRGB` output remains available, but deliberately converts into
the narrower BT.709/sRGB gamut and is not the full HDR10/BT.2020 path.
`AudioSink` can use an injected `AudioFrameConverter` when decoded and device
PCM formats differ. Applications link `QtAV::AudioResample` and pass a
`SwresampleAudioConverter` through `Player::setAudioFrameConverter()`.
`AudioSink::drain()` is called after each completed playback segment, including
a loop boundary, after the converter is drained and before the final sink
close; the default implementation is a no-op for existing synchronous or
non-queuing sinks. The CPU renderer currently supports full-surface `Stretch`
rendering with no rotation. The Apple-only
Objective-C++ Metal renderer supports Fit, Fill, Stretch, custom viewports,
resize, and all right-angle rotations for software YUV, NV12/NV21, P010, and
RGB-family frames. Its strongly typed device and command queue are borrowed,
and the application supplies the current texture or drawable for each render
call.

On macOS, `CoreAudioAudioSink` follows the default output device or accepts an
explicit backend-specific `CoreAudioDevice`. It negotiates Float32
mono/stereo PCM at the device's nominal sample rate, so decoded formats
normally use the injected `SwresampleAudioConverter`. The sink copies PCM into
its native AudioQueue pool, implements pause/flush/natural-end drain, and
provides the device-master clock and latency consumed by `Player`.

On Windows, `WasapiAudioSink` follows the default multimedia render endpoint
or accepts an explicit owning `WasapiEndpointId`. It negotiates interleaved
Float32 mono/stereo PCM at the endpoint mix rate, so decoded formats normally
use `SwresampleAudioConverter`. A dedicated multimedia-class thread owns COM,
the event-driven WASAPI client, and copied PCM queue. The sink implements
pause/flush/natural-end drain and exposes a cached `IAudioClock`-based
device-master position plus engine and stream latency.

On Android API 26 or newer, `AAudioAudioSink` follows the default output route
or accepts a backend-specific integer `AAudioDeviceId`. It negotiates
interleaved Float32 mono/stereo PCM and uses
`SwresampleAudioConverter` when decoded sample rate, channel layout, or sample
format differs. Accepted PCM enters a fixed-capacity SPSC queue; the native
real-time callback only copies or clears preallocated memory and updates
atomics. `AAudioStream_getTimestamp()` anchors the device-master clock, and
reported latency includes both the native pipeline and backend queue. A
non-callback worker observes route changes and rebuilds disconnected
default-route streams using the same negotiated format.

On Windows, `D3D11DeviceAccess` verifies and retains an application-selected
`ID3D11Device` and its immediate context. `D3D11VideoRenderer` accepts that
shared access (or creates one from its compatibility constructor), borrows the
render-target view and optional `IDXGISwapChain3` returned by an application
callback, and holds the shared recursive context guard while rendering.
Applications issuing concurrent calls on the same immediate context must
acquire `D3D11DeviceAccess::contextGuard()` or provide equivalent external
serialization. `tryContextGuard()` is the non-blocking alternative used by the
real-time renderer; context or renderer contention declines that render
attempt instead of waiting on the native/UI render thread. The renderer
uploads software RGB, YUV, NV12/NV21, P010, and gray frames; handles
range/matrix, PQ/HLG, primaries, and tone mapping in its pixel shader; and
supports SDR 8-bit, FP16 scRGB, and RGB10/PQ targets.
Supplying the swap chain enables per-frame `IDXGIOutput6` discovery,
`SetColorSpace1()`, Windows SDR-white lookup, and automatic display/HDR-setting
changes. Resize, custom viewports, aspect handling, right-angle rotation, and
render-target recreation remain supported. Windows SDK types stay in
platform/backend headers and never enter a core public header.

Applications that do not need to share a D3D11 device should prefer
`D3D11VideoOutput`. Its `attach()` call takes exclusive ownership of the
player's default render slot and render callback until `detach()`, configures
D3D11VA against the output-owned device, and installs the D3D11 Video
Processor interop path. With a hosting HWND, its default `PreferHdr` policy
creates an FP16 scRGB composition layer, resolves the current monitor through
`IDXGIOutput6`, configures the swap-chain color space, and preserves PQ/HLG
output while Windows HDR is active. `RequireHdr` reports unavailable HDR
instead of presenting SDR, while `SdrOnly` keeps an explicit BGRA8 path.
`colorInfo()` exposes the active contract for diagnostics. The player and
native composition surface must outlive that attachment. Direct
`D3D11VideoRenderer`, `setRenderCallback()`, and `renderVideo()` use remains
the advanced path for externally owned graphics contexts, offscreen targets,
or custom presentation loops.

`d3d11vaHardwareDecodeConfig()` creates FFmpeg's D3D11VA device on the same
retained device access, installs callbacks for the shared recursive lock, and
requests a bounded number of extra decoder surfaces. `D3D11VAFrame` retains a
decoded NV12/P010 texture-array slice and validates its native resource before
returning borrowed D3D11 pointers. The core `NativeHandle` now carries an
optional subresource index, while Windows SDK and FFmpeg declarations remain
outside installed core headers. Explicit CPU mapping, software fallback, seek,
media replacement, stop, and retained lifetime after player shutdown are
implemented. `QtAV::RenderD3D11` now exposes decoder-independent
`D3D11HardwareFrameInterop` and retained `D3D11TextureFrame` interfaces for
the adapter layer; imported texture and shader-view pointers remain valid
while the texture-frame object lives. `QtAV::InteropD3D11` implements the
adapter with same-device validation and a D3D11 Video Processor pass from the
decoder NV12/P010 array slice to a shader-readable SDR BGRA8, FP16 scRGB, or
RGB10/PQ intermediate. The imported texture now reports its DXGI format and
color space. PQ/BT.2020 hardware frames therefore stay HDR through the
zero-CPU-map path instead of being unconditionally converted to SDR. The
renderer consumes that result, reports D3D11 hardware-frame capability, and
offers an explicit, disabled-by-default software mapping fallback through
`setAllowSoftwareMappingFallback()`. The interop and renderer use the same
recursive context guard; foreign devices are rejected before context access.
Windows integration coverage includes generated H.264/NV12 and PQ/BT.2020
HEVC Main10/P010 zero-CPU-map presentation, HDR-preserving FP16 pixel
readback, swap-chain Advanced Color state, display switching, pause/resume,
seek, media replacement, explicit stop, target recreation, and retained
source/import lifetime after player shutdown. The strict generated H.264/AAC
console test passes with an active WASAPI render endpoint and audible output,
while sessions without an endpoint report a CTest skip. The retained-resource
contract remains documented in [D3D11VA.md](D3D11VA.md).

Hardware imports are retained through Video Processor and draw submission,
then released without a per-frame completion query. All decoder, interop, and
renderer GPU submissions use the same serialized immediate context, so later
decoder-surface reuse remains ordered after earlier reads while D3D11 retains
resources referenced by queued commands. This avoids both decoder-surface
starvation and query-induced driver stalls after repeated seeks.

For offline PCM inspection, `WavAudioSink` negotiates an interleaved output
format and writes a standard RIFF/WAVE file. It does not expose a device clock
or pace playback. Decoded planar audio therefore normally uses
`SwresampleAudioConverter` before reaching the file sink.

## Deliberately deferred

- remaining platform audio device implementations (ALSA/PulseAudio, OHAudio);
- the OHOS EGL adapter and Vulkan OHOS/Linux validation;
- remaining hardware decoders (VAAPI and OHCodec) plus Linux, Android
  OpenGL ES, and OHOS GPU zero-CPU-copy interop;
- subtitle decoding and libass rendering;
- active track switching after load;
- buffering policy for live/network streams;
- audio time-stretch without pitch change;
- compressed Dolby passthrough, Atmos object rendering, and Dolby Vision.

The Android harness is currently an integration checkpoint rather than a
legacy QtAV API replacement. It proves the NDK, packaging, signing,
connected-device logging, software decode, and Vulkan/OpenGL ES presentation.
`MobileVideoRendererSelector` now implements the accepted Android/OHOS policy:
it prefers application-created Vulkan, performs bounded same-API recreation
for recoverable surface loss, switches one-way to an application-created
OpenGL ES/EGL backend after fatal or repeated Vulkan failure without reopening
media, and leaves audio plus decoded-frame callbacks available if both fail.
Native renderer factories remain in the application/platform layer, the
selector has no SDL3 dependency, and decoder and interop fallback stay
independent.
Android MediaCodec direct-surface H.264/HEVC output is now stable, including
explicit present/drop, seek/flush, media replacement, stop, stale-surface
rejection, background/foreground surface recreation, and shutdown on the
recorded device. The separate Android Vulkan native-buffer adapter is now
implemented and device-validated; Android OpenGL ES and both OHOS
native-buffer adapters remain planned. Their
zero-CPU-copy contract forbids decoded-pixel mapping, software transfer, CPU
staging, and re-upload; it requires retained native-buffer lifetime, explicit
producer/release synchronization, and capability-gated format support. A
Vulkan-to-OpenGL ES switch attempts compatible GLES native import for
subsequent frames, then follows the caller's explicit direct-surface,
software-decode, or no-video policy instead of silently copying a hardware
frame. Android Vulkan now uses private GPU-sampled
`AImageReader`/`AHardwareBuffer` import; the remaining GLES design uses
`SurfaceTexture` with `GL_TEXTURE_EXTERNAL_OES`. OHOS GLES uses
`OH_NativeImage` with an external-OES texture. OHOS Vulkan remains conditional
on adding a retained
`OH_AVBuffer`/`OH_NativeBuffer` bridge: the current FFmpeg 8 OHCodec buffer
branch calls `OH_AVBuffer_GetAddr()` and `av_image_copy2()`, so it is not a
zero-CPU-copy source as-is.
The Vulkan engine now has offscreen goldens, a bounded three-frame resource
ring, Android background/foreground surface recreation coverage, and numeric
P010/BT.2020 PQ/HLG checks for mastering-display, MaxCLL, default luminance,
SDR tone mapping, native 10-bit HDR10/PQ plus HDR10/HLG encoding, HLG-to-PQ
conversion, and FP16 extended-linear output above reference white. The Android
adapter now exposes output preference and selected-surface queries, enables
HDR format/color-space selection when the application created its instance
with `VK_EXT_swapchain_colorspace`, and submits static metadata when the
borrowed device was created with `VK_EXT_hdr_metadata`. The recorded
Adreno 830 device passes a required HDR10/PQ swapchain run across
background/foreground surface recreation, while Android reports the presented
layer as HDR and a synthetic metadata-bearing P010/BT.2020/PQ frame exercises
the complete source-to-present path. The OpenGL ES 3.x engine and Android EGL
adapter now cover the advertised software formats, common geometry, SDR color
conversion, P010/PQ-to-SDR fallback, explicit BT.2020/PQ and BT.2020/HLG
encoding, and exact RGB10_A2 surface selection. The recorded device presents
P010/BT.2020/PQ through EGL and Android independently recognizes that surface
as an HDR layer. `QtAV::AudioAAudio` now presents converted Float32 PCM,
publishes the playback-master clock and latency, and survives the harness
pause/resume plus background/foreground lifecycle. MediaCodec direct-surface
presentation now passes H.264 and HEVC connected-device coverage with explicit
present/drop decisions, seek, media replacement, stop, surface-generation
replacement, stale-token rejection, and clean shutdown. Android
MediaCodec/Vulkan interop now passes H.264 and HEVC private-AImageReader
import with native YCbCr/external-format sampling, one returned release fence
per import, bounded pending images, and zero decoded-source
map/transfer/staging/upload counters. Android OpenGL ES, OHOS, and Linux
texture interop remain separate backend work under the responsibility and
lifecycle boundaries in
[`MOBILE.md`](MOBILE.md).

The current audio callback exposes the decoder's native sample format and
reference-counted planes. A platform audio sink should convert/resample only
when its device format requires it.

### Dolby formats

The core is codec-agnostic and uses the decoder registered by FFmpeg. AC-3,
E-AC-3, and TrueHD software decoding have been exercised through the audio
frame callback. This is PCM decode support only; IEC 61937/HDMI passthrough,
Atmos object rendering, Dolby Vision processing, licensing, and certification
are separate backend/product work.

## Threading rules

- control methods are thread-safe;
- demux, FFmpeg decode, asynchronous control, and state/status callbacks
  normally run on the playback worker;
- decoded audio/video frame callbacks and `setRenderCallback()` run on the
  presentation worker; the bounded video queue drops obsolete late frames
  when application presentation falls behind;
- media events normally run on the playback or audio-output worker; forwarded
  audio-sink events run on the backend's event thread;
- callbacks may request another player state, but must not destroy the player;
- `seek()` invalidates queued presentation generations without waiting on
  their queue locks; while a playing seek waits for actual output,
  `position()` is held at the target and status transitions
  `Loaded -> Buffering -> Loaded`; a clock-capable audio sink must publish a
  valid post-flush clock sample before playback time resumes, while
  callback-only playback resumes after the first new-generation item is
  delivered;
- initial playback still falls back to the monotonic clock after the first
  delivered buffer when a clock-capable sink has not yet produced a valid
  device-clock sample;
- an audio-device underrun re-enters `Buffering`, freezes the fallback clock,
  and leaves it frozen until output re-anchors instead of letting wall time
  advance without sound or video;
- A/V startup and playing seeks perform a bounded video preroll before device
  audio is released; presentation clock refresh is non-blocking and continues
  from the cached, submitted-audio-bounded device clock while a sink write is
  waiting for backend queue space;
- HTTP(S) inputs default to a 15-second FFmpeg I/O timeout plus bounded
  reconnect attempts; `avformat.rw_timeout`, `avformat.reconnect`, and the
  other FFmpeg protocol properties can override those defaults;
- `renderVideo()` runs synchronously on its caller and should be called from the
  thread that owns the native graphics context; it returns a negative value
  when no frame is ready or a retryable player/backend lock is busy, so native
  render loops should retry instead of blocking the UI/render thread;
- D3D11 renderer, decoder, interop, and application calls sharing one
  immediate context must serialize through the same
  `D3D11DeviceAccess::contextGuard()` or equivalent external locking; the
  renderer uses non-blocking `tryContextGuard()` and declines a frame on
  contention;
- `VideoRenderAPI::render()` runs synchronously inside `renderVideo()` and
  backend event callbacks may request another player state;
- audio-sink and video-render backend event callbacks run on the thread chosen
  by the backend and may request another player state;
- decoded audio crosses a bounded queue to a dedicated audio-output worker;
  ordinary conversion, sink writes, and primary device-clock sampling run
  there without the player mutex held and cannot be blocked by application
  rendering; presentation performs only a non-blocking opportunistic clock
  refresh and otherwise uses the cached sample;
- audio-sink/converter lifecycle and segment-end drain calls run on the
  playback worker, serialized with audio-output calls; `drain()` may block
  until queued audio is presented;
- player shutdown synchronizes its quitting predicate with each worker
  condition-variable mutex before notification, preventing a worker from
  sleeping after the final wake-up;
- a sink open that finishes after stop, seek, or media replacement is treated
  as stale and closed before it can become the active output;
- `Player::position()` reads a generation-checked cached device-clock snapshot
  and never calls a sink or waits behind a sink write;
- changing `HardwareDecodeConfig` while media is open interrupts and
  asynchronously reopens the decoder; hardware frame callbacks run on the
  presentation worker;
- replacing its supplied `HardwareDecodeDevice` token also reopens the
  decoder; a token/type mismatch follows the selected software-fallback
  policy before decoder open;
- frame data remains valid for as long as the copied `AudioFrame` or
  `VideoFrame` object is alive.
- an opaque hardware native handle remains valid while its `HardwareFrame`
  lives, and a CPU mapping remains valid while its mapping object lives.

## Recommended next implementation order

1. Use the CPU swscale renderer and image-buffer target as the reference
   software path.
2. Use the interleaved PCM libswresample backend when a sink negotiates a
   different format.
3. Complete the Apple production path by adding CoreAudio, then VideoToolbox
   and CVPixelBuffer/Metal interop.
4. Add hardware decode and zero-copy frame handles for that path.
5. Implement the accepted Windows D3D11VA device/frame contract, then add
   zero-CPU-copy texture rendering through D3D11 interop.
6. Add subtitle and multi-track switching.
7. Add live-stream buffering and recovery policies.

Each backend should remain optional so the core library never acquires a GUI
toolkit dependency.
