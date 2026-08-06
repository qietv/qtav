# Android and OHOS mobile design

This document records the shared design boundary for the Android and OHOS
production paths. The two platforms reuse portable rendering and test logic,
but they do not share a native application, window-system, audio, or codec ABI.

## Responsibility and target boundaries

The reusable pieces are compile-time C++ targets in this repository:

- `qtav_render_vulkan` is the platform-neutral Vulkan renderer engine;
- `qtav_render_opengl` is the platform-neutral OpenGL ES renderer
  engine and the required mobile fallback for software-frame presentation;
- Android surface and swapchain integration belongs under
  `backends/render/vulkan/android/` and small Android lifecycle helpers belong
  under `platform/android/`;
- Android EGL context and surface integration belongs under
  `backends/render/opengl/android/`;
- OHOS surface and swapchain integration belongs under
  `backends/render/vulkan/ohos/` and small OHOS lifecycle helpers belong under
  `platform/ohos/`;
- OHOS EGL context and surface integration belongs under
  `backends/render/opengl/ohos/`;
- `qtav_audio_aaudio` and `qtav_audio_ohaudio` are separate audio backends;
- `qtav_hw_mediacodec` and `qtav_hw_ohcodec` are separate hardware-decoder
  backends;
- Vulkan and OpenGL ES native-buffer import are separate, optional interop
  targets; texture import remains separate from decoding and final rendering.

Every platform interop target is limited to native import, format/plane
exposure, timestamp/generation correlation, producer/release synchronization,
and source lifetime through GPU completion. It does not own semantic color,
Dolby Vision, tone/gamut mapping, scaling, or output encoding.

No Android or OHOS SDK declaration may enter an installed core header.
Backend-specific public headers may expose strong native types when needed,
but core continues to see only `VideoRenderAPI`, `AudioSink`,
`HardwareDecodeConfig`, and `HardwareFrame`.

## Shared libplacebo Vulkan renderer engine

The Vulkan engine consumes software `VideoFrame` values or retained native
images and a current render target supplied by a platform adapter. libplacebo
is the sole semantic authority for pixel-format interpretation, range and
matrix conversion, transfer functions, primaries conversion, Dolby Vision
reshaping, tone mapping, gamut mapping, scaling, and SDR/HDR output encoding.
The QtAVCore Vulkan layer is responsible only for:

- mapping supported FFmpeg software storage or wrapping an imported native
  image as a libplacebo source;
- viewport, Fit/Fill/Stretch aspect modes, and right-angle rotations expressed
  through libplacebo geometry;
- target image, format, output-color contract, and generation tracking;
- a bounded ring of in-flight resources, with one retained `VideoFrame` per
  submitted slot until its completion fence is signalled;
- explicit image layout transitions and queue submission synchronization.

The engine borrows the selected Vulkan physical device, logical device, queue,
and queue-family identity through a Vulkan-backend-specific header. It does
not create a window surface or swapchain. The current-target contract supplies
the image/view, format, output `VkColorSpaceKHR`, extent, acquire semaphore,
completion fence, and presentation semaphore for one render. A target
generation changes whenever a surface or swapchain is recreated; stale
generation resources are retired only after their fences complete.

The implemented engine lives in `backends/render/vulkan/`. It maps the planned
software pixel families through the shared FFmpeg/libplacebo bridge and uses a
bounded three-frame resource ring with one retained source frame per submission
fence. Deterministic offscreen readback covers source formats, structured
SDR/HDR metadata, geometry, target generation, SDR tone mapping, native PQ/HLG,
and extended-linear output. Output encoding is explicit rather than inferred
from the image format. Backend-local shaders are allowed only when an opaque
native image must be normalized into a raw component representation before
libplacebo; those shaders may crop or repack channels, but may not implement a
matrix, transfer, gamut, tone-map, Dolby Vision, scaling, or output operation.
Portable source/target contracts and deterministic vectors are shared between
Android and OHOS; native import and presentation remain platform-specific.

Dolby Vision in this design means FFmpeg-parsed metadata for the
residual-disabled base-layer case, applied by libplacebo after raw source
components have been preserved. Enhancement-layer reconstruction, Dolby
licensing, and certification are not claimed.

