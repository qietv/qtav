# QtAV to QtAVCore migration

The current component and ownership model is documented in
[`ARCHITECTURE.md`](ARCHITECTURE.md); implementation order lives in
[`PLAN.md`](PLAN.md), and durable decisions live in
[`DECISIONS.md`](DECISIONS.md).

QtAVCore is an incremental replacement, not a compatibility wrapper. Legacy
QtAV remains buildable while callers move to the new API one integration at a
time.

QtAVCore requires FFmpeg 8.0 or newer. Compatibility code for FFmpeg 5–7 is
intentionally not carried in `modern/`; this does not change the dependency
range of the legacy root QtAV implementation.

QtAVCore migration targets Windows, Android, and OHOS only. The former macOS
and iOS backends and their migration notes are preserved under
[`../archived_apple/`](../archived_apple/) as unmaintained historical material;
they are no longer part of the active API, build, package, or support matrix.
Linux is also outside the active target matrix.

This support change removes the former Apple CMake targets and options plus
`HardwareDeviceType::VideoToolbox` and `HardwareDeviceType::Metal`. The unused
Linux `HardwareDeviceType::VAAPI` value and ALSA/PulseAudio/VAAPI placeholder
options were removed at the same time. Consumers must not probe or request
those identifiers from the active headers.

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
| renderer paint method | normally owned by a high-level output; reason-aware `renderVideoDetailed()` or compatibility `renderVideo()` plus `VideoRenderAPI` for external-context integration |
| `AudioOutput` | `onAudioFrame()` and optional `setAudioSink()` |
| `QThread` playback workers | standard C++ demux, independent audio/video decode, audio-output, and presentation workers with bounded queues |
| `QString`, `QList`, `QImage` frame API | STL values and reference-counted frame views |

For ordinary Windows composition presentation, `QtAV::OutputD3D11` owns the
D3D11 device, swap chain, render target, redraw coalescing, render thread,
reason-aware rendering, `Present()`, and Advanced Color policy. The application
supplies only its hosting HWND, a native surface-binding callback, attaches a
   `Player`, and forwards surface size or composition-scale changes. The default
prefers an FP16 scRGB HDR layer and automatically tracks display moves and the
Windows HDR setting.

The lower-level application-owned contract remains available for engines that
already own a graphics context or require multiple/custom render targets:

1. decoding makes a frame current;
2. `setRenderCallback()` asks the application to schedule a redraw;
3. the application calls `renderVideoDetailed()` (or the compatibility
   `renderVideo()`) on its native render thread;
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
- optional Windows `WasapiAudioSink` shared-mode device output through
  `QtAV::AudioWASAPI`;
- optional Android `AAudioAudioSink` output through `QtAV::AudioAAudio`, with
  bounded callback-safe PCM buffering, presentation timestamps, latency, and
  disconnect recovery;
- optional OHOS `OHAudioAudioSink` output through `QtAV::AudioOHAudio`, with
  negotiated Float32 PCM, the shared callback-safe SPSC buffer, hardware
  presentation timestamps, lifecycle control, and route/error recovery;
- optional high-level Windows composition presentation through
  `QtAV::OutputD3D11`, including owned D3D11 resources, render scheduling,
  D3D11VA configuration, zero-CPU-map interop, HDR/SDR output selection,
  per-frame display tracking, resize, presentation, and teardown;
- optional D3D11VA hardware decode through `QtAV::HWD3D11VA`, using the
  application-selected `D3D11DeviceAccess` and retained decoder texture-array
  slices;
- optional Android H.264/HEVC MediaCodec hardware decode through
  `QtAV::HWMediaCodec`, using an application-supplied versioned
  `ANativeWindow` and move-only direct-surface present/drop tokens;
- optional OHOS H.264/HEVC OHCodec decoder selection through
  `QtAV::HWOHCodec`, using a retained, versioned application-supplied
  `OHNativeWindow` and move-only direct-surface present/drop/timed tokens;
