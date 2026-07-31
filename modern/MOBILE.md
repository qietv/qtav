# Android and OHOS mobile design

This document fixes the shared design boundary for the Android and OHOS
production paths before either platform hardware decoder is added. The two
platforms reuse portable rendering and test logic, but they do not share a
native application, window-system, audio, or codec ABI.

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
- `qtav_audio_aaudio` and a future OHOS OHAudio target are separate audio
  backends;
- `qtav_hw_mediacodec` and a future OHCodec target are separate hardware
  decoder backends;
- Vulkan and OpenGL ES native-buffer import are separate, optional interop
  targets; texture import remains separate from decoding and final rendering.

No Android or OHOS SDK declaration may enter an installed core header.
Backend-specific public headers may expose strong native types when needed,
but core continues to see only `VideoRenderAPI`, `AudioSink`,
`HardwareDecodeConfig`, and `HardwareFrame`.

## Shared Vulkan renderer engine

The Vulkan engine consumes software `VideoFrame` values and a current render
target supplied by a platform adapter. It is responsible for:

- packed RGB and planar or bi-planar YUV staging;
- YUV420/YUV422/YUV444, NV12/NV21, and little-endian P010 sampling;
- limited/full range normalization and BT.601, BT.709, or BT.2020 matrices;
- PQ/HLG/SDR transfer handling and source-primary conversion using the
  structured frame metadata;
- viewport, Fit/Fill/Stretch aspect modes, and all right-angle rotations;
- pipeline, descriptor, sampler, shader, geometry, and staging-buffer
  lifetime;
- a bounded ring of in-flight frame resources, with one retained
  `VideoFrame` per submitted slot until its completion fence is signalled;
- explicit image layout transitions and queue submission synchronization.

The engine borrows the selected Vulkan physical device, logical device, queue,
and queue-family identity through a Vulkan-backend-specific header. It does
not create a window surface or swapchain. The current-target contract supplies
the image/view, format, output `VkColorSpaceKHR`, extent, acquire semaphore,
completion fence, and presentation semaphore for one render. A target
generation changes whenever a surface or swapchain is recreated; stale
generation resources are retired only after their fences complete.

The implemented engine lives in `backends/render/vulkan/`. It supports every
planned software pixel family through a storage-buffer shader, applies the
structured SDR/HDR color inputs plus viewport/aspect/rotation geometry, and
uses a bounded three-frame resource ring with one retained source frame per
submission fence. Deterministic offscreen readback checks cover ring reuse,
YUV output, limited/full range, BT.601/BT.709 conversion, P010/BT.2020 PQ and
HLG input, HDR mastering-display/MaxCLL/default-luminance selection, viewport,
rotation, and target-generation replacement. Deterministic targets cover SDR
BGRA8 HDR-to-SDR compression, 10-bit HDR10/PQ output, 10-bit HDR10/HLG output,
HLG-to-PQ conversion, and FP16 extended-sRGB-linear plus BT.2020-linear values
above reference white. Output encoding is explicit rather than being inferred
from the image format.

Shader input structures, color conversion constants, geometry generation,
staging layout, capability decisions, and golden pixel vectors are shared
between Android, OHOS, and later Linux coverage. Deterministic engine tests use
offscreen images and do not require a window system.

## OpenGL ES fallback renderer

Android and OHOS use Vulkan as the preferred software-frame renderer and
OpenGL ES through EGL as the required fallback. The OpenGL ES engine implements
the same `VideoRenderAPI` behavior for supported software RGB, planar YUV,
NV12/NV21, and P010 inputs. It reuses color-conversion constants, shader input
definitions, geometry, capability rules, and golden vectors where their
semantics match Vulkan, while keeping shaders, upload resources,
synchronization, and lifetime rules native to OpenGL ES.

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
RGB/BGR/RGBA/BGRA/ARGB, and Gray8 software frames, applies the same structured
range, matrix, transfer, primaries, viewport, aspect, and rotation semantics as
the Vulkan path, and renders to a caller-supplied current framebuffer whose
target contract explicitly selects SDR sRGB, BT.2020/PQ, or BT.2020/HLG.
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
Android adapter and uses the same bounded recreation path; fatal OpenGL ES or
failed recovery enters the explicit no-renderer state. Selection notifications
record selected, recovered, fallback, and unavailable transitions with their
reasons.

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
4. The scheduler bounds outstanding undecided outputs. When the bound is
   reached it applies the documented late-frame drop policy instead of
   blocking an unbounded decoder queue.
5. Seek, loop, stop, and media replacement invalidate the scheduling
   generation, drop pending outputs, flush the native codec, and reject late
   callbacks from an older generation.
6. Surface loss stops release-for-presentation immediately. Recreating the
   surface either rebinds the codec when supported or reopens it explicitly;
   the application remains responsible for the native surface lifetime.
7. A copied generic hardware frame retains only resources whose native API
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
completes the prerequisite for later `SurfaceTexture` and
`AImageReader`/`AHardwareBuffer` work, but it does not claim a
shader-readable or texture-interoperable frame. Decoder fallback and
renderer/interop fallback remain independent.

