# QtAVCore architecture

This document describes the current structure and ownership boundaries of the
active Qt-free rewrite under `modern/`. Milestone status and task ordering live
in [`PLAN.md`](PLAN.md); durable trade-offs live in
[`DECISIONS.md`](DECISIONS.md); migration from legacy QtAV is documented in
[`MIGRATION.md`](MIGRATION.md).

## Supported scope

QtAVCore's maintained support matrix remains Windows, Android, and OHOS. The
implemented production paths today are Windows and Android; OHOS production
implementation is deferred. macOS and iOS code is archived under
`../archived_apple/`, and Linux is outside the support matrix.

The active implementation has these non-negotiable boundaries:

- no Qt dependency in `modern/`;
- FFmpeg 8.0 or newer, supplied for supported targets by `../ffmpeg/`;
- no FFmpeg, graphics API, window-system, or platform-audio types in core
  public headers;
- hardware decode, hardware-frame interop, rendering, audio output, and native
  window ownership remain separate responsibilities;
- native frames use reference-counted lifetime and explicit synchronization;
- zero-CPU-copy means no decoded-source CPU map, software transfer, CPU
  staging, or re-upload. It may still include a GPU-only representation
  normalization pass and an intermediate texture. Final render-target readback
  is allowed only for validation;
- strict no-intermediate source zero-copy is a narrower claim: the retained
  decoded native buffer has an explicit graphics format and plane mapping and
  is wrapped directly as libplacebo's source, with no normalization draw or
  intermediate source texture.

## Repository layers

```text
modern/
├── core/                         toolkit- and platform-neutral player/API
├── backends/
│   ├── render/
│   │   ├── cpu/                  libswscale image-buffer reference
│   │   ├── d3d11/                Windows external-context renderer
│   │   ├── libplacebo/           shared FFmpeg/libplacebo frame bridge
│   │   ├── vulkan/               libplacebo Vulkan renderer
│   │   ├── opengl/               libplacebo OpenGL ES renderer
│   │   └── mobile/               Vulkan/OpenGL selection and recovery
│   ├── audio/                    resample, file, WASAPI, AAudio, and OHAudio
│   ├── hwaccel/                  D3D11VA and MediaCodec decoder adapters
│   ├── interop/                  D3D11 and MediaCodec GPU-frame bridges
│   └── output/d3d11/             high-level Windows composition output
├── platform/                     small Windows/Android/OHOS OS helpers
├── examples/                     integration applications and harnesses
└── tests/                        deterministic and connected-device coverage
```

`qtav_core` is installed as `QtAV::Core`. Optional backends are separate CMake
targets and link only the SDKs and libraries they own. Applications choose and
compose the required targets; the core does not discover runtime plugins.

## Core playback model

`qtav::Player` is a PIMPL facade. Public calls enqueue asynchronous work and do
not expose decoder or platform objects. The current data flow is:

```text
application control
       │
       ▼
demux/control worker
       ├── bounded audio packets ──► audio decode worker
       │                                  │
       │                                  ▼
       │                            bounded PCM queue
       │                                  │
       │                                  ▼
       │                            audio-output worker
       │                            + device-clock cache
       │
       └── bounded video packets ──► video decode worker
                                          │
                                          ▼
                              timestamp-ordered presentation queue
                                          │
                                          ▼
                              callbacks and render scheduling
                                          │
                                          ▼
                              application graphics-owner thread
```

Audio and video decode are isolated so an audio-device write, GPU import, or
render callback cannot stop packet delivery to the other stream. Queues are
bounded. Late video is dropped rather than converted into unbounded latency.
The audio device clock is the playback master when valid; otherwise a bounded
monotonic fallback is used.

Decoded `AudioFrame` and `VideoFrame` objects are cheap reference-counted
views. Copying a frame retains its backing FFmpeg frame or hardware token. A
pending hardware import must retain the exact decoded frame it is correlating;
it may not substitute the player's newer current frame.

## Rendering contract

`VideoRenderAPI` defines renderer lifecycle and target geometry without naming
a graphics API. `Player::setRenderCallback()` requests a redraw, and the
application calls `renderVideoDetailed()` on the thread that owns the native
graphics context. Immutable current-frame and renderer-binding snapshots keep
that hot path independent of the Player control mutex. The detailed result
carries status, structured retry reason, frame sequence, and presentation
generation; generation is
checked again after the backend call so an invalidated frame is not presented.
The older `renderVideo()` timestamp/negative-value contract remains as a
compatibility wrapper. Multiple renderer instances may be keyed by application
opaque pointers.