- optional OHOS OHCodec/Vulkan texture interop through
  `QtAV::InteropOHCodecVulkan`, using a private `OH_ConsumerSurface`, retained
  `OHNativeWindowBuffer`/`OH_NativeBuffer` import, and strict rejection unless
  Vulkan exposes an explicit sampled multi-plane format;
- optional Android MediaCodec/Vulkan texture interop through
  `QtAV::InteropMediaCodecVulkan`, using a private GPU-sampled `AImageReader`,
  retained `AHardwareBuffer` external-format import, and acquire/release
  synchronization without mapping or re-uploading decoded pixels;
- optional Android MediaCodec/OpenGL ES texture interop through
  `QtAV::InteropMediaCodecOpenGL`, using a private GPU-sampled `AImageReader`,
  AHardwareBuffer/EGLImage import, raw `GL_EXT_YUV_target` component sampling,
  native-fence synchronization, and no decoded-source mapping or upload;
- optional platform-neutral `QtAV::RenderVulkan` software-frame rendering and
  Android `QtAV::RenderVulkanAndroid` surface/swapchain adaptation using
  application-owned Vulkan context objects and NativeActivity lifecycle;
  libplacebo now owns Vulkan color conversion, scaling, tone mapping, output
  encoding, and FFmpeg-parsed Dolby Vision RPU reshaping;
- optional OHOS `QtAV::RenderVulkanOHOS` surface/swapchain adaptation using
  an ArkUI/XComponent `OHNativeWindow` and application-owned Vulkan context,
  plus a minimal signed-HAP integration harness for software-frame playback;
- optional platform-neutral `QtAV::RenderOpenGL` OpenGL ES 3.x rendering with
  libplacebo color conversion, scaling, tone mapping, output encoding, and
  FFmpeg-parsed Dolby Vision RPU reshaping, plus Android
  `QtAV::RenderOpenGLAndroid` EGL/window adaptation for native RGB10_A2 HDR or
  explicit RGBA8/sRGB fallback and OHOS `QtAV::RenderOpenGLOHOS` adaptation
  for a capability-verified RGBA8/sRGB baseline;
- one GPU semantic pipeline: libplacebo is the sole Windows D3D11, Vulkan, and
  OpenGL ES authority for color conversion, Dolby Vision, tone/gamut mapping,
  scaling, and output encoding. Platform interop only imports native buffers,
  preserves synchronization and lifetime, and exposes raw representation;
  backend-local shaders may normalize that representation but may not perform
  semantic color work;
- optional platform-neutral `QtAV::RenderMobile` policy that keeps one
  `VideoRenderAPI` attached across Vulkan-preferred startup, bounded same-API
  recovery, one-way OpenGL ES fallback, and the no-renderer state, with an
  explicit synchronous route for hardware frames that reconfigures subsequent
  decoder output instead of retrying a retired Vulkan-surface frame;
- multiple video renderer instances keyed by an application opaque pointer;
- libswscale CPU rendering into application-owned packed image buffers;
- D3D11 rendering of decoded software frames into an application-provided
  current render-target view, with optional swap-chain-driven Windows
  Advanced Color SDR, FP16 scRGB, and RGB10 HDR10 presentation;
- shared retained D3D11 device/immediate-context access and recursive
  synchronization through `QtAV::PlatformWindows`;
- media and track information;
- interruptible FFmpeg I/O when media changes or playback stops;
- standalone static/shared CMake builds and installable package metadata;
- an Android arm64 cross-build and NativeActivity
  connected-device harness proving QtAVCore/FFmpeg 8 software A/V decode,
  Vulkan presentation, OpenGL ES/EGL native-HDR plus SDR fallback, AAudio
  output, MediaCodec H.264/HEVC direct-surface decode, and private-AImageReader
  Vulkan texture import without Qt.