## OpenGL ES fallback renderer

Android and OHOS use Vulkan as the preferred software-frame renderer and
OpenGL ES through EGL as the required fallback. The OpenGL ES engine implements
the same `VideoRenderAPI` behavior for supported software RGB, planar YUV,
NV12/NV21, and P010 inputs. It maps frames and structured metadata through the
same FFmpeg/libplacebo bridge. libplacebo generates the OpenGL shaders and owns
the same semantic color, geometry, tone/gamut, Dolby Vision, and output
decisions as Vulkan; QtAVCore does not maintain parallel conversion constants
or a second semantic shader pipeline.

`qtav_render_opengl` contains reusable OpenGL ES rendering logic.
Android and OHOS have separate EGL/window adapters because `ANativeWindow`,
`OHNativeWindow`, XComponent, context ownership, surface replacement, and
thread-affinity rules are not ABI-compatible. Each adapter owns its EGL
display, context, surface, and recreation state while retaining or borrowing
the current native window according to its platform contract. Neither adapter
enters the core target or exposes its SDK types through core public headers.

The baseline is OpenGL ES 3.x with runtime capability checks. Formats or output
features that require unavailable texture formats, precision, extensions, or
HDR surface support are reported explicitly and use a documented lower-quality
conversion where one exists; they are not silently claimed as native support.
SDL3 is not part of the renderer fallback contract. An application may use
SDL3 in its own shell, but QtAVCore does not require SDL to select, create, or
recover either mobile renderer.

The implemented baseline lives under `backends/render/opengl/`.
`QtAV::RenderOpenGL` uploads YUV420/422/444, NV12/NV21, little-endian P010,
RGB/BGR/RGBA/BGRA/ARGB, and Gray8 software frames, gives their structured
metadata and geometry to libplacebo, and renders to a caller-supplied current
framebuffer whose target contract explicitly selects SDR sRGB, BT.2020/PQ, or
BT.2020/HLG.
P010/PQ/HLG input is deterministically tone-mapped only for the SDR target;
native HDR targets preserve luminance, convert primaries to BT.2020, and encode
PQ or HLG as selected. `QtAV::RenderOpenGLAndroid` separately owns its EGL
display, OpenGL ES 3.x context, window surface, and generation while retaining
the active `ANativeWindow`. It tries exact RGB10_A2 with
`EGL_EXT_gl_colorspace_bt2020_pq`, then the HLG extension, and finally explicit
RGBA8/sRGB according to the caller's prefer/require/SDR-only policy. It also
sets and verifies the corresponding Android buffer dataspace. Android device
checks cover all advertised upload families, viewport, rotation,
target-generation replacement, P010/PQ-to-SDR readback, PQ/HLG output numeric
encoding, real RGB10_A2/PQ presentation, and compositor HDR-layer recognition.
`QtAV::RenderOpenGLOHOS` separately retains the XComponent
`OHNativeWindow`, owns its EGL display, OpenGL ES 3.x context, surface, swap,
and generation, and verifies exact RGBA8 plus sRGB native-window/EGL state.
The first OHOS slice exposes `SdrOnly` and SDR fallback for `PreferHdr`, while
`RequireHdr` fails explicitly until an OHOS HDR format, EGL colorspace, and
compositor path passes the device capability gate. The connected OHOS harness
proves initial GLES selection, Vulkan and GLES resize recreation, and fatal
one-way Vulkan-to-GLES fallback on one media open.
The application/platform selector is implemented separately as
`QtAV::RenderMobile`.

## Renderer selection and fallback

Renderer selection belongs to the application or thin platform integration
layer because that layer owns the activity, native window, and graphics
devices. Core does not silently create a second graphics API. The shared policy
for Android and OHOS is:

1. Prefer Vulkan for a new renderer session. Before publishing a Vulkan
   renderer, verify the loader/API version, required instance and device
   extensions, queue and presentation support, surface formats, and swapchain
   creation.
2. Select OpenGL ES when Vulkan is unavailable, required capabilities are
   missing, device selection fails, or the first surface/swapchain generation
   cannot be created. Record the selected API and fallback reason in device
   facts and emit an observable renderer detail event.
