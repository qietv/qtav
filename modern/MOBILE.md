# Android and OHOS mobile design

This document fixes the shared design boundary for the Android and OHOS
production paths before either platform hardware decoder is added. The two
platforms reuse portable rendering and test logic, but they do not share a
native application, window-system, audio, or codec ABI.

## Responsibility and target boundaries

The reusable pieces are compile-time C++ targets in this repository:

- `qtav_render_vulkan` is the platform-neutral Vulkan renderer engine;
- Android surface and swapchain integration belongs under
  `backends/render/vulkan/android/` and small Android lifecycle helpers belong
  under `platform/android/`;
- OHOS surface and swapchain integration belongs under
  `backends/render/vulkan/ohos/` and small OHOS lifecycle helpers belong under
  `platform/ohos/`;
- `qtav_audio_aaudio` and a future OHOS OHAudio target are separate audio
  backends;
- `qtav_hw_mediacodec` and a future OHCodec target are separate hardware
  decoder backends;
- optional texture import remains separate from decoding and final rendering.

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
the image/view, format, extent, acquire semaphore, completion fence, and
presentation semaphore for one render. A target generation changes whenever a
surface or swapchain is recreated; stale generation resources are retired only
after their fences complete.

Shader input structures, color conversion constants, geometry generation,
staging layout, capability decisions, and golden pixel vectors are shared
between Android, OHOS, and later Linux coverage. Deterministic engine tests use
offscreen images and do not require a window system.

## Platform surface adapters

Android and OHOS use separate adapters because their window ownership and
lifecycle callbacks are not ABI-compatible.

The Android application owns the `ANativeActivity` or Java/Kotlin activity,
permissions, and the current `ANativeWindow`. The Android adapter retains the
window only for the active surface generation, borrows the application-selected
Vulkan instance/device/queue, and owns its `VkSurfaceKHR`, swapchain, image
views, acquire/present synchronization, and recreation state. Window
replacement invalidates the old generation before a new one is published.

The OHOS application owns ArkUI/XComponent state and the current
`OHNativeWindow`. Its adapter follows the same renderer-target protocol but
uses an independent implementation and OHOS-specific lifecycle rules. Neither
adapter pretends that `ANativeWindow`, `OHNativeWindow`, EGL objects, or their
callbacks are interchangeable.

An OpenGL ES engine may reuse color constants, shader inputs, geometry, and
golden vectors. EGL context, surface, thread affinity, and loss/recreation
remain API- and platform-specific rather than hidden behind a false Vulkan/EGL
common lifecycle.

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

Direct-surface H.264 and HEVC presentation is implemented and validated before
SurfaceTexture, `AHardwareBuffer`, NativeBuffer, or Vulkan texture import is
claimed. Decoder fallback and renderer/interop fallback remain independent.

## Audio boundaries

Android uses AAudio and OHOS uses OHAudio. Both implement the existing
`AudioSink` contract and may reuse a portable bounded single-producer,
single-consumer PCM queue, timestamp arithmetic, and deterministic queue
tests. Native stream creation, callback signatures, route changes,
disconnects, latency queries, and restart behavior remain in separate
targets. No OpenSL ES fallback is added unless the selected Android minimum
API or device results demonstrate that it is required.

## Connected-device validation

Shared generated media and lifecycle scenarios cover:

- software audio/video decode and end-of-media;
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