The `VideoRenderAPI` and `AudioSink` contracts are connected to `Player`.
The default decode path remains software-only. Hardware video frames attach a
reference-counted `HardwareFrame`; supported backend-specific accessors expose
strong native views while the generic contract can perform an explicit read
mapping to CPU memory. Device creation and pixel-format negotiation failures
either report `decoder.hardware.fallback` and continue in software or report
`decoder.hardware.error`, according to the selected fallback policy. Android
applications can create a backend-specific
`MediaCodecSurface`, pass `mediaCodecHardwareDecodeConfig()` before playback,
and turn each MediaCodec hardware frame into a `MediaCodecFrame`.
`present()`, monotonic-time `presentAt()`, and `drop()` are mutually exclusive
decisions for that output. A new native-window generation requires a new
surface token and asynchronous decoder reopen; foreign or stale generations
are rejected when the application validates the output against its current
token. Retained MediaCodec frames keep their FFmpeg decoder context alive so
queue invalidation, seek, stop, media replacement, and surface recreation
cannot free the codec before its output buffers are released.
For direct presentation, `setVideoFrameScheduler()` now supplies the target
steady-clock deadline on the video-decode worker; accepting the frame bypasses
the later `onVideoFrame()` and renderer callbacks so a small MediaCodec output
pool is not retained until ordinary presentation. `playbackStatistics()`
separates decoded/delivered video counts from queue-overflow and late drops.
Applications that link `QtAV::InteropMediaCodecVulkan` can instead construct
a `MediaCodecVulkanInterop` against their Vulkan device, pass its private
surface to `mediaCodecHardwareDecodeConfig()`, and bind the same interop to
`AndroidVulkanVideoRenderer`. The interop correlates MediaCodec outputs with
asynchronously acquired private `AImage` timestamps, imports retained
`AHardwareBuffer` memory with driver-provided YCbCr/external-format sampling,
and returns a Vulkan release sync fd through asynchronous image deletion.
Its configured width and height are optional default reader dimensions;
MediaCodec supplies the actual decoded buffer size, so applications do not
need to pre-probe media dimensions before creating the interop. The OpenGL ES
AImageReader interop follows the same default-size behavior.
`MediaCodecVulkanInterop::queueFrame()` releases an output and performs a
bounded wait for its matching AImage ownership transfer, allowing applications
to reserve a render slot and schedule Vulkan presentation independently.
`MediaCodecOpenGLInterop::queueFrame()` instead releases non-blockingly after
the application reserves bounded capacity; its AImageReader listener wakes the
graphics thread when the exact timestamp-correlated image arrives. OpenGL
window adapters should set `OpenGLVideoRenderer::setPresentCallback()` so the
platform submit (Android `eglSwapBuffers()`) runs before the interop exports
the image's release fence. This replaces adapters that presented only after
`render()` returned, which could leave default-framebuffer work outside the
release fence and eventually exhaust AImageReader acquisitions.
Vulkan image, memory, view, and YCbCr-conversion objects are reused by retained
`AHardwareBuffer` identity until AImageReader reports that allocation removed;
the interop statistics now distinguish per-frame imports from persistent
hardware-buffer imports, cache hits, removals, and the cache high-water mark.
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
`VideoFrame::colorSpaceInfo()` replaces string parsing for range, primaries,
transfer, matrix, and chroma location. HDR10 mastering-display and content
light side data are copied into toolkit-independent value types. Active
renderers use that metadata for range, matrix, transfer, primaries, and HDR
output decisions without exposing FFmpeg types.
`VideoFrame::hasDolbyVisionMetadata()` similarly reports parsed RPU frame
metadata without exposing its FFmpeg representation. The repository FFmpeg
MediaCodec overlay parses HEVC RPU NAL units with FFmpeg's built-in parser and
matches them to hardware output presentation timestamps before creating each
`VideoFrame`.
`AudioSink` can use an injected `AudioFrameConverter` when decoded and device
PCM formats differ. Applications link `QtAV::AudioResample` and pass a
`SwresampleAudioConverter` through `Player::setAudioFrameConverter()`.
`AudioSink::drain()` is called after each completed playback segment, including
a loop boundary, after the converter is drained and before the final sink
close; the default implementation is a no-op for existing synchronous or
non-queuing sinks. The CPU renderer currently supports full-surface `Stretch`
rendering with no rotation.

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