3. Treat window replacement, an out-of-date or suboptimal swapchain, and
   ordinary surface loss as recoverable Vulkan lifecycle events. Suspend
   presentation, invalidate the old target generation, wait for retained
   in-flight resources as required, and attempt bounded recreation before
   changing APIs.
4. Treat device loss, unrecoverable queue submission or presentation failure,
   and repeated recreation failure for a valid native-window generation as
   fatal to the Vulkan renderer. Quiesce and destroy that renderer, invalidate
   its generation, emit the failure reason, and create the OpenGL ES renderer
   against the current window without reopening the media.
5. Do not automatically switch back to Vulkan during the same renderer
   session. A new application-led renderer session may probe Vulkan again.
   This prevents backend oscillation after a driver or device failure.
6. If EGL/OpenGL ES context or surface loss is recoverable, recreate that
   backend first. If OpenGL ES initialization or bounded recovery also fails,
   report video presentation as unavailable; playback, audio output, and
   decoded-frame callbacks remain usable.

The implemented platform-neutral policy lives in
`backends/render/mobile/`. `MobileVideoRendererSelector` owns neither graphics
API nor a native window; application/platform factories return a prepared
`VideoRenderAPI` for the current window generation or an explicit unavailable
reason. `open()` starts a new session and probes Vulkan first. `SurfaceLost`
performs the configured bounded number of complete same-API recreations,
whereas `Error` is fatal. Fatal or repeatedly unrecoverable Vulkan is retired
for the session before OpenGL ES is created and the retained frame is retried.
OpenGL ES context/display/surface loss is classified as recoverable by the
Android and OHOS adapters and uses the same bounded recreation path; fatal
OpenGL ES or failed recovery enters the explicit no-renderer state. Selection
notifications record selected, recovered, fallback, and unavailable
transitions with their reasons.

The portable `VideoRenderAttemptResult` is the synchronous decision boundary
used by both mobile APIs and their native adapters. `Presented` completes the
frame, `DeferredUntilRedraw` retains that exact frame until an asynchronous
producer/GPU callback raises `RedrawRequested`, `RetryAfterBackoff` asks the
application for a bounded timer retry, and `Discarded` terminally consumes a
stale or retired-generation frame. `SurfaceLost` starts bounded same-API
recreation; `FatalError` starts the one-way Vulkan-to-OpenGL ES policy. These
outcomes no longer depend on guessing from a boolean return plus a synchronous
event side channel. Legacy boolean renderers remain supported through the
default retry-after-backoff mapping.

The platform calls `suspendSurface()` before releasing a native-window
generation, updates the application state captured by the factories, and calls
`recreateSurface()` after publishing the replacement. The selector object
remains attached to `Player`, so same-API recreation and Vulkan-to-OpenGL ES
fallback do not reopen media. Deterministic mock-adapter tests cover
Vulkan-unavailable startup, initial Vulkan open failure, recoverable Vulkan and
OpenGL ES recreation, fatal Vulkan fallback, bounded-recovery exhaustion,
window replacement, one-way behavior, and both-backends-unavailable. The
Android NativeActivity harness uses the same selector for real Vulkan HDR
playback/surface recreation and a forced-startup fallback check through the
real EGL adapter.

Fallback transfers no live Vulkan or EGL resource between APIs. Copied
`VideoFrame` values retain software-frame data across the transition, while
old renderer generations reject late redraw or completion work. Renderer
selection, hardware-decoder selection, direct-surface presentation, and
hardware-frame interop are independent policies: a Vulkan failure does not by
itself reopen the decoder, and an interop failure must not be disguised as a
graphics-API fallback.

Hardware frames are the deliberate exception to retained-frame retry. A frame
decoded for the retired Vulkan producer surface is never submitted to the new
OpenGL ES renderer. `MobileVideoRendererSelector` invokes its synchronous
hardware-frame fallback callback after preparing the OpenGL ES candidate. The
application rebinds subsequent decoder output and returns one explicit route:
compatible OpenGL ES native interop, direct-surface presentation, software
decode, or no video. Late frames from the retired native surface are discarded
without mapping. OpenGL ES interop requires the prepared candidate to
advertise the source hardware device; no callback or a `None` decision makes
presentation explicitly unavailable.