The high-level Windows output reserves the shared immediate context before
each output pass makes its first non-blocking acquisition. An uncontended pass
continues immediately; contention uses a private-thread, at-most-8-ms handoff
before the one-frame latest retry mailbox and bounded timer backoff. The
reservation is honored by FFmpeg/internal D3D11 users, owns no context itself,
and is released with the render result before retry classification,
application-requested statistics reads, or `Present()`. D3D11 statistics are
tiered as off, counters, or coarse timing; the output never consumes them as a
retry-control channel.

Windows decoder lifetime remains split across responsibilities. Core retains a
compatible initialized D3D11 hardware-frames context across repeated FFmpeg
format selection; `QtAV::HWD3D11VA` opts the paired repository FFmpeg package
into retaining the matching decoder and output views with that context. Device,
format, dimensions, pool capacity, texture, profile, and complete decoder
configuration are compatibility gates, so a real change still creates a new
pool and decoder. The renderer separately retains each imported source through
GPU completion, destroys renderer-owned wrappers, and hands the source frame
plus interop reference to a fixed-capacity release worker. This worker keeps
final FFmpeg/driver destruction off the graphics-owner thread but does not
replace compatible decoder reuse: a driver may serialize the shared device no
matter which CPU thread performs an incompatible decoder teardown.

The renderer owns resources derived from its API device/context. Native window
and presentation ownership stays in a platform adapter or high-level output:

- `QtAV::RenderVulkan` borrows a Vulkan instance/device/queue and current
  image; `QtAV::RenderVulkanAndroid` owns the Android surface and swapchain;
- `QtAV::RenderOpenGL` borrows the current OpenGL ES context/framebuffer;
  `QtAV::RenderOpenGLAndroid` owns EGL display/context/window-surface/swap;
- `QtAV::RenderD3D11` borrows Windows D3D11 resources and uses libplacebo's
  D3D11 backend; Windows does not build the Vulkan or OpenGL render targets;
  `QtAV::OutputD3D11` is the high-level composition owner for ordinary Windows
  presentation.

`QtAV::RenderMobile` owns neither graphics API. It keeps a stable renderer
contract attached to the player, prefers Vulkan at session start, performs
bounded same-API surface recovery, and makes a one-way switch to OpenGL ES
after fatal Vulkan failure. Decoder and interop reconfiguration is an explicit
application/platform decision; a frame produced for a retired native surface
is never retried through another API.

## libplacebo color pipeline

libplacebo is the shader and color-pipeline authority for the D3D11, Vulkan,
and OpenGL ES renderers. QtAVCore no longer maintains handwritten shaders
for YCbCr-to-RGB matrices, transfer functions, primaries conversion, Dolby
Vision reshaping, tone mapping, gamut mapping, scaling, or SDR/HDR output
encoding.

The shared target `QtAV::RenderLibplaceboCommon` bridges an internal FFmpeg
frame to libplacebo's FFmpeg mapping API. The GPU renderers then supply:

- the source frame and its structured range, matrix, transfer, primaries,
  chroma-location, HDR10, and Dolby Vision metadata;
- crop and display geometry;
- an SDR, BT.2020/PQ, BT.2020/HLG, scRGB, or extended-linear output contract;
- a borrowed target texture/framebuffer and synchronization hooks.

libplacebo generates the backend shaders and performs the complete semantic
color pipeline. A backend-local shader is permitted only for unavoidable
representation conversion that preserves raw source components. It must not
apply a color matrix, inverse/forward transfer, gamut conversion, Dolby
Vision reshape, tone mapping, or output encoding.

Platform interop is limited to native-buffer import, format/plane exposure,
producer/consumer synchronization, timestamp and generation correlation, and
reference-counted lifetime through GPU completion. It must not become a second
color pipeline. This division applies equally to Windows D3D11, Vulkan, and
OpenGL ES: libplacebo is their sole semantic color, Dolby Vision, tone-map,
gamut-map, scaling, and output-encoding authority.

This boundary currently allows two mobile normalization passes:

- Vulkan external-format hardware images may be normalized to an explicit raw
  Y/Cb/Cr representation when libplacebo cannot wrap the opaque external
  format directly;
- OpenGL ES samples an AHardwareBuffer-backed EGLImage with
  `GL_EXT_YUV_target` and stores crop-aware raw Y, Cb, and Cr in RGBA16F before
  libplacebo renders it.

These passes are GPU-to-GPU representation work, not decoded-source copies and
not alternative color pipelines. They satisfy the zero-CPU-copy definition
when their source lifetime and synchronization are retained, but they do not
satisfy strict no-intermediate source zero-copy. A Vulkan native image qualifies
for the strict claim only when an explicit `VkFormat` and plane mapping let the
renderer wrap the retained decoded allocation directly for libplacebo.

Windows needs no normalization shader. By default the decoder array is not
shader-readable: the renderer copies only the even-aligned visible rectangle
into a same-format NV12/P010 shader-readable texture, then libplacebo wraps its
luma and chroma plane views. The bounded copy remains GPU-local and zero-CPU-
copy, but it is not strict no-intermediate source zero-copy. An explicit option
`D3D11VideoOutputOptions::directDecoderTextureSampling = true` instead creates
`D3D11_BIND_SHADER_RESOURCE` decoder textures and wraps the retained array
slice directly; only that mode satisfies the strict claim. The switch defaults
to `false`, selecting the GPU-copy path. Both modes retain the source/import
through GPU completion and defer final release to the bounded recycler. A
D3D11 Video Processor RGB conversion is deliberately absent in both modes
because it would erase the raw Profile 5 representation before Dolby Vision
reshaping. These contracts are governed by [AD-010](DECISIONS.md#ad-010-windows-copies-the-visible-decoder-region-by-default)
and [AD-011](DECISIONS.md#ad-011-windows-reuses-compatible-d3d11va-frames-contexts-and-decoders).

## Dolby Vision and HDR behavior

FFmpeg parses Dolby Vision RPU data into frame side data. libdovi is not
required or enabled. QtAVCore passes metadata to libplacebo only for the
base-layer, residual-disabled case (`disable_residual_flag`); it does not
reconstruct an enhancement layer. Android correlates MediaCodec output with
the parsed RPU by presentation timestamp; OHOS does the same in the FFmpeg
OHCodec wrapper using the exact microsecond PTS submitted to and returned by
the codec, then retains that metadata-bearing `VideoFrame` through normalized
NativeImage timestamp matching. Windows retains FFmpeg's RPU on the decoded
D3D11VA-backed `VideoFrame`. Dolby licensing and certification are outside the
project scope.

Profile 5 has no conventional HDR10-compatible base layer. Its raw base-layer
components must reach libplacebo before ordinary color conversion:

```text
MediaCodec + parsed RPU
       │
       ▼
private AImageReader / AHardwareBuffer
       ├── Vulkan external image ──► raw Y/Cb/Cr ──┐
       └── EGLImage + GL_EXT_YUV_target ──────────┤
                                                  ▼
                                      libplacebo DOVI reshape
                                      + color/tone/gamut pipeline
                                                  │
                         ┌────────────────────────┴──────────────────────┐
                         ▼                                               ▼
                  SDR target                                      HDR target
                  tone-mapped sRGB                                BT.2020/PQ
```

An Android import that cannot prove the raw-component contract is rejected for
Dolby Vision. Implicit `SurfaceTexture`/external-OES conversion is not treated
as a valid Profile 5 source.

The Windows path applies the same ordering through D3D11 only:

```text
FFmpeg HEVC/D3D11VA + parsed RPU
       -> retained P010 texture-array slice
       -> visible-region same-format GPU copy (default)
          or direct decoder slice (explicit option)
       -> libplacebo D3D11 luma/chroma plane views
       -> Dolby Vision reshape
       -> libplacebo color/tone/gamut pipeline
       -> BGRA8 sRGB, FP16 scRGB, or RGB10/PQ D3D11 target
       -> DXGI composition/presentation
```

The native D3D11 layer selects resources, swap-chain color spaces, display
capabilities, and presentation timing. It does not parse Dolby metadata or
implement YCbCr conversion, transfer functions, tone mapping, gamut mapping,
or output encoding in a separate shader.

The user-facing HDR policy selects the application renderer's output target:

- HDR enabled requests a supported native HDR surface and preserves HDR
  through libplacebo;
- HDR disabled selects an SDR surface and libplacebo performs Dolby Vision
  reshape before tone mapping to SDR;
- when ZeroCopy is disabled while MediaCodec hardware decode remains enabled,
  Android uses direct-Surface presentation. That path is owned by MediaCodec
  and Android composition and bypasses libplacebo, so renderer HDR/tone-map
  controls do not apply to it.

## Android hardware-frame paths

Hardware decode is provided by `QtAV::HWMediaCodec`; it owns codec interaction
but not the destination surface. Each surface token has a generation, and each
decoded output receives exactly one present or drop decision.

Application-rendered Vulkan path:

```text
MediaCodec
  -> private AImageReader
  -> retained AHardwareBuffer + acquire fence
  -> Vulkan external memory / raw components
  -> libplacebo Vulkan renderer
  -> release semaphore/sync fd
  -> asynchronous AImage release
```

Application-rendered OpenGL ES path:

```text
MediaCodec
  -> private AImageReader
  -> retained AHardwareBuffer + acquire fence
  -> EGLImage
  -> GL_EXT_YUV_target raw Y/Cb/Cr normalization
  -> libplacebo OpenGL renderer
  -> EGL native release fence
  -> asynchronous AImage release
```

Both paths preserve crop, timestamp/generation, exact-frame ownership, and
producer/consumer fence order. Native-buffer queues remain bounded. Cache
entries may reuse GPU imports by AHardwareBuffer identity, but a codec output
and its AImage remain retained until the GPU has finished consuming them.

Direct-Surface mode is a third, separate path. It has no application-readable
texture and therefore cannot be described as Vulkan, OpenGL ES, libplacebo, or
application-side tone mapping.

## OHOS hardware-frame interop

OHCodec direct-surface presentation remains separate from application-rendered
texture interop. `QtAV::InteropOHCodecVulkan` implements the preferred strict
Vulkan route. It owns a private `OH_ConsumerSurface`, presents exactly one
retained OHCodec output into that surface, acquires the corresponding
`OHNativeWindowBuffer`, retains its `OH_NativeBuffer`, imports the acquire sync
fd and memory through `VK_OHOS_external_memory`, and releases the consumer
buffer only after renderer GPU completion. This path accepts only an explicit
sampled two- or three-plane `VkFormat` that libplacebo can wrap directly. An
opaque external format is rejected rather than normalized, so the target does
not claim a weaker result than strict no-intermediate source zero-copy.

The OHOS OpenGL ES fallback target is raw import through
`GL_EXT_YUV_target`, followed by a crop-aware RGBA16F GPU normalization of raw
Y/Cb/Cr before libplacebo. That fallback is also zero-CPU-copy but not strict
source zero-copy. Implicit external-OES YUV-to-RGB conversion is not a target:
it hides the source representation from libplacebo and cannot preserve the raw
contract required for Dolby Vision. OHCodec/NativeImage may propagate the codec
PTS unchanged in microseconds, so the interop compares the observed value and
its microsecond-to-nanosecond candidate against the exact queued-frame PTS set,
then stores and correlates the selected value in nanoseconds. The connected
Mate 60 Pro exposes only `VK_FORMAT_UNDEFINED` plus an opaque external-format
ID for real H.264 and HEVC outputs. Its strict `UNSUPPORTED` result validates
the fail-closed gate; a texture-interop success remains unclaimed until an
explicit multi-plane format is imported, sampled, and released after GPU
completion on suitable hardware.

The connected Profile 5 and Profile 8.4 OpenGL ES runs each rendered 45 HEVC
frames with `45/45/45` Dolby Vision queued/timestamp-matched/released counts,
zero implicit-RGB images, and zero decoded-source map, transfer, staging, or
upload calls. Profile 8.4 exercises MMR reshaping; the repository libplacebo
overlay corrects its generated GLES array-index types and third-order branch
syntax so the strict Maleoon shader compiler accepts that path. This validates
the raw OpenGL ES half of the Dolby Vision route. The strict Vulkan half stays
open because this device reports the P010 consumer buffer only as
`VK_FORMAT_UNDEFINED` plus an opaque external-format ID.

## Audio architecture

`AudioSink` is a platform-neutral lifecycle and timing contract. Decoded PCM
crosses a bounded queue to an audio-output worker. If the device negotiates a
different format, `QtAV::AudioResample` performs conversion before submission.

- `QtAV::AudioWASAPI` owns Windows shared-mode device output and clocking;
- `QtAV::AudioAAudio` feeds Android's realtime callback from a bounded SPSC
  queue and rebuilds disconnected streams outside the callback;
- `QtAV::AudioOHAudio` feeds OHOS's realtime callback from the shared portable
  SPSC queue, publishes hardware-committed timing, and rebuilds route-changed
  or failed streams on its backend worker;
- `QtAV::AudioFile` is a diagnostic RIFF/WAVE sink and never becomes a device
  clock.

No platform audio header reaches the core API.

## Build and package boundaries

Supported-target FFmpeg and transitive dependencies come only from the
repository `../ffmpeg/` vcpkg subproject. D3D11, Vulkan, and OpenGL ES
rendering require the packaged libplacebo build. Windows additionally requires
`pl_has_d3d11=1` and SPIRV-Cross for libplacebo's SPIR-V-to-HLSL compilation;
Android requires `pl_has_opengl=1` for `QtAV::RenderOpenGL`. The current Windows
package also exposes the opt-in compatible D3D11VA decoder-reuse ABI required
by `QtAV::HWD3D11VA`; `cmake/verify-install.cmake` checks that field. A stock or
independently downloaded Windows FFmpeg is not an ABI-compatible fallback for
that backend. The extension and retirement condition are governed by
[FFmpeg FD-005](../ffmpeg/DECISIONS.md#fd-005-reuse-a-compatible-d3d11va-decoder-with-its-frames-context).

Public targets expose only their required installed dependencies. Build-tree,
NDK, SDK, and producer-machine paths must not leak into exported CMake targets.
Static and shared installs are validated with external `find_package` consumers.

## Platform status and next work

Windows D3D11/D3D11VA/WASAPI and Android Vulkan/OpenGL ES/MediaCodec/AAudio
are the completed production paths. OHOS now has Vulkan and OpenGL ES software
presentation, shared selector fallback, OHAudio output, and explicit OHCodec
H.264/HEVC decoder selection plus single-decision present/drop/timed surface
output on a retained window generation. The surface-output decoder shares the
MediaCodec packet-feed and output-retention bounds, but its OHOS SDK types and
native lifetime remain backend-local. Direct surface output remains separate
from the optional native-buffer interop targets.

Direct `OHNativeWindow` present/drop scheduling and its lifecycle matrix are
connected-device validated. The Vulkan-first interop code, shared/static
package consumption, exact consumer-buffer acquisition, and opaque-format
rejection are also validated. The current device cannot exercise the final
direct-plane import/sampling/GPU-release gate, so no OHOS Vulkan texture PASS
is claimed yet. [`PLAN.md`](PLAN.md) remains the source of truth for task
ordering. The transferred Intel Windows investigation traced the persistent
post-seek stall to redundant D3D11VA decoder teardown and repaired it with the
compatible frames-context/decoder policy above. The remaining Windows gate is
the same final Release revision on NVIDIA and AMD, `legend.mkv` only, with
`directDecoderTextureSampling` both off and on.

## Architectural invariants for changes

Before accepting a backend change, verify that:

1. core public headers still contain no Qt, FFmpeg, graphics, or platform SDK
   types;
2. the graphics-owner thread performs rendering and API-context work;
3. bounded queues and reference-counted exact-frame lifetime are preserved;
4. native-buffer release occurs only after GPU completion;
5. a zero-CPU-copy claim keeps decoded-source map/transfer/staging/upload counters
   at zero;
6. a strict source zero-copy claim additionally proves an explicit native
   format/plane mapping and no pre-libplacebo normalization texture or draw;
7. libplacebo remains the sole semantic color/shader authority for D3D11,
   Vulkan, and OpenGL ES;
8. direct-Surface and application-rendered modes are reported distinctly;
9. unsupported capabilities fail or fall back explicitly rather than silently
   changing color interpretation or copying decoded pixels;
10. Windows repeated format selection reuses a decoder only after every Core
    frames-context and FFmpeg decoder/configuration compatibility gate passes,
    and the paired overlay ABI remains install-verified.