On OHOS API 23 or newer, `OHAudioAudioSink` requests the current output route
in fast mode, falls back to normal mode when required, and negotiates 48 kHz
interleaved Float32 mono/stereo PCM. It reuses the portable AAudio-proven SPSC
queue implementation, so the OHAudio write callback only copies or clears
preallocated memory and updates atomics. Hardware-committed frame timestamps
anchor the device-master clock; reported latency includes both submitted
native frames and backend-queued PCM. Pause, flush, natural-end drain,
interrupt handling, route-change reconstruction, and native error recovery
remain in the OHOS backend. The connected HAP validates native PCM delivery,
clock/latency, pause/resume, seek/flush, and loop-boundary drain; subjective
audibility remains a manual listening check.

OHOS applications can now create a backend-specific `OHCodecSurface` and pass
`ohCodecHardwareDecodeConfig()` before opening H.264 or HEVC media. The
backend retains the exact `OHNativeWindow` generation, creates FFmpeg's
`AV_HWDEVICE_TYPE_OHCODEC` device, and requests the explicit `*_ohcodec`
wrapper with an independent software-fallback policy. Generic hardware frames
identify `HardwareDeviceType::OHCodec` and carry the source surface identity
and generation without exposing OHOS or FFmpeg types through core headers.
`ohCodecFrame()` validates that identity and returns a move-only
`OHCodecFrame`; exactly one of `present()`, `presentAt()`, or `drop()` may be
used, and an undecided token drops on destruction. A final retained frame
release without an explicit decision also unconditionally drops/frees the
FFmpeg output instead of implicitly rendering it. Player applies the shared
surface-output packet-feed and output-retention bounds before invoking the
decode-worker scheduler. The complete connected HAP now passes H.264 and HEVC
direct output with 48/40 presentations and 5 drops per codec, pause/resume, a
2000 ms seek callback, media replacement, stop, background/foreground, surface
recreation, stale-generation rejection, and bounded retention
(`maxPending=2`, `pendingEnd=0`, `maxQueued=0`). Native hardware-frame texture
interop remains separate from that direct-surface path. The independent
`QtAV::InteropOHCodecVulkan` target now owns a private `OH_ConsumerSurface`,
presents exactly one retained OHCodec output into it, acquires and retains the
corresponding `OHNativeWindowBuffer`/`OH_NativeBuffer`, imports its acquire
sync fd, and keeps the buffer alive until the GPU completion timeline retires
the texture. It passes only explicit sampled multi-plane `VkFormat` values to
libplacebo and rejects opaque external formats without mapping, transfer,
staging, upload, or GPU normalization. The 2026-08-06 connected Mate 60 Pro
probe acquired real H.264 and HEVC buffers, but its driver exposed only
`VK_FORMAT_UNDEFINED` plus external format `1000156003`; the harness therefore
reported the expected strict `UNSUPPORTED` result. Direct sampling and
GPU-completion release remain to be validated on a device that exposes an
explicit multi-plane Vulkan format. The OpenGL ES fallback requires raw
`GL_EXT_YUV_target` sampling and RGBA16F GPU normalization, not implicit
external-OES YUV-to-RGB conversion.

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
range/matrix, PQ/HLG, primaries, Dolby Vision reshaping, tone mapping, gamut
mapping, scaling, and output encoding through libplacebo's D3D11 backend; and
supports SDR 8-bit, FP16 scRGB, and RGB10/PQ targets. The former handwritten
Windows HLSL color pipeline has been removed. Windows builds expose D3D11 as
their only QtAVCore GPU renderer and do not build the Vulkan or OpenGL render
targets.
Supplying the swap chain enables per-frame `IDXGIOutput6` discovery,
`SetColorSpace1()`, Windows SDR-white lookup, and automatic display/HDR-setting
changes. Resize, custom viewports, aspect handling, right-angle rotation, and
render-target recreation remain supported. Windows SDK types stay in
platform/backend headers and never enter a core public header.