An asynchronous interop attempt returns `DeferredUntilRedraw` before its
producer image is ready. The selector preserves the active API and exact frame;
only `SurfaceLost` or `FatalError` starts recovery or fallback.

## Platform surface adapters

Android and OHOS use separate adapters because their window ownership and
lifecycle callbacks are not ABI-compatible.

The Android application owns the `ANativeActivity` or Java/Kotlin activity,
permissions, and the current `ANativeWindow`. The Android adapter retains the
window only for the active surface generation, borrows the application-selected
Vulkan instance/device/queue, and owns its `VkSurfaceKHR`, swapchain, image
views, acquire/present synchronization, and recreation state. Window
replacement invalidates the old generation before a new one is published.

The Android adapter is implemented as
`QtAV::RenderVulkanAndroid`. It retains the active `ANativeWindow` and owns its
surface, swapchain, image views, and per-frame acquire/present semaphores while
borrowing the application-created instance/device/queue. The NativeActivity
harness enables `VK_EXT_swapchain_colorspace`, requires a supported native HDR
pair, prefers HDR10/PQ, optionally enables `VK_EXT_hdr_metadata`, and has
presented 180 decoded YUV420P frames on the recorded Adreno 830 device. It is
recognized as an active HDR layer by the Android compositor, presents a
synthetic P010/BT.2020/PQ frame with mastering/MaxCLL metadata, and rebuilds
the HDR surface/swapchain after a background/foreground window replacement
without reopening the media. Production callers can instead prefer HDR with
SDR fallback or require SDR explicitly; selected format/color space and
HDR-active state remain observable.

`QtAV::RenderOpenGLAndroid` follows the same output-policy meanings without
sharing Vulkan objects or lifecycle code. The recorded Android 16/Adreno 830
device exposes an exact RGB10_A2 BT.2020/PQ EGL surface; the adapter presents a
synthetic P010/BT.2020/PQ frame through it, and Android reports the layer as
HDR. The same run first forces `SdrOnly` to prove the RGBA8/sRGB tone-mapping
fallback remains explicit. HLG is selected only when the EGL display exposes
its colorspace extension and PQ cannot be established.

The OHOS application owns ArkUI/XComponent state and the current
`OHNativeWindow`. Its adapter follows the same renderer-target protocol but
uses an independent implementation and OHOS-specific lifecycle rules. Neither
adapter pretends that `ANativeWindow`, `OHNativeWindow`, EGL objects, or their
callbacks are interchangeable.

The OpenGL ES engine reuses only the portable rendering inputs described
above. EGL context, surface, thread affinity, and loss/recreation remain API-
and platform-specific rather than hidden behind a false Vulkan/EGL common
lifecycle.

## Surface-backed hardware decode

MediaCodec and FFmpeg 8 OHCodec use one toolkit-independent presentation
state machine while keeping their native buffers and surfaces in separate
backends:

1. The application supplies a versioned surface token before decoder open.
2. Decoder selection is explicit; failure follows the caller's independent
   software-decode fallback policy.
3. A decoded output carries its media timestamp, surface generation, and a
   single pending decision: present at a requested monotonic time or drop.
4. Releasing an undecided token, or the last retained FFmpeg frame reference
   when no token decision was made, is abandonment. It unconditionally
   drops/frees the codec output and never infers presentation from native
   buffer attributes.
5. The scheduler bounds outstanding undecided outputs. When the bound is
   reached it applies the documented late-frame drop policy instead of
   blocking an unbounded decoder queue.
6. Seek, loop, stop, and media replacement invalidate the scheduling
   generation, drop pending outputs, flush the native codec, and reject late
   callbacks from an older generation.
7. Surface loss stops release-for-presentation immediately. Recreating the
   surface either rebinds the codec when supported or reopens it explicitly;
   the application remains responsible for the native surface lifetime.
8. A copied generic hardware frame retains only resources whose native API
   permits post-callback retention. Direct-surface output that cannot be
   retained is represented as a presentation token, not a fake texture
   handle.

