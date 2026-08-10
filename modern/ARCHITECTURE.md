# QtAVCore architecture

This document describes the current structure and ownership boundaries of the
active Qt-free rewrite under `modern/`. Milestone status and task ordering live
in [`PLAN.md`](PLAN.md); durable trade-offs live in
[`DECISIONS.md`](DECISIONS.md); migration from legacy QtAV is documented in
[`MIGRATION.md`](MIGRATION.md). Completed checklists, investigation narratives,
device matrices, and historical validation evidence live in
[`PLAN_HISTORY_2026-08-10.md`](PLAN_HISTORY_2026-08-10.md), not in this current
state description.

## Supported scope

QtAVCore's maintained support matrix remains Windows, Android, and OHOS. The
implemented production paths cover Windows, Android, and the current OHOS
policy. OHOS strict direct multi-plane Vulkan wrapping and its corresponding
Dolby Vision validation remain gated on suitable explicit-format hardware;
that narrower gate does not make the implemented OHOS software, audio,
direct-surface, OpenGL ES, or bounded opaque-format Vulkan paths provisional.
macOS and iOS code is archived under `../archived_apple/`, and Linux is outside
the support matrix.

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

Windows has one production graphics path: D3D11VA decode, D3D11 interop and
D3D11 rendering/presentation. D3D11 is the default and complete Windows
backend. QtAVCore does not build, export, or plan a Windows Vulkan renderer,
swapchain output, or D3D11VA-to-Vulkan interop target. The portable Vulkan
renderer remains part of the Android/OHOS architecture; that does not imply a
Windows Vulkan support commitment. The evidence and rejection decision are
recorded in [AD-012](DECISIONS.md#ad-012-reject-a-production-windows-vulkan-backend).

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
│   ├── audio/                    resample, time stretch, file, and device sinks
│   ├── subtitle/                 optional libass text/ASS rasterization
│   ├── hwaccel/                  D3D11VA, MediaCodec, and OHCodec adapters
│   ├── interop/                  D3D11, MediaCodec, and OHCodec GPU bridges
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
       ├── main + external audio/subtitle timestamp merge
       ├── bounded audio packets ──► audio decode worker
       │                                  │
       │                                  ▼
       │                            bounded PCM queue
       │                                  │
       │                                  ▼
       │                            audio-output worker
       │                            ├─ optional format conversion
       │                            ├─ optional pitch-preserving time stretch
       │                            └─ physical device-clock cache
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

The main input plus at most one configured external audio and subtitle input
are opened by the same control worker. Their selected packets are ordered on a
timeline normalized by each input's start time, while the main input remains
the duration/end-of-media authority. Seek, loop, track replacement, and media
replacement reset all active demux states at one presentation-generation
boundary. Public track selectors are unique across inputs; the actual
per-input stream index remains diagnostic metadata only. These ownership and
identity rules are governed by
[AD-017](DECISIONS.md#ad-017-the-main-input-owns-the-timeline-while-sidecars-share-one-track-namespace).

Audio, video, and subtitle track switches use the same asynchronous control
boundary. A switch validates the public selector synchronously, invalidates the
retired presentation generation, replaces only the affected decoder state,
restores the media position, and renegotiates the audio sink when required.
Decoded text subtitles are normalized and timed in the core; optional libass
rasterization is a separate backend that consumes retained ASS header/event
data and returns application-owned coverage bitmaps.

Accurate seek and frame stepping are refinements of the same generation model,
not separate synchronous decode entry points. An accurate seek starts at a
preceding keyframe, suppresses pre-target audio/video/subtitle output, and makes
the first video frame at or after the target an immediate, non-droppable
presentation anchor. Its callback reports the actual published timestamp.
Forward and backward steps publish exactly one adjacent frame, keep device
audio paused, and leave Player paused; backward stepping reconstructs history
when the predecessor is not retained. Accepted control work takes precedence
over concurrent natural-end teardown. See
[AD-018](DECISIONS.md#ad-018-accurate-seek-and-frame-stepping-use-presentation-generations-and-an-anchor-frame).

Decoded `AudioFrame` and `VideoFrame` objects are cheap reference-counted
views. Copying a frame retains its backing FFmpeg frame or hardware token. A
pending hardware import must retain the exact decoded frame it is correlating;
it may not substitute the player's newer current frame.

## Buffering, live latency, and recoverable input

Compressed packets remain owned by the control/demux worker and the existing
bounded per-stream queues. One usable-media-time gate releases both selected
A/V decoders for initial playback, seek, track switching, network recovery, or
confirmed underflow. Capacity-limited completion is explicit, and a short
external source may reach its own EOF without holding the main input. AD-013
defines that shared gate.

The optional disk tier in AD-014 spills compressed payloads, not decoder state,
into one bounded player-specific system-temporary file. It is volatile
prefetch, automatically removed across generation resets and destruction, and
is not a persistent download or offline cache.

Low-latency playback and network recovery are deliberately independent:

- AD-015 may keep a bounded newest decoded-video window and drop late video;
  it never removes compressed packet history, audio, subtitles, or clock state;
- AD-016 first lets FFmpeg protocol recovery run, then performs a bounded fresh
  open on the sole format-context owner, validates selected stream identity,
  installs new stream references only after decoder drain and generation
  invalidation, and refills through the packet-buffer gate.

No policy may silently convert a non-seekable reopen into lossless recovery,
adaptive streaming, or unbounded retry.

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
color pipeline. This division applies equally to Windows D3D11 and the
Android/OHOS Vulkan and OpenGL ES paths: libplacebo is their sole semantic
color, Dolby Vision, tone-map, gamut-map, scaling, and output-encoding
authority.

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
texture interop. `QtAV::InteropOHCodecVulkan` owns a private
`OH_ConsumerSurface`, presents exactly one retained OHCodec output into that
surface, acquires the corresponding `OHNativeWindowBuffer`, retains its
`OH_NativeBuffer`, imports acquire synchronization and memory through
`VK_OHOS_external_memory`, and releases the consumer buffer only after renderer
GPU completion.

The Vulkan interop has three explicitly reported format routes:

1. A queried explicit sampled multi-plane `VkFormat` may be wrapped directly by
   libplacebo. This is the preferred strict no-intermediate route.
2. `VK_FORMAT_UNDEFINED` may use the standards-based
   `VkExternalFormatOHOS`/`VkSamplerYcbcrConversion` route and a GPU-only raw-
   representation normalization pass. This remains zero-CPU-copy but is not
   strict source zero-copy.
3. [AD-009](DECISIONS.md#ad-009-ohos-external-format-guessing-is-a-bounded-workaround)
   permits a production-default, closed-allowlist Huawei workaround for known
   video-YUV external IDs. It still gates native format family, usage, sampled-
   image features, image creation, import/bind, views, synchronization, and
   actual sampling at runtime, retains an application kill switch, and falls
   back one-way to OpenGL ES or software decode on failure. It is not a portable
   promise that an arbitrary external ID equals a `VkFormat`.

The OHOS OpenGL ES fallback target is raw import through
`GL_EXT_YUV_target`, followed by a crop-aware RGBA16F GPU normalization of raw
Y/Cb/Cr before libplacebo. That fallback is also zero-CPU-copy but not strict
source zero-copy. Implicit external-OES YUV-to-RGB conversion is not a target:
it hides the source representation from libplacebo and cannot preserve the raw
contract required for Dolby Vision. OHCodec/NativeImage may propagate the codec
PTS unchanged in microseconds, so the interop compares the observed value and
its microsecond-to-nanosecond candidate against the exact queued-frame PTS set,
then stores and correlates the selected value in nanoseconds.

The connected Mate 60 Pro and Pura X Max expose H.264/NV12 and HEVC/P010 as
`VK_FORMAT_UNDEFINED` with the same two observed opaque external IDs. Opaque
import/sampling, the separately diagnosed explicit-format/direct-plane route,
and the bounded production workaround have each passed their recorded 60-frame
matrices with zero decoded-source CPU map, transfer, staging, or upload. These
results validate the named devices and bounded policy, not a general explicit-
plane contract. Strict direct wrapping remains open until a device reports a
non-opaque sampled multi-plane format and passes import, sampling, precision,
and GPU-release validation.

The connected Profile 5 and Profile 8.4 OpenGL ES runs each rendered 45 HEVC
frames with `45/45/45` Dolby Vision queued/timestamp-matched/released counts,
zero implicit-RGB images, and zero decoded-source map, transfer, staging, or
upload calls. Profile 8.4 exercises MMR reshaping; the repository libplacebo
overlay corrects its generated GLES array-index types and third-order branch
syntax so the strict Maleoon shader compiler accepts that path. This validates
the raw OpenGL ES half of the Dolby Vision route. The strict Vulkan half stays
open because the tested devices report the P010 consumer buffer only as
`VK_FORMAT_UNDEFINED` plus an opaque external-format ID.

## General processing architecture

General effects are optional core contracts, not a new ownership layer.
`AudioFrameProcessor` runs on the audio-output worker after negotiated format
conversion and pitch-preserving time stretch, and before the sink. It can
buffer PCM, but it preserves format, media-timeline order, and the completed
segment's physical sample count. The core serializes process, reset, drain, and
close without holding the player mutex. The optional `QtAV::AudioFilter`
reference target owns its FFmpeg `volume` graph; FFmpeg types and arbitrary
filter descriptions do not cross the core boundary.

`VideoFrameProcessor` is a synchronous one-to-one transform on the video-decode
worker. Direct `VideoFrameScheduler` handling has priority; a declined frame is
processed before it enters ordinary presentation. The transform may change
software pixels, geometry, format, and color metadata, but preserves timestamp
and duration. It cannot queue delayed frames. Cadence conversion therefore
requires a future queued contract, while graphics-context effects remain owned
by `VideoRenderAPI` on the native render thread.

Pause/resume preserves processor state. Timeline discontinuities reset it,
natural completion drains it, and track/media or live processor replacement
closes it at a clean serialized generation boundary. Processor failure is
fail-closed and reported through a categorized media event.

## Audio architecture

`AudioSink` is a platform-neutral lifecycle and timing contract. Decoded PCM
crosses a bounded queue to an audio-output worker. If the device negotiates a
different format, `QtAV::AudioResample` performs conversion. For a non-1.0
playback rate, an injected `AudioTimeStretcher` then changes the physical PCM
sample count without changing pitch or media timestamps. An injected
`AudioFrameProcessor` then applies format- and timeline-preserving effects
before submission.

`QtAV::AudioTimeStretch` is the reference implementation and owns an FFmpeg
`atempo` graph. It is a separate optional target; core owns only the streaming
contract and lifecycle. Rate 1.0 bypasses the processor. A live rate change
reopens the audio chain and, for seekable loaded media, crosses an accurate
seek generation at the current position so PCM already submitted at the old
rate cannot leak into the new one.

The sink clock measures actual device PCM elapsed from the first media
timestamp after open or flush. Player owns the rate mapping from that physical
delta back to media time and caps it at submitted media time. This keeps
WASAPI, AAudio, and OHAudio responsible only for device timing and keeps the
same mapped media clock as the A/V master.

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
QtAVCore 2.0.0 is the first formal rewrite release. The root CMake project
version generates the public `<qtav/version.h>`, unconditional
`QtAVCore_VERSION*` package variables, the `SameMajorVersion` discovery file,
and every supported shared target's full `VERSION` plus major `SOVERSION`.
Configuration recursively verifies those shared-target properties before
examples or tests introduce non-package targets.
Windows maps the full property to its PE image version, and OHOS emits the
major ELF soname plus full/major/unversioned library names. Android keeps the
NDK's required unversioned `.so` name and soname; its release identity comes
from the same generated header and installed CMake package.

Package discovery compatibility, source compatibility after rebuilding, and
shared-library binary compatibility are distinct boundaries. Same-major
package discovery requires an installed version at least as new as the request.
Shared ABI compatibility is limited to the same target architecture, compiler
ABI/toolset family, standard library/runtime mode, relevant build mode, and
dependency ABI; static linkage and cross-compiler C++ ABI are not covered.
Release increments and exported-target compatibility are governed by
[AD-022](DECISIONS.md#ad-022-qtavcore-200-starts-the-versioned-c-and-cmake-package-contract).
Optional backends remain compile-time targets in this repository while their
interfaces evolve. Runtime loading, cross-toolchain C++ ABI exposure, and
repository splitting are not current architecture; the conditions for a later
versioned C ABI or split are governed by
[AD-019](DECISIONS.md#ad-019-backends-remain-compile-time-modules-until-a-runtime-boundary-is-justified).

## Platform status and next work

Windows D3D11/D3D11VA/WASAPI and Android Vulkan/OpenGL ES/MediaCodec/AAudio
are complete production paths. The AD-010 Windows visible-copy policy and its
NVIDIA/AMD default/direct matrix are closed. A separate Intel post-seek
performance workstream remains external and must not be described as closed
until its administrator trace, correction, and same-revision cross-vendor gate
pass.

OHOS has Vulkan and OpenGL ES software presentation, shared selector fallback,
OHAudio output, OHCodec H.264/HEVC and capability-gated VVC/H.266 selection,
single-decision direct-surface present/drop/timed output, raw OpenGL ES
hardware-frame interop, and the AD-009 bounded Vulkan policy. These paths and
their fallback/lifecycle matrices are connected-device validated. The remaining
native-buffer item is the narrower explicit-plane direct-wrap/precision gate on
suitable hardware, followed by strict Vulkan Dolby Vision validation.

Core feature work is complete through active audio/video/subtitle track
switching, external audio/subtitle sources, optional libass rasterization,
packet buffering/cache, live-latency control, bounded network recovery,
frame-accurate seek, forward/backward stepping, and optional pitch-preserving
audio time stretch. [`PLAN.md`](PLAN.md) remains the source of truth for task
ordering and incomplete gates.

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
    and the paired overlay ABI remains install-verified;
11. timeline-changing control work invalidates the retired presentation
    generation before output can cross the boundary, and the main input remains
    the duration/range/end-of-media authority when sidecars are active;
12. optional backends remain responsibility-specific compile-time targets until
    a separately accepted versioned runtime boundary exists.