Applications that do not need to share a D3D11 device should prefer
`D3D11VideoOutput`. Its `attach()` call takes exclusive ownership of the
player's default render slot and render callback until `detach()`, configures
D3D11VA against the output-owned device, and installs the D3D11 raw-plane
interop path. The retained `D3D11DeviceAccess` enables native immediate-context
multithread protection before the decoder and renderer workers share the
device; wrapping an external device now fails if that protection is
unavailable. With a hosting HWND, its default `PreferHdr` policy
creates an FP16 scRGB composition layer, resolves the current monitor through
`IDXGIOutput6`, configures the swap-chain color space, and preserves PQ/HLG
output while Windows HDR is active. `RequireHdr` reports unavailable HDR
instead of presenting SDR, while `SdrOnly` keeps an explicit BGRA8 path.
`colorInfo()` exposes the active contract for diagnostics. The player and
native composition surface must outlive that attachment. Direct
`D3D11VideoRenderer`, `setRenderCallback()`, and `renderVideoDetailed()` use
remains the advanced path for externally owned graphics contexts, offscreen
targets, or custom presentation loops. The output classifies transient render
failures, keeps only the latest pending frame, and retries on its private
thread. For immediate-context contention it reserves priority over new
FFmpeg-side acquisitions, performs an immediate bounded handoff wait, and only
then applies bounded backoff; recovered retries are not counted as terminal
drops.

Opaque video players may additionally select
`D3D11HdrPresentationMode::HDR10` and `DXGI_ALPHA_MODE_IGNORE` in
`D3D11VideoOutputOptions`. This creates an RGB10/PQ composition path while
Advanced Color is active; the default remains FP16 scRGB for general-purpose
composition and alpha blending.

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
the adapter layer; imported raw texture pointers and array-slice identity
remain valid while the texture-frame object lives. `QtAV::InteropD3D11`
implements same-device validation and retains the decoder NV12/P010 slice
without a Video Processor RGB pass. The renderer wraps plane-specific D3D11
views with libplacebo, attaches the exact FFmpeg Dolby Vision RPU before color
conversion, and renders directly to the selected SDR, FP16 scRGB, or RGB10/PQ
target. PQ/BT.2020 and Profile 5 hardware frames therefore preserve their raw
semantic input through the zero-CPU-map path. Direct plane wrapping uses no
normalization draw or intermediate source texture and therefore satisfies the
strict no-intermediate source zero-copy definition. The renderer reports D3D11
hardware-frame capability and
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

Hardware imports, their copied core frames, and borrowed targets are retained
until a D3D11 completion event reports that the libplacebo draw has finished.
The renderer bounds this queue at three submissions, and
`D3D11VideoRenderer::flush()` explicitly drains it before target resize or
replacement. Decoder, interop, and renderer submissions share a natively
multithread-protected immediate context plus QtAVCore's recursive guard.
Every successfully imported D3D11VA frame uses libplacebo's fast sampling
policy without the optional GPU histogram peak-detection pass. Successful
per-frame submission remains asynchronous, without `pl_gpu_finish()`, and
Dolby Vision raw NV12/P010 input samples the retained decoder array slice
directly without a GPU copy. Software frames keep the default render
parameters. Normal flush, resize, media replacement, failure cleanup, and
teardown still perform the explicit drains required by their lifecycles.
This vendor-neutral policy is accepted on NVIDIA, Intel, and AMD. A separately
reported visual 4K cadence issue on an AMD integrated GPU remains a performance
investigation rather than an imported-frame correctness regression. Closure
requires a same-build Intel performance regression and exact hardware, driver,
and objective cadence/stage data from both final test devices.