The Android direct-surface checkpoint is implemented in
`QtAV::HWMediaCodec`. `MediaCodecSurface` retains and versions the
application-supplied `ANativeWindow`; the backend creates FFmpeg's MediaCodec
hardware device for that surface and explicitly selects the
`*_mediacodec` wrapper decoder. Each hardware `VideoFrame` produces a
move-only `MediaCodecFrame` with immediate present, monotonic-time present,
and drop decisions. The core keeps the decoder context alive until every
copied output is released, so invalidating playback queues cannot leave an
FFmpeg output buffer referring to a destroyed codec.

The Android 16 device checkpoint passes H.264 and HEVC with bounded output,
both present and drop, seek/flush, media replacement, explicit stop,
background/foreground surface loss and reopen, stale-generation rejection,
and clean shutdown. No decoded pixel is mapped in this direct path. This
completes the prerequisite for the implemented private-`AImageReader`
OpenGL ES and Vulkan paths described below. Those separate interop checkpoints
provide shader-readable retained `AImageReader`/`AHardwareBuffer` frames; the
direct-surface path itself still makes no texture-interoperability claim.
Decoder fallback and renderer/interop fallback remain independent.

The OHOS direct-surface checkpoint is implemented in `QtAV::HWOHCodec` using
FFmpeg's explicit OHCodec wrapper and opaque single-decision release token. The
2026-08-05 signed HAP passed on a Mate 60 Pro (`ALN-AL80`), HarmonyOS
6.1.0.135 / OpenHarmony 6.1.1.120 API 24. H.264 presented 48 outputs and
dropped 5; HEVC presented 40 and dropped 5. One pause/resume, one 2000 ms
target/callback seek, media replacement, explicit stop, background/foreground,
surface recreation, and stale-generation rejection all passed. The run reached
`maxPending=2`, observed `pendingAtStop=1`, drained to `pendingEnd=0`, and kept
`maxQueued=0`. This completes the OHCodec direct-surface lifecycle prerequisite
without making a texture-interoperability claim.

## Zero-CPU-copy texture interop

Direct-surface presentation is the first hardware-output milestone because it
can avoid CPU access without requiring a shader-readable decoder frame. It
does not prove that a decoded image can participate in QtAVCore color,
geometry, composition, or post-processing passes. Texture interop is a
separate milestone for each graphics API; the Android Vulkan and OpenGL ES
milestones are implemented below.

For this project, a mobile path is described as **zero-CPU-copy** only when no
decoded pixel is mapped to CPU memory, transferred to a software
`VideoFrame`, copied into a CPU staging buffer, or uploaded again by the
renderer. Retaining or passing an opaque native-buffer handle, importing it
into a graphics API, waiting on its producer fence, sampling it in a GPU
conversion pass, and presenting it are allowed. This definition does not claim
that a codec, driver, or system compositor performs no internal hardware copy.
Tests and status output use the precise `zero-CPU-copy` or `zero-CPU-map`
wording rather than an unqualified end-to-end zero-copy claim.

**Strict no-intermediate source zero-copy** is narrower. It requires an
explicit native graphics format and plane mapping so the exact retained
decoder allocation can be wrapped directly as libplacebo's source. A
pre-libplacebo normalization draw or intermediate source texture disqualifies
the strict claim even when all decoded-source CPU counters remain zero.

The shared interop contract requires:

- a reference-counted hardware frame or presentation object that retains the
  native buffer until all GPU consumers complete;
- source device, surface generation, dimensions, native format, range, color
  space, HDR metadata, and protected-content state validation before import;
- explicit producer-to-renderer and renderer-to-release synchronization using
  the platform's supported fence or semaphore mechanism. GPU-native waiting is
  the normal performance target; a CPU fence wait is a latency defect but does
  not by itself violate the zero-CPU-copy definition because it does not map or
  copy decoded pixels;
- capability reporting per graphics API and per format, including NV12-like
  8-bit output and P010/10-bit output where the device exposes them;
- a same-device or explicitly shareable-device rule, with foreign or stale
  resources rejected before graphics-context access;