## Zero-CPU-copy texture interop

Direct-surface presentation is the first hardware-output milestone because it
can avoid CPU access without requiring a shader-readable decoder frame. It
does not prove that a decoded image can participate in QtAVCore color,
geometry, composition, or post-processing passes. Texture interop is a later,
separate milestone for both Vulkan and OpenGL ES.

For this project, a mobile path is described as **zero-CPU-copy** only when no
decoded pixel is mapped to CPU memory, transferred to a software
`VideoFrame`, copied into a CPU staging buffer, or uploaded again by the
renderer. Retaining or passing an opaque native-buffer handle, importing it
into a graphics API, waiting on its producer fence, sampling it in a GPU
conversion pass, and presenting it are allowed. This definition does not claim
that a codec, driver, or system compositor performs no internal hardware copy.
Tests and status output use the precise `zero-CPU-copy` or `zero-CPU-map`
wording rather than an unqualified end-to-end zero-copy claim.

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

The confirmed Android Vulkan design uses an application-owned `AImageReader`
created for private, GPU-sampled images. Its `ANativeWindow` is supplied to
MediaCodec, and a decoded presentation token is released into that producer.
The consumer acquires an `AImage` and acquire fence asynchronously, obtains
the retained `AHardwareBuffer`, imports supported memory with
`VK_ANDROID_external_memory_android_hardware_buffer`, and uses native
YCbCr/external-format sampling where required. After the last GPU consumer,
the release fence is returned through asynchronous image deletion. The
adapter correlates the codec presentation timestamp with the acquired image
timestamp and never calls `AHardwareBuffer_lock*()`.

The confirmed Android OpenGL ES design uses a MediaCodec `Surface` backed by
`SurfaceTexture` as its primary path. `updateTexImage()` exposes the current
decoded image through `GL_TEXTURE_EXTERNAL_OES`; timestamp/generation
correlation and the single-current-image lifetime are explicit parts of the
adapter. A private `AImageReader` plus `AHardwareBuffer`/`EGLImage` import is
an optional alternative when all required EGL, GL, format, and fence
capabilities are present. Neither route reads decoded pixels through CPU
memory. P010, HDR, and formats that cannot be sampled with the required color
control remain capability-gated rather than silently converted on the CPU.

The confirmed OHOS OpenGL ES design supplies the `OHNativeWindow` produced by
`OH_NativeImage` to OHCodec surface output, updates the surface image, and
samples its bound `GL_TEXTURE_EXTERNAL_OES` texture. The adapter retains the
corresponding codec presentation token and native-image generation until the
consumer/release rules of the selected SDK permit reuse. This is a separate
OHOS implementation; it does not reuse Android handles or ABI assumptions.

OHOS Vulkan is conditionally feasible, not yet a direct consequence of the
current FFmpeg 8 OHCodec wrapper. OHOS exposes native-buffer Vulkan external
memory, but the wrapper's buffer-output branch currently obtains a CPU address
with `OH_AVBuffer_GetAddr()` and copies with `av_image_copy2()`. Before Vulkan
interop can be called zero-CPU-copy, a backend or narrowly scoped FFmpeg bridge
must instead expose and retain the decoded `OH_AVBuffer`/`OH_NativeBuffer`
until GPU completion, import it through the target SDK's OHOS Vulkan external
memory path, and then call `OH_VideoDecoder_FreeOutputBuffer()` without either
CPU operation. The current surface-output wrapper provides present/drop
tokens suitable for direct presentation and the `OH_NativeImage` GLES path,
but it does not expose a Vulkan-importable native buffer. The exact bridge,
format, protected-content, lifetime, and fence APIs remain target-SDK/device
gates.

When the active renderer changes from Vulkan to OpenGL ES, the platform layer
first attempts the OpenGL ES interop path for newly decoded native buffers. If
that path is unavailable, it follows the caller's explicit policy: reconfigure
for direct-surface presentation, reopen video in software, or report video
presentation unavailable. It never maps and uploads a hardware frame merely
because the graphics API changed. The reverse transition is not attempted
during the same renderer session.

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

## Connected-device validation

Shared generated media and lifecycle scenarios cover:

- software audio/video decode and end-of-media;
- Vulkan-preferred startup and forced OpenGL ES selection when Vulkan is
  unavailable, rejected by capability checks, or fails initial surface setup;
- recoverable Vulkan surface/swapchain recreation without an API switch;
- fatal Vulkan failure followed by one-way OpenGL ES fallback without media
  reopen, plus explicit failure when neither renderer is usable;
- MediaCodec H.264/HEVC direct-surface output with explicit present/drop,
  seek/flush, media replacement, stop, background/foreground surface
  recreation, stale-generation rejection, bounded retained outputs, and clean
  shutdown;
- capability-gated MediaCodec/OHCodec native-buffer import through Vulkan and
  OpenGL ES with zero CPU map/transfer calls, retained lifetime, fence
  ordering, format/color validation, and explicit unsupported-path results;
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