For offline PCM inspection, `WavAudioSink` negotiates an interleaved output
format and writes a standard RIFF/WAVE file. It does not expose a device clock
or pace playback. Decoded planar audio therefore normally uses
`SwresampleAudioConverter` before reaching the file sink.

## Deliberately deferred

- broader OHOS Vulkan/OpenGL ES format, HDR, and lifecycle validation;
- OHOS explicit-multi-plane Vulkan device validation for the implemented
  retained `OH_NativeBuffer` path, plus broader raw `GL_EXT_YUV_target`
  OpenGL ES device coverage;
- subtitle decoding and libass rendering;
- active track switching after load;
- buffering policy for live/network streams;
- audio time-stretch without pitch change;
- compressed Dolby passthrough, Atmos object rendering, Dolby Vision
  enhancement-layer residual reconstruction, display tunnelling, licensing,
  and certification.

The Android and OHOS harnesses are integration checkpoints rather than a
legacy QtAV API replacement. They prove cross-compilation, packaging, signing,
connected-device logging, software decode, and platform presentation. The
OHOS checkpoint now covers software-frame Vulkan, OpenGL ES, forced initial
OpenGL ES selection, recoverable native-window recreation, and fatal one-way
Vulkan-to-OpenGL ES fallback without reopening media. It also covers OHAudio
PCM delivery, device-master timing, pause/resume, seek/flush, and segment-end
drain, plus the complete OHCodec H.264/HEVC direct-surface lifecycle matrix.
Android covers the complete Vulkan/OpenGL ES native-buffer interop matrix
below. OHOS additionally covers the strict Vulkan native-buffer capability
gate with real H.264/HEVC outputs: the current device exposes only an opaque
external format, so the expected `UNSUPPORTED` result is accepted while a
direct-plane PASS remains pending on suitable hardware.
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
recorded device. OHOS OHCodec direct-surface H.264/HEVC output now passes the
same lifecycle classes with bounded retained outputs and final-reference drop.
The separate Android Vulkan and OpenGL ES native-buffer adapters are now
implemented and device-validated. Their zero-CPU-copy contract forbids
decoded-pixel mapping, software transfer, CPU staging, and re-upload; it
requires retained native-buffer lifetime, explicit producer/release
synchronization, and capability-gated format support. This claim permits a
GPU-only normalization texture. The narrower strict no-intermediate
source zero-copy claim additionally requires an explicit graphics format and
plane mapping that libplacebo wraps directly, without a normalization draw. A
Vulkan-to-OpenGL ES switch attempts compatible GLES native import for
subsequent frames, then follows the caller's explicit direct-surface,
software-decode, or no-video policy instead of silently copying a hardware
frame. Android Vulkan uses private GPU-sampled `AImageReader`/AHardwareBuffer
import. Android GLES now uses its own private AImageReader, imports retained
AHardwareBuffers as EGLImages, and samples raw Y/Cb/Cr through
`GL_EXT_YUV_target`. A crop-aware RGBA16F normalization pass performs no color
conversion; libplacebo then applies Dolby Vision reshaping and the complete
SDR/PQ/HLG pipeline. Imports without the raw-component contract are rejected
for Dolby Vision rather than relying on implicit SurfaceTexture conversion.
Both Android presentation adapters also treat a republished identical
`ANativeWindow` with changed buffer geometry as a resize and refresh their
swapchain/EGL target without reopening the decoder.
The Vulkan-to-GLES policy is now connected: after the selector prepares the
OpenGL ES candidate, its synchronous hardware-frame callback rebinds
subsequent MediaCodec output to the OpenGL AImageReader producer or selects the
caller's direct-surface, software-decode, or no-video route. The current and
late frames from the retired Vulkan surface are discarded without mapping.
The connected Adreno 830 run injected fatal Vulkan failure after 30 successful
imports and continued the same H.264 media session with 180 raw EGLImage
imports and matching release fences; both interop paths reported zero
decoded-source map/transfer/staging/upload calls. These are zero-CPU-copy
results; the RGBA16F normalization path is not strict source zero-copy.