- bounded imported-frame caching keyed by native-buffer identity and
  generation, with deterministic retirement after GPU completion;
- no implicit CPU mapping. An optional mapping/copy path, if added for
  diagnostics, is disabled by default, emits an observable fallback event,
  and remains independent of hardware-decoder and graphics-API fallback.

The Android Vulkan design is implemented by the independent
`QtAV::InteropMediaCodecVulkan` target. Its application-owned interop object
creates an `AImageReader` for private, GPU-sampled images and supplies the
reader's versioned `ANativeWindow` to MediaCodec. A decoded presentation token
is released into that producer; the consumer acquires an `AImage` and
optional acquire fence asynchronously, obtains the retained `AHardwareBuffer`,
and imports supported memory through
`VK_ANDROID_external_memory_android_hardware_buffer`. Driver-reported native
YCbCr/external-format conversion is attached to an immutable sampler, foreign
queue-family ownership is transferred around sampling, and codec-aligned
allocations use the `AImage` crop rectangle rather than assuming the buffer
and visible dimensions are equal. After submission, an exportable semaphore
provides the release sync fd returned through asynchronous image deletion.
The adapter correlates codec and acquired-image timestamps, bounds outstanding
images, and never calls `AHardwareBuffer_lock*()`.

If Vulkan reports an explicit `VkFormat` and plane mapping, libplacebo can wrap
the imported decoder allocation directly and the path can qualify as strict
source zero-copy. An opaque external format may instead require a GPU-only raw
representation normalization texture before libplacebo. That remains
zero-CPU-copy, but is not strict source zero-copy. The normalization shader may
only crop or preserve/repack raw components; semantic color conversion, Dolby
Vision, tone/gamut mapping, scaling, and output encoding remain libplacebo's
exclusive responsibility.

`QtAV::RenderVulkan` exposes the decoder-independent retained sampled-image
contract used by this target; it keeps the imported image, hardware buffer,
view, conversion sampler, and synchronization resources alive until the
submission fence completes. `QtAV::RenderVulkanAndroid` polls image readiness
before acquiring a swapchain image, so an asynchronous producer cannot strand
the presentation ring. The Android 16/Adreno 830 checkpoint renders both
H.264 and HEVC, returns one release fence for every imported image, keeps the
pending-image high-water mark within the configured reader bound, and reports
zero decoded-source map, software-transfer, staging-copy, and renderer-upload
calls.

The implemented Android OpenGL ES path uses a private GPU-sampled
`AImageReader`, retains each `AHardwareBuffer`, imports it as an EGLImage, and
samples raw Y/Cb/Cr through `GL_EXT_YUV_target`. A crop-aware GPU pass stores
those raw components in RGBA16F before libplacebo. It performs no matrix,
transfer, gamut, tone-map, Dolby Vision, scaling, or output operation. The
adapter correlates timestamps and generations, waits the acquire fence, retains
the exact source through presentation, and returns a release fence before
releasing the image. This route is zero-CPU-copy but, because it uses the
RGBA16F normalization texture, is not strict source zero-copy. Imports that
cannot prove raw component sampling are rejected for the semantic/Dolby Vision
path; implicit `SurfaceTexture` or external-OES YUV-to-RGB conversion is not a
substitute.

The preferred OHOS hardware-frame interop target is implemented by the
independent `QtAV::InteropOHCodecVulkan` target. Its application-owned interop
object creates a private `OH_ConsumerSurface` and supplies that surface's
producer window to `QtAV::HWOHCodec`. Surface-mode callback `OH_AVBuffer`
objects do not expose usable native memory, so the adapter presents exactly one
retained codec output into the private surface, acquires the corresponding
`OHNativeWindowBuffer`, retains its `OH_NativeBuffer`, and imports it through
`VK_OHOS_external_memory`. An acquire sync fd becomes a Vulkan semaphore. The
consumer buffer stays retained until the renderer GPU timeline destroys the
imported image and memory, then it is returned to the consumer surface.

The target accepts only explicit sampled two- or three-plane 4:2:0 8/10-bit
`VkFormat` values that libplacebo wraps directly. It therefore claims strict
source zero-copy only after that route succeeds. `VK_FORMAT_UNDEFINED` with an
opaque external-format ID is rejected; this target deliberately does not add a
raw GPU normalization texture that would weaken the claim to zero-CPU-copy.