For OHOS, `QtAV::InteropOHCodecVulkan` implements the preferred retained
native-buffer path through `VK_OHOS_external_memory`. Surface-mode OHCodec
outputs do not expose usable native memory through their callback
`OH_AVBuffer`, so the interop presents the output into its private
`OH_ConsumerSurface` and acquires the corresponding retained
`OHNativeWindowBuffer`/`OH_NativeBuffer` from the consumer queue. It may claim
strict source zero-copy only when an explicit `VkFormat` and plane mapping
allow direct libplacebo wrapping. This strict target rejects an opaque external
format rather than adding a GPU normalization texture. The GLES fallback must
sample raw components through
`GL_EXT_YUV_target` into RGBA16F before libplacebo and is therefore also not
strict source zero-copy. Implicit external-OES conversion is not a target.
OHCodec/NativeImage may propagate the codec PTS unchanged in microseconds, so
the interop compares the observed value and its microsecond-to-nanosecond
candidate against the exact queued-frame PTS set, then stores and correlates
the selected value in nanoseconds. The consumer-surface bridge avoids the
software-copying FFmpeg OHCodec buffer-output branch and leaves the generic
core API free of OHOS types.
The Vulkan renderer now imports the application device through libplacebo.
`BorrowedVulkanDevice` therefore also requires its `VkInstance` and explicit
confirmation that Vulkan 1.2 `timelineSemaphore` and `hostQueryReset` were
enabled at logical-device creation. Existing aggregate initializers must add
those three fields. The engine has offscreen goldens, including synthetic
FFmpeg Dolby Vision metadata, a bounded three-frame resource ring, Android
background/foreground surface recreation coverage, and numeric P010/BT.2020
PQ/HLG checks for mastering-display, MaxCLL, default luminance, SDR tone
mapping, native 10-bit HDR10/PQ plus HDR10/HLG encoding, HLG-to-PQ conversion,
and FP16 extended-linear output above reference white. The Android
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
MediaCodec/Vulkan interop now passes H.264, HEVC, and a real profile 8.4 Dolby
Vision stream through private-AImageReader import with native
YCbCr/external-format sampling, one returned release fence per imported DOVI
frame, bounded pending images, and zero decoded-source
map/transfer/staging/upload counters. The DOVI phase retains parsed RPU
metadata on all 100 output frames and renders 97 through libplacebo. OHOS
direct-surface lifecycle coverage is complete. Its strict Vulkan native-buffer
target is implemented and the fail-closed opaque-format path is connected-
device validated; direct multi-plane sampling remains pending under the
responsibility and lifecycle boundaries in [`MOBILE.md`](MOBILE.md).

The current audio callback exposes the decoder's native sample format and
reference-counted planes. A platform audio sink should convert/resample only
when its device format requires it.

### Dolby formats

The core is codec-agnostic and uses the decoder registered by FFmpeg. AC-3,
E-AC-3, and TrueHD software decoding have been exercised through the audio
frame callback. This is PCM decode support only; IEC 61937/HDMI passthrough,
and Atmos object rendering remain separate backend/product work. On the
libplacebo GPU video paths, FFmpeg-parsed `AV_FRAME_DATA_DOVI_METADATA` is
passed to libplacebo only for residual-disabled base-layer Dolby Vision
reshaping and target-aware tone mapping;
the HEVC MediaCodec wrapper obtains that metadata through the same in-tree
FFmpeg RPU parser. libdovi is not required or enabled. Enhancement-layer
residual reconstruction, licensing, and certification remain outside this
implementation.

## Threading rules