The OHOS OpenGL ES fallback is a separate, lower-priority target. It must prove
raw `GL_EXT_YUV_target` sampling and normalize crop-aware Y/Cb/Cr into RGBA16F
before libplacebo. It is therefore zero-CPU-copy, not strict source zero-copy.
An implicit `OH_NativeImage` external-OES YUV-to-RGB path is no longer a target
because it hides the raw source representation and cannot support the required
libplacebo/Dolby Vision ordering. OHCodec/NativeImage may propagate the codec
PTS unchanged in microseconds, so the interop compares the observed value and
its microsecond-to-nanosecond candidate against the exact queued-frame PTS set,
then stores and correlates the selected value in nanoseconds. The FFmpeg
OHCodec wrapper independently parses each HEVC RPU before submission, keys it
by that same microsecond PTS, and attaches it only to the matching returned
output. The private consumer-surface bridge avoids FFmpeg's software-copying
OHCodec buffer-output branch and keeps the SDK types in the optional backend.
Exact format,
protected-content, lifetime, and fence support remain target-SDK/device gates.
On 2026-08-06 the connected Mate 60 Pro acquired one real H.264 and one real
HEVC buffer, both reported `NATIVEBUFFER_PIXEL_FMT_YCBCR_420_SP`, but Vulkan
returned only `VK_FORMAT_UNDEFINED` with external format `1000156003`. The HAP
therefore reported the expected strict `UNSUPPORTED` result with two opaque
rejections and zero map/transfer/staging/upload/normalization counters. No OHOS
texture-interop PASS is claimed until explicit-plane import, sampling, and
post-GPU release execute on suitable hardware.

On the same device, residual-disabled Profile 5 and Profile 8.4 each rendered
45 HEVC frames through the raw OpenGL ES path with 45 RPU frames queued,
timestamp-matched, and released. Both runs reported zero implicit-RGB images
and zero decoded-source map/transfer/staging/upload calls. Profile 8.4 also
validated MMR reshaping after the libplacebo overlay corrected generated GLES
integer array indexing and third-order branch syntax. This is a zero-CPU-copy
OpenGL result, not a strict source-zero-copy Vulkan result.

When the active renderer changes from Vulkan to OpenGL ES, the implemented
selector callback first permits the platform layer to reconfigure newly
decoded native buffers for the prepared OpenGL ES interop path. If that path
is unavailable, the callback selects direct-surface presentation, software
decode, or no video. Direct-surface and no-video routes retire the renderer;
software decode keeps OpenGL ES active for the later software frames. The
current Vulkan hardware frame is dropped, not retried, and none of these
routes maps or uploads it. The reverse transition is not attempted during the
same renderer session.

Seek, loop, stop, media replacement, decoder flush, surface replacement, and
renderer fallback advance a generation and reject late native buffers. Device
tests must prove zero calls to CPU mapping/transfer hooks, correct producer and
release fence ordering, bounded outstanding buffers, retained lifetime through
asynchronous GPU completion, and clean failure or explicit fallback for
unsupported formats and protected content. Direct-surface validation remains
a prerequisite; a capability-gated texture test that did not exercise a
supported native import is reported as skipped, not passed. A test may read
back the final render target to validate pixels, but decoded-source map,
transfer, staging-copy, and re-upload counters must remain zero.

## Audio boundaries

Android uses AAudio and OHOS uses OHAudio. Both implement the existing
`AudioSink` contract and may reuse a portable bounded single-producer,
single-consumer PCM queue, timestamp arithmetic, and deterministic queue
tests. Native stream creation, callback signatures, route changes,
disconnects, latency queries, and restart behavior remain in separate
targets. No OpenSL ES fallback is added unless the selected Android minimum
API or device results demonstrate that it is required.

The Android implementation now lives in `QtAV::AudioAAudio`. It negotiates
mono/stereo Float32 PCM and copies accepted buffers into a fixed-capacity SPSC
ring on the player's audio-output worker. The AAudio data callback performs no
allocation, lock, sleep, stream lifecycle operation, or application callback;
it only consumes that ring, fills bounded silence, and publishes atomic timing
state. `AAudioStream_getTimestamp(CLOCK_MONOTONIC)` supplies the media-timeline
device clock, while latency includes native pipeline frames and queued PCM.
A separate management worker observes transparent route-ID changes and handles
AAudio disconnect callbacks by closing and rebuilding the default-route stream
with the same negotiated format. The API 28 device baseline passes without an
OpenSL ES fallback.

The OHOS implementation now lives in `QtAV::AudioOHAudio`. It requests 48 kHz
mono/stereo Float32 PCM in fast mode with normal-mode construction fallback,
and uses the same portable allocation-free SPSC queue implementation without
sharing any OHAudio ABI with Android. Its native write callback has the same
bounded no-allocation/no-lock responsibilities, while
`OH_AudioRenderer_GetAudioTimestampInfo()` supplies hardware-committed frame
timing. Route changes, forced interruptions, errors, and stream reconstruction
run on an OHOS-only management worker. The API 24 Mate 60 Pro baseline proves
PCM delivery, clock/latency, pause/resume, seek/flush, and loop-boundary drain;
the recorded run did not induce a physical route change.

## Connected-device validation

Shared generated media and lifecycle scenarios cover:

- software audio/video decode and end-of-media;
- Vulkan-preferred startup and forced OpenGL ES selection when Vulkan is
  unavailable, rejected by capability checks, or fails initial surface setup;
- recoverable Vulkan surface/swapchain recreation without an API switch;
- fatal Vulkan failure followed by one-way OpenGL ES fallback without media
  reopen, plus explicit failure when neither renderer is usable;
- fatal Vulkan failure while MediaCodec is producing for a private
  AImageReader, followed by synchronous decoder rebind to the prepared
  OpenGL AImageReader producer and continued raw `GL_EXT_YUV_target` rendering
  without a media replacement or decoded-source CPU access;
- MediaCodec H.264/HEVC direct-surface output with explicit present/drop,
  seek/flush, media replacement, stop, background/foreground surface
  recreation, stale-generation rejection, bounded retained outputs, and clean
  shutdown;
- OHCodec H.264/HEVC direct-surface output with timed present/drop,
  pause/resume, 2000 ms seek, media replacement, stop, background/foreground,
  surface recreation, stale-generation rejection, bounded retained outputs,
  final-reference drop, and clean shutdown;
- OHCodec H.264/HEVC private-ConsumerSurface native-buffer acquisition,
  exact one-frame queueing, strict explicit-plane Vulkan gating, opaque-format
  rejection, and zero CPU map/transfer/staging/upload/normalization counters;
- MediaCodec H.264/HEVC private-AImageReader Vulkan import with timestamp
  correlation, native/external-format validation, aligned-allocation crop,
  bounded images, release-fence return, and zero decoded-source CPU
  map/transfer/staging/upload counters;
- capability-gated MediaCodec native-buffer import through Vulkan and OpenGL
  ES with zero CPU map/transfer calls, retained lifetime, fence ordering,
  format/color validation, and explicit unsupported-path results; the OHCodec
  Vulkan test applies the same rule and reports `UNSUPPORTED`, not PASS, when
  a real native buffer lacks an explicit multi-plane Vulkan format;
- pause/resume and monotonic position;
- seek and loop flush;
- media replacement and explicit stop;
- background/foreground transition;
- surface loss and recreation;
- retained-frame lifetime and bounded outstanding hardware output;
- audio route/disconnect handling when the backend exists.

Thin platform harnesses are responsible for packaging, signing, installation,
launch, logs, and machine-readable pass/fail markers. Android uses APK/ADB;
OHOS uses HAP and its selected device tooling. Device facts recorded with a
result include ABI, OS/API version, GPU/driver, graphics API version, and
audio/codec capabilities.

Recent Android and OHOS devices may require an on-device confirmation before a
test application can be installed or replaced. An installation/update failure
that may be waiting for this authorization is a hard pause: the harness reports
the failure once and asks the user to approve it manually. It must not retry,
bypass, or automate the device prompt.