- control methods are thread-safe;
- demux, asynchronous control, and state/status callbacks normally run on the
  playback worker; selected audio/video packets cross bounded queues to
  independent decode workers so one codec or output path cannot starve the
  other stream;
- `setVideoFrameScheduler()` runs on the video-decode worker before ordinary video
  delivery; an accepted frame is not sent to `onVideoFrame()` or renderers;
- decoded audio/video frame callbacks and `setRenderCallback()` run on the
  presentation worker; the bounded video queue drops obsolete late frames
  when application presentation falls behind;
- media events normally run on the playback, decode, or audio-output worker;
  forwarded audio-sink events run on the backend's event thread;
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
- `renderVideoDetailed()` runs synchronously on its caller and should be called
  from the thread that owns the native graphics context. Atomically published
  immutable frame/binding snapshots keep this hot path off the Player control
  mutex. `VideoRenderAPI::renderDetailed()` classifies a backend attempt as
  presented, deferred until a backend redraw, timer-backoff retry, terminally
  discarded, surface-lost, or fatal. `VideoRenderResult` maps those outcomes
  to Player-facing statuses and includes frame sequence, presentation
  generation, optional retry delay, and detail. A generation change during the
  backend call returns a terminal `FrameDiscarded` completion;
- compatibility `renderVideo()` returns a negative value for every non-rendered
  result and therefore cannot distinguish a missing frame from retryable
  contention. New native render loops retain the exact frame for
  `RendererDeferred`, retry `RendererBusy` after bounded backoff, recreate the
  platform surface for `SurfaceLost`, and do not retry `FrameDiscarded`;
- `OpenGLPresentCallback` runs on that same graphics-owner thread after
  framebuffer submission and before hardware-source release; it is intended
  for the adapter's bounded window-present call, not general application work;
- D3D11 renderer, decoder, interop, and application calls sharing one
  immediate context must serialize through the same
  `D3D11DeviceAccess::contextGuard()` or equivalent external locking; the
  renderer uses non-blocking `tryContextGuard()` and declines a frame on
  contention. A private output worker may pair `reserveContext()` with
  `tryContextGuardFor()` for a bounded, reservation-aware handoff without
  blocking an application UI thread;
- `VideoRenderAPI::renderDetailed()` runs synchronously inside Player's
  `renderVideoDetailed()` (and therefore the compatibility `renderVideo()`
  wrapper). Its default implementation calls the legacy boolean `render()`;
  backend event callbacks may request another player state;
- audio-sink and video-render backend event callbacks run on the thread chosen
  by the backend and may request another player state;
- compressed audio packets cross a bounded queue to their decode worker, then
  decoded audio crosses a second bounded queue to the dedicated audio-output worker;
  ordinary conversion, sink writes, and primary device-clock sampling run
  there without the player mutex held and cannot be blocked by application
  rendering; when a playing seek awaits the first valid device timestamp, the
  same audio-output worker polls it briefly while presentation uses only the
  cached sample;
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

1. Keep the resolved Android OpenGL ZeroCopy long-form/reopen regression in
   the connected-device gate while preserving the zero-CPU-copy and libplacebo
   color paths.
2. Keep the CPU swscale renderer and image-buffer target as the reference
   software path.
3. Keep the interleaved PCM libswresample backend as the reference when a sink
   negotiates a different format.
4. Use the completed Windows D3D11VA device/frame and zero-CPU-copy interop
   contracts as the native desktop reference.
5. Use the completed Android renderer, audio, MediaCodec, and interop paths as
   the mobile reference.
6. Continue the OHOS native-buffer milestone on hardware that exposes an
   explicit sampled multi-plane Vulkan format, then broaden OpenGL ES/HDR
   coverage using the same shared mobile contracts.
7. Add subtitle and multi-track switching.
8. Add live-stream buffering and recovery policies.

Each backend should remain optional so the core library never acquires a GUI
toolkit dependency.
