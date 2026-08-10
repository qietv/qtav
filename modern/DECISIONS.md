# QtAVCore architecture decisions

This document records durable architectural decisions for the active Qt-free
rewrite. It explains constraints and intended behavior; implementation progress
and task ordering remain outside this document.

Decision statuses are `Proposed`, `Accepted`, `Superseded`, or `Rejected`.

## AD-001: Android libplacebo and Dolby Vision rendering

- Date: 2026-08-03
- Status: Accepted
- Scope: Android video rendering, tone mapping, and Dolby Vision

### Context

Dolby Vision Profile 5 has no HDR10 or HLG-compatible base layer. Its decoded
base-layer samples require the per-frame RPU reshape before ordinary RGB color
processing. Treating those samples as conventional YCbCr produces visibly
incorrect color.

FFmpeg 8.1.2 can parse the RPU into `AVDOVIMetadata`. The optional libdovi
parser is therefore unnecessary. MediaCodec direct-Surface presentation does
not expose a texture to the application renderer and consequently bypasses
libplacebo.

### Decision

1. libplacebo is the authority for application-rendered color conversion,
   Dolby Vision RPU reshaping, tone mapping, gamut mapping, and output encoding.
   Backend-owned shaders may only perform unavoidable GPU representation work,
   such as crop-aware external-image normalization; they must not implement a
   competing color pipeline.
2. libdovi remains disabled. FFmpeg parses the RPU, and decoded frames carry
   FFmpeg's `AVDOVIMetadata` to libplacebo.
3. The supported Android Dolby Vision Profile 5 hardware paths are:

   ```text
   MediaCodec -> AImageReader/AHardwareBuffer -> Vulkan -> libplacebo
   MediaCodec -> AImageReader/AHardwareBuffer -> EGLImage raw YCbCr
              -> OpenGL ES -> libplacebo
   ```

   The decoded source must remain free of CPU mapping, readback, transfer,
   staging, or upload. An on-GPU external-format normalization pass is allowed
   when it preserves raw Y, Cb, and Cr for libplacebo.
4. Hardware decode, ZeroCopy, Vulkan, and HDR are the Android player defaults.
   Disabling ZeroCopy while hardware decode remains enabled selects MediaCodec
   direct-Surface presentation and therefore does not use libplacebo.
5. With an application renderer active, enabling HDR requests a native HDR
   target and preserves HDR when the display supports it. Disabling HDR selects
   an SDR target; libplacebo must apply the Dolby Vision reshape first and then
   tone map the result to SDR. The HDR switch cannot promise application-side
   tone mapping while direct-Surface presentation is active.
6. OpenGL ES uses the libplacebo OpenGL backend. Its Android hardware interop
   imports private `AImageReader` images as AHardwareBuffer-backed EGLImages,
   samples raw Y, Cb, and Cr with `GL_EXT_YUV_target`, and normalizes those
   components into an internal floating-point texture before libplacebo sees
   the frame. The normalization shader performs no matrix, transfer, gamut,
   tone-mapping, or output-encoding work.
7. An OpenGL hardware import that cannot prove the raw-component contract must
   be rejected for Dolby Vision rather than treated as conventional YCbCr.
   SurfaceTexture's implicit external-OES color conversion is not a supported
   Dolby Vision source path.

### Consequences

- Dolby Vision Profile 5 may use either validated libplacebo backend. A
  Vulkan-to-OpenGL ES fallback must first rebind MediaCodec to the OpenGL
  interop's private `AImageReader`; a frame from the retired Vulkan producer is
  never retried through OpenGL ES.
- Direct-Surface presentation remains an explicit codec-passthrough mode, not
  a libplacebo mode.
- Both GPU paths preserve zero decoded-source CPU copies and are validated
  independently.

### Current implementation and validation

Implemented:

- FFmpeg-parsed RPU metadata is attached to MediaCodec output frames by
  presentation timestamp;
- Vulkan uses libplacebo for Dolby Vision reshape and SDR/HDR rendering;
- Android external-format hardware frames expose an unconverted raw-component
  sampler for the libplacebo path;
- OpenGL ES uses libplacebo for software and hardware frames, and its Android
  interop imports `AImageReader`/AHardwareBuffer images through EGLImage with
  acquire/release native-fence synchronization;
- the OpenGL-only normalization pass preserves crop-aware raw Y/Cb/Cr in an
  RGBA16F texture before libplacebo performs the Dolby Vision reshape and all
  color processing;
- the Android player defaults to the Vulkan ZeroCopy path and reports RPU frame
  and raw YCbCr import diagnostics.

Connected-device validation used `/sdcard/Download/Wednesday.mp4`, identified
as Dolby Vision Profile 5, Level 6, compatibility ID 0. The validated path
received RPU metadata on every observed frame, presented through a 10-bit
BT.2020/PQ Vulkan surface with HDR metadata, and retained AHardwareBuffer
zero-copy operation without queue or late-frame drops. Independent OpenGL ES
validation presented the same file through raw AHardwareBuffer/EGLImage YCbCr
imports and libplacebo to both a BT.2020/PQ HDR EGL surface and an sRGB SDR EGL
surface. RPU processing and raw-import counters continued in both modes; the
SDR mode reported libplacebo SDR output rather than direct-Surface passthrough.

## AD-002: Android long-form playback scheduling and software decode threading

- Date: 2026-08-03
- Status: Accepted
- Scope: Android long-form A/V scheduling, MediaCodec interop, software video
  decoding, and player diagnostics

### Context

Local playback of `/sdcard/Download/legend.mkv` and
`/sdcard/Download/suzume.mkv` appeared to pause approximately every two
seconds even when the original debug window reported no dropped frames. The
problem was observable with and without MediaCodec ZeroCopy, while other
Android players presented the same files smoothly. Disabling hardware decode
also initially produced no picture for these 10-bit files.

The sources used for diagnosis were:

- `legend.mkv`: 3840x2160, 25 fps, HEVC Main 10, `yuv420p10le`;
- `suzume.mkv`: 3840x1608, 24 fps, HEVC Main 10, `yuv420p10le`.

A decoded-frame or callback count alone cannot identify this failure. A queue
can repeatedly become empty and then catch up without overflowing or dropping
a frame, producing visible freezes while aggregate decoded and presented
counts remain close.

### Investigation path

1. Reproduce each path independently: MediaCodec direct Surface,
   MediaCodec/AImageReader Vulkan ZeroCopy, software Vulkan, and software
   OpenGL ES. Report the actual active path and output color space rather than
   inferring them from switch state. This separated rendering, HDR, and decode
   policy from the timing fault.
2. Remove per-frame logging as a variable and measure it. Normal logging was
   limited to setup, first-frame, Dolby Vision, and error events. CPU sampling
   later placed UI and diagnostic work below one percent, so logging was not
   the cause.
3. Split diagnostics into application callbacks/presents, render duration,
   core decoded/delivered frames, bounded-queue and late drops, queue
   high-water mark, presentation starvation count/maximum duration, and pacing
   stalls/catchups. This exposed frequent queue starvation that the former
   drop-only display hid.
4. Trace the hardware path. Rendering had run inline with playback work;
   renderer `RedrawRequested` events were not reliably returned to the native
   render loop; and an AImageReader import that was still pending could later
   be retried using the player's newer current frame instead of the exact
   MediaCodec output being correlated. Producer bursts could therefore
   coalesce private AImages, while audio decode/output backpressure could delay
   video packet delivery.
5. Separate demux, audio decode, video decode, presentation, audio submission,
   and native rendering. Pace MediaCodec packets by DTS before decode, retain
   only a bounded decoder-output window, reserve a renderer slot before
   releasing ZeroCopy output, retain the exact frame across the bounded
   AImageReader ownership wait, route redraw requests to the native render
   thread, and accept zero as a valid AImage timestamp. Add explicit planar
   10-bit software-frame mapping so `yuv420p10le` reaches both renderers.
6. Re-test software decode after the scheduling fixes. `simpleperf` showed
   about 70 percent of sampled CPU in FFmpeg HEVC Main 10 work on one thread,
   with that thread continuously saturated. Vulkan rendering used about 12
   percent and averaged roughly 17-19 ms per frame, below the 40 ms budget of
   `legend.mkv`. Thermal status was normal and more than five CPU cores
   remained idle; the device was not globally underpowered.
7. Inspect FFmpeg configuration and compare the legacy QtAV decoder. The
   Android FFmpeg build had pthread and AArch64/NEON support, but libavcodec's
   context default was one decode thread and QtAVCore opened it unchanged.
   Legacy `VideoDecoderFFmpeg` instead defaults `threads` to zero (automatic)
   and enables both frame and slice threading.

### Decision

1. Playback work remains split across bounded packet, decode, presentation,
   audio-output, and application render stages. No codec, device write, GPU
   import, or application render callback may block demux or the other codec's
   packet delivery.
2. Application-rendered Android output runs on the native graphics-owner
   thread. A retryable backend result or `RedrawRequested` schedules another
   attempt there; it does not block a UI/playback worker.
3. A pending hardware import retains the exact reference-counted decoded frame
   and its presentation deadline until success, expiry, or explicit retirement.
   It must never substitute a newer player-current frame for an older pending
   MediaCodec/AImageReader correlation.
4. MediaCodec output is paced before decode and kept within a small bounded
   window. ZeroCopy presentation reserves bounded application capacity before
   releasing another codec output, preventing a producer burst from silently
   replacing images in a latest-image queue.
5. Every software video decoder sets `AVCodecContext::thread_count` to zero and
   requests `FF_THREAD_FRAME | FF_THREAD_SLICE` before `avcodec_open2()`.
   Libavcodec then selects a suitable worker count for codecs that support
   threading. Hardware decoder contexts keep their backend-specific policy.
6. Long-form smoothness is assessed from starvation duration, late/queue
   drops, successful presentation rate, and render timing together. Close
   decoded/presented totals are necessary but not sufficient evidence.
7. Per-frame logging remains disabled. Diagnostic counters are accumulated in
   memory and rendered at a bounded UI cadence.

### Consequences

- Software HEVC Main 10 decoding can use the available heterogeneous Android
  CPU instead of saturating one core. Automatic frame threading retains more
  decoded reference frames and therefore trades additional memory and decode
  latency for stable throughput.
- Hardware and software scheduling share the same bounded presentation model,
  while decoder output ownership and graphics-thread ownership remain
  separate responsibilities.
- A refresh-rate request, HDR surface state, ZeroCopy state, or zero drop count
  cannot by itself be used to declare playback smooth. The diagnostics must
  identify the active path and show whether presentation is being starved.
- Direct-Surface mode is labelled as Android/MediaCodec-owned presentation;
  Vulkan and application HDR controls must not imply that the Vulkan renderer
  is active in that mode.

### Connected-device validation

Before enabling automatic software decode threading, a long
`legend.mkv` run recorded 4,730 delivered frames, nine late drops, 1,540
presentation starvations, and a maximum starvation of 506 ms. A call-graph
profile placed the HEVC decode work on one saturated thread; the whole process
used about 1.33 CPU cores over a ten-second sample.

After setting the software decoder to automatic frame/slice threading, FFmpeg
created nine `av:hevc:df0` through `av:hevc:df8` workers and used about 2.05
CPU cores over the same interval. On the connected SM8750/Adreno 830 device:

- `legend.mkv` presented 2,625 consecutive frames over approximately 105
  seconds with zero queue drops, zero late drops, and zero presentation
  starvation;
- `suzume.mkv` presented 2,821 consecutive frames over approximately 118
  seconds with zero queue drops, zero late drops, and zero presentation
  starvation; its average/maximum render time was about 15.8/40.7 ms.

These results distinguish the resolved scheduling and decoder-parallelism
faults from device performance, thermal throttling, logging, display refresh,
ZeroCopy selection, and HDR output policy.

## AD-003: Android OpenGL ZeroCopy asynchronous ownership and present ordering

- Date: 2026-08-03
- Status: Accepted
- Scope: Android player OpenGL ES MediaCodec/AImageReader scheduling and EGL
  release synchronization

### Context

The Vulkan fixes in AD-002 had already separated demux, audio decode, video
decode, presentation, audio output, and native rendering. OpenGL ES still had
two path-specific ownership faults. It released a MediaCodec output from the
graphics thread and could wait there for an AImage; moving that wait to the
video scheduler instead caused the bounded compressed-video packet queue to
fill and eventually backpressure the shared demux feed, starving audio.

Making release fully asynchronous exposed two further invariants. The example
did not count a frame removed from its queue but still waiting/rendering as one
of its four reserved slots. The AImageReader listener could also reach
`MAX_IMAGES_ACQUIRED` while draining a coalesced callback and leave an
already-queued image without a later listener edge. Finally, the generic
renderer returned the source AImage before the Android adapter called
`eglSwapBuffers()`. On this device the exported native fence could therefore
exclude deferred default-framebuffer work. After enough frames, asynchronous
AImage returns accumulated behind unsignalled fences and exhausted the reader.

The failing reopen run of `wednesday.mp4` fell to 7.8 successful presents per
second with 943 callbacks, 541 presents, 397 application drops, and 398 core
late drops. A later ordinary-HDR run stopped permanently at
queued/acquired/imported 2564/2556/2546 and emitted an AAudio underrun.

### Decision

1. `MediaCodecOpenGLInterop::queueFrame()` only registers the exact frame key
   and releases the codec output. It never waits for AImage ownership or does
   EGL/GL work on the video-decode worker.
2. The Android example reserves one of four application slots before release
   and counts the frame currently being attempted until it is presented,
   retired, or returned to the pending queue.
3. The private AImageReader has two acquisition slots in addition to its
   four-image timestamp-correlation window. Images evicted while a listener
   callback drains are returned immediately rather than being retained until
   the callback completes.
4. `OpenGLVideoRenderer` exposes an optional `OpenGLPresentCallback`. It runs
   with the graphics context current after libplacebo framebuffer submission
   and before `OpenGLHardwareFrameInterop::releaseFrame()`. The Android adapter
   performs `eglSwapBuffers()` there, so the exported release fence covers both
   sampling and window presentation.
5. An AImageReader completion only schedules work on the native render thread.
   Pending imports retain the exact reference-counted frame and deadline and
   return immediately when the image is not ready.

### Consequences

- Audio/video decode separation remains in the core and no OpenGL ownership
  wait is moved onto either decoder or the shared demux path.
- The OpenGL renderer gains a public, platform-neutral presentation hook.
  Adapters that need a window submit before native-source release should use
  it; offscreen callers can leave it empty.
- The decoded source remains zero-copy: the additional acquisition capacity
  changes only ownership slack, and the existing AHardwareBuffer/EGLImage,
  raw-YCbCr, and native-fence path is retained.
- Queue counters can trail by the small bounded in-flight window; smoothness is
  still judged from source-rate presents, non-growing depth, core drops,
  starvation, and audio underruns together.

### Connected-device validation

The Release APK was installed on the Android 16/Adreno 830 device and tested
through OpenGL ES ZeroCopy with the requested files:

- Dolby Vision Profile 5 `wednesday.mp4`: the first run reached 1,427 callbacks
  at 24.0 fps; after force-stopping and reopening the application, the second
  run reached 3,632 callbacks and 3,626 presents at 23.8 fps with core
  queue/late drops `0/0` and no AAudio underrun;
- the reopened P5 session passed pause/resume, a slider seek to 13:17, and a
  background/foreground surface rebuild, resuming at 24.0 fps;
- ordinary HDR `legend.mkv` reached 1,454 callbacks and 1,450 presents at
  25.0 fps with core queue/late drops `0/0`; hiding Debug did not interrupt
  playback, which continued past 01:42.

The Android player APK cross-build, v3 signing, installation, raw-YCbCr/Dolby
Vision rendering, native HDR surface, and native-fence return all remained
active during these runs.

## AD-004: Playback time advances only after output resumes

- Date: 2026-08-03
- Status: Accepted
- Scope: Player clock, seek, startup, audio underrun, and application progress

### Context

Resetting a monotonic wall clock to a seek target can make `position()` advance
before the replacement audio/video generation reaches an output. Repeated
forward and backward seeks then appear as a moving timeline with frozen sound
and picture. A clock-capable audio sink also has no valid post-seek device
position until flushed output is submitted and presented.

### Decision

1. An accepted playing seek invalidates obsolete packet, decode, presentation,
   and audio generations, anchors `position()` at the requested target, and
   reports `Buffering` until the new generation produces usable output.
2. With a clock-capable audio sink, playback time resumes only after a valid
   post-flush device-clock sample. Callback-only playback resumes after the
   first new-generation output is actually delivered.
3. A/V startup and playing seeks use bounded video preroll before device audio
   is released. This prevents the audio clock from running ahead while the
   first video frames are still being decoded.
4. `Player::position()` reads a generation-checked cached clock snapshot. It
   never calls the audio device or waits behind a sink write, and any
   extrapolation is bounded by the media time already submitted to the sink.
5. Audio underrun re-enters `Buffering` and freezes fallback time until output
   re-anchors. Applications observe Player state, status, and position; they do
   not advance an independent playback clock.

### Consequences

- The requested seek position can remain visibly unchanged for a short period
  while replacement output is prepared; that is intentional buffering, not a
  stalled UI clock.
- UI timers may sample and display `position()`, but cannot infer progress by
  adding wall time.
- Audio sinks that expose a presentation clock must invalidate it on flush and
  publish a new-generation sample after accepting output.
- Tests for seek and underrun must assert both clock behavior and real output
  resumption, not only asynchronous seek completion.

## AD-005: D3D11 presentation is bounded, retryable, and submission-ordered

- Date: 2026-08-03
- Status: Accepted
- Scope: Player video presentation and Windows D3D11 output, rendering, and
  hardware-frame interop

### Context

Repeated seeks exposed stalls when a render attempt waited behind Player or
D3D11 locks, synchronous swap-chain backpressure, or a synchronous per-frame
GPU-completion wait. The wait serialized CPU submission behind driver
completion; retained D3D11VA decoder surfaces therefore also needed an explicit
bounded lifetime. Recreating interop output textures without bounded reuse made
transient GPU memory look like a process leak.

### Decision

1. Player rendering and the D3D11 renderer do not block the native render or UI
   thread on control, render, or immediate-context ownership. AD-008 replaces
   Player's former non-blocking state-lock attempt with immutable snapshots and
   adds reason-aware results plus bounded output-thread retry/handoff.
2. When the bounded presentation queue is full, it preserves its contiguous
   near-term frames and rejects a farther-future incoming frame. A decode burst
   after seek must not replace the frame that is about to be displayed.
3. `D3D11VideoOutput` owns presentation on its private render thread, caps
   flip-model frame latency at one, coalesces redraws, and uses non-blocking
   `Present()` plus bounded waitable-object backpressure when available before
   retrying.
4. A hardware-frame import remains alive through libplacebo draw-command
   submission and is retained by the renderer's bounded GPU-completion queue.
   AD-007 requires native immediate-context multithread protection and keeps
   successful imported-frame submission asynchronous. Decoder, interop, and
   renderer submissions also share the QtAVCore recursive context guard.
5. Interop retains the decoder's raw NV12/P010 slice and owns no conversion
   texture pool. AD-010 refines the later lifetime: default-copy plane wrappers
   persist on the bounded ordinary-resource ring, while direct wrappers and all
   imported source frames remain retained through GPU completion. Renderer-
   owned wrappers are then destroyed before source/import references move to
   the bounded recycler. Explicit GPU drains remain lifecycle-only operations.
6. Retry attempts, terminal drops, compositor-busy presents, and maximum render
   stage durations are counted without per-frame logging. AD-008 defines the
   reason-level counters and compatibility `skippedRenders` semantics.

### Consequences

- Audio, UI dispatch, and control operations remain responsive when the GPU or
  compositor is temporarily busy.
- The detailed render result determines whether the high-level output retries.
  The compatibility `renderVideo()` negative value intentionally loses that
  distinction; a busy present still schedules a later attempt.
- Driver working set may rise transiently across seek and queued GPU work, but
  retained application resources are bounded and must not grow linearly per
  frame or per seek.
- A future design that submits decoder reuse and rendering on different
  immediate contexts, devices, or unordered queues must add an explicit GPU
  synchronization contract before using the same completion-retention rule.

### Windows validation

The exercised 3840x2160 HEVC Main10 HDR URL completed twelve alternating
forward/backward seeks at 24-25 scheduled and rendered frames per second.
Maximum draw time fell from 155-194 ms to about 0.5 ms and total render time
remained about 3-5 ms. Process private memory measured 965.6 MiB before the
seek run, 986.8 MiB after four seeks, and 984.8 MiB after eight more seeks;
working set returned from a transient 886.4 MiB to 332.6 MiB. This is evidence
of bounded reuse rather than per-seek linear growth, while deterministic
Windows static and shared builds pass all 34 tests.

## AD-006: Windows D3D11 uses libplacebo as its color authority

- Date: 2026-08-03
- Status: Accepted
- Scope: Windows D3D11 software rendering, D3D11VA interop, Dolby Vision, and
  HDR/SDR presentation

### Context

The former Windows renderer implemented YCbCr conversion, PQ/HLG transfer,
primaries conversion, and tone mapping in a handwritten HLSL pixel shader.
Its hardware path first converted D3D11VA NV12/P010 decoder slices to RGB with
the D3D11 Video Processor. That split color semantics across two native paths
and made Dolby Vision Profile 5 incorrect: Profile 5 raw base-layer components
must be reshaped with the FFmpeg-parsed RPU before ordinary YCbCr conversion.

Android already establishes libplacebo as the sole semantic color authority
and preserves exact-frame lifetime, raw-component ordering, and non-blocking
graphics scheduling. Windows needs the same semantic contract without adding
Vulkan or OpenGL to its supported rendering path.

### Decision

1. Windows GPU rendering uses only `QtAV::RenderD3D11`. The Windows build does
   not produce QtAVCore Vulkan or OpenGL renderer targets, including when those
   APIs happen to be installed on the development machine.
2. libplacebo's D3D11 backend owns YCbCr conversion, Dolby Vision RPU
   reshaping, PQ/HLG handling, tone mapping, gamut mapping, scaling, and final
   SDR/scRGB/HDR10 encoding. QtAVCore has no alternative Windows shader for
   those semantic operations.
3. D3D11VA decoder textures are created shader-readable. `InteropD3D11`
   validates and retains the same-device raw NV12/P010 texture-array slice; it
   performs no Video Processor RGB conversion and no decoded-pixel CPU map,
   transfer, staging copy, or upload.
4. The renderer wraps the retained luma and chroma planes directly with
   libplacebo and maps the `AV_FRAME_DATA_DOVI_METADATA` belonging to that exact
   `VideoFrame` before `pl_render_image()`. Profile 5 is rejected if a hardware
   import cannot preserve that raw-plane contract.
5. Software FFmpeg frames use libplacebo's FFmpeg mapping bridge with Dolby
   mapping enabled. DXGI output/display discovery still selects SDR BGRA8,
   linear FP16 scRGB, or RGB10/PQ targets, but does not implement color
   conversion itself.
6. AD-005 remains binding: render/context acquisition is non-blocking,
   submission uses the shared immediate context, and the exact imported frame
   stays alive through completion-query retirement. AD-007 keeps imported
   hardware frames on fast parameters without a per-frame completion drain.

### Consequences

- Windows and Android now share the same semantic color authority while using
  different libplacebo GPU backends.
- A Dolby Vision Profile 5 frame can be tone-mapped to an SDR target or mapped
  to an active HDR target without first being misinterpreted as conventional
  YCbCr.
- Native D3D11 code remains responsible for device, resource, swap-chain,
  display-capability, clear, and presentation operations; those operations do
  not constitute an alternative color pipeline.
- Dolby licensing, certification, display tunnelling, enhancement-layer
  residual reconstruction, and compressed passthrough remain outside this
  decision.

## AD-007: Windows protects shared D3D11 contexts and imported frames

- Date: 2026-08-03; native-context correction and vendor-neutral asynchronous
  policy accepted across NVIDIA, Intel, and AMD 2026-08-04
- Status: Accepted
- Validation: current protected asynchronous policy accepted on NVIDIA, Intel,
  and AMD; AMD integrated-GPU 4K cadence plus an Intel baseline regression are
  a separate performance follow-up
- Scope: Windows D3D11 immediate-context threading, D3D11VA/libplacebo
  resource completion, and Advanced Color presentation

### Context

After the Windows renderer moved to libplacebo raw-plane sampling, both Dolby
Vision Profile 5 `wednesday.mp4` and ordinary HDR10 `legend.mkv` repeatedly
failed in Intel's D3D11 user-mode driver. Windows Error Reporting identified
`igd10um64xe.dll` version `32.0.101.6733`, exception `0xc0000005`, and module
offset `0x5e56b`. Retaining decoder slices, libplacebo wrappers, borrowed
targets, and D3D11 completion queries did not prevent the failure. Copying the
Dolby Vision decoder array slice GPU-to-GPU into a private single-slice
shader-resource texture also reproduced the same crash. `legend.mkv` then
proved that the trigger was not Dolby-Vision-specific.

The complete imported-frame workaround subsequently stabilized the same path
on an AMD Radeon 880M, and playback was manually verified as normal on the
tested Intel and AMD adapters. On an NVIDIA GeForce RTX 3050, however, the
same build either crashed in `nvwgf2umx.dll` or left a high-priority NVIDIA
driver thread spinning while playback and shutdown remained blocked. The
fault was not codec- or color-specific: generated H.264/NV12, HDR10/P010
`legend.mkv`, and Dolby Vision Profile 5 `wednesday.mp4` all reproduced it.

Controlled A/B builds showed that disabling fast render parameters did not
change the access violation; disabling the Dolby decoder-surface copy only
delayed the failure into a hang; removing `pl_gpu_finish()` changed the early
access violation into an unfinished first-frame submission; and forcing an
SDR BGRA8 target still crashed. The common condition was the output-owned
D3D11 immediate context shared between FFmpeg decode workers and the
libplacebo render worker. Enabling the context's native multithread protection
before constructing `D3D11DeviceAccess` eliminated the NVIDIA failure while
retaining D3D11VA, raw-plane color processing, and zero decoded-source CPU
transfer.

Separately, FP16 scRGB readback produced the expected Windows absolute
luminance and the compositor reported an active HDR layer, yet both files
looked dim beside MPC-BE on the same display. An HDR-active diagnostic is not a
visual brightness acceptance test, and this WinUI composition surface does not
need alpha blending.

### Decision

1. `D3D11VideoOutputOptions` exposes `D3D11HdrPresentationMode`. QtAVCore keeps
   FP16 scRGB as its general-purpose default, while an opaque video host may
   select RGB10/PQ with `DXGI_ALPHA_MODE_IGNORE`.
2. The WinUI 3 player is an opaque video host and selects RGB10/PQ. libplacebo
   remains the color authority and encodes the target as BT.2020/PQ; the
   application does not add another tone-mapping or transfer pass.
3. Every successfully submitted imported D3D11VA hardware frame remains alive
   with its libplacebo wrappers and target through the bounded completion-query
   queue. Successful per-frame submission does not call `pl_gpu_finish()`;
   explicit drains remain in flush, resize, media replacement, failure
   cleanup, and teardown paths.
4. Software frames and hardware frames that take the explicit software-mapping
   fallback retain libplacebo's default parameters and the same bounded
   asynchronous GPU-completion model.
5. Imported hardware frames use libplacebo's fast parameters without the
   optional GPU histogram peak-detection pass. Dolby Vision raw NV12/P010
   imports sample the retained decoder array slice directly and do not create
   a same-device decoder-surface copy.
6. This asynchronous policy is vendor-neutral and accepted on NVIDIA, Intel,
   and AMD. A future synchronization or driver-failure regression may reopen
   the policy for a narrowly evidenced fallback; a cadence or throughput issue
   remains a separate performance investigation and does not justify silently
   weakening native context protection.
7. `D3D11DeviceAccess::create()` enables the immediate context's native
   multithread protection and rejects a context that cannot expose or enable
   it. The existing recursive guard remains required around application-owned
   immediate-context calls; native protection covers context calls made inside
   FFmpeg and libplacebo before user-mode-driver dispatch.

A source audit of the accepted implementation confirms the boundary of this
decision. Of the former three imported-frame workarounds, only fast render
parameters remain. Successful imported-frame submission has no per-frame
`pl_gpu_finish()`, Dolby Vision wraps the retained decoder texture and selected
array slice directly, and there is no adapter-vendor branch. The retained
`decoder-copies` diagnostic therefore has no increment path and reports zero.
Native immediate-context multithread protection is a separate required fix;
the recursive context guard, bounded completion-query lifetime retention, and
explicit failure/flush/teardown drains also remain as general correctness and
lifecycle mechanisms rather than per-import workarounds.

### Consequences

- Imported D3D11VA paths avoid the former per-frame GPU-wide completion wait
  and Dolby copy while retaining fast parameters and bounded source lifetime.
  Ordinary HDR10 sustained source rate on the recorded NVIDIA host; Dolby
  Vision remains more expensive and can show scene-dependent dips.
- The protected asynchronous path is accepted across NVIDIA, Intel, and AMD,
  closing the original crash/correctness scope. The user's separate report of
  visually dropped 4K frames on an AMD integrated GPU requires objective
  cadence and stage timing before it is attributed to the renderer. That
  performance investigation also requires an Intel regression from the same
  build and workloads before closure.
- RGB10/PQ avoids the observed scRGB/DWM brightness mismatch for this opaque
  surface but cannot provide premultiplied-alpha video composition.
- `colorInfo()` remains useful evidence for format, color space, SDR white,
  and display peak, but perceived brightness still requires comparison with a
  trusted player on the same monitor.
- A future vendor-driver retest must isolate the synchronization change from
  the RGB10/PQ presentation decision. Removing `pl_gpu_finish()` cannot be
  used to evaluate brightness, and changing output encoding cannot be used to
  evaluate the driver crash.

### Windows validation

Current manual acceptance is explicit: the former full imported-frame
workaround was accepted on the earlier Intel and AMD adapters, the exact
protected asynchronous direct decoder-surface policy was measured on the
recorded NVIDIA adapter, and the user subsequently confirmed that the same
current modification is usable on Intel and AMD platforms. AD-007 is therefore
closed across all three vendors.

The failing adapter was Intel Iris Xe (`PCI\VEN_8086&DEV_A7A0`), driver
`32.0.101.6733`. With Intel-wide imported-frame completion and native RGB10/PQ
presentation:

- `legend.mkv` reached 01:12 with 25 fps scheduled and mostly 23.3-24.4 fps
  rendered;
- `wednesday.mp4` reached 02:05 with 24 fps scheduled, mostly 21-23.5 fps
  rendered, and a scene-dependent low near 16 fps before recovery;
- both applications closed normally and no new QtAV Application Error event
  was recorded after the corrected build started testing;
- the user compared both output paths with MPC-BE on the same HDR display and
  confirmed matching brightness;
- the same full workaround was subsequently exercised on an AMD Radeon 880M.
  Without it, both representative files and a generated H.264/NV12 clip
  failed in `amdxx64.dll`. With it, six alternating cold starts, 120-second
  and 90-second continuous runs, and 20 combined seeks completed without
  software decoding or decoded-frame CPU mapping;
- the vendor-neutral VS 2026 Release build completed and all 36 registered
  CTest tests passed. On the AMD host, fresh 15-second debugger observations
  of both files retained D3D11VA decode and completed without a crash;
  `wednesday.mp4` also reported active decoder-surface copies.

The NVIDIA validation host ran Windows 10 Enterprise 25H2 build
`26200.8246` with a GeForce RTX 3050
(`PCI\VEN_10DE&DEV_2584&SUBSYS_184610DE`, driver `32.0.15.9186`, NVIDIA
591.86). Before native context protection, Windows Error Reporting recorded
five QtAV failures in `nvwgf2umx.dll` at exception `0xc0000005` and offset
`0x59f589`; the ordinary HDR path also produced a full dump with an NVIDIA
UMD thread spinning and shutdown blocked. Debug logs retained D3D11VA and raw
NV12/P010 import, with no software decode or decoded-source CPU map.

After native context protection:

- a generated 1920x1080 H.264/NV12 clip rendered 900 frames through D3D11VA
  to natural end and closed cleanly; the unprotected control crashed at the
  same NVIDIA offset after about 22 seconds;
- `wednesday.mp4` sustained 45 seconds at about 23-24 rendered fps with active
  decoder-surface copies, then completed four seeks to 10:00, 30:00, 02:00,
  and 50:00 in a separate run and closed cleanly;
- `legend.mkv` sustained 45 seconds at about 24.4-24.6 rendered fps with
  active RGB10/PQ output and closed cleanly;
- one process completed `legend.mkv` -> `wednesday.mp4` -> `legend.mkv` media
  replacement and shutdown without a driver or application failure.
- the updated ClangCL/Visual Studio 2026 shared and static Release builds each
  completed with all 36 registered Windows CTest tests passing, including
  native H.264/AAC A/V, D3D11VA lifecycle, Main10/P010 zero-copy interop,
  high-level composition, and the native multithread-protection contract.

A follow-up NVIDIA-only control retained native context protection while
simultaneously disabling all three earlier imported-frame workarounds: it used
libplacebo default parameters instead of fast parameters, sampled the Dolby
Vision decoder array slice directly instead of copying it, and omitted the
per-import `pl_gpu_finish()`. On the same RTX 3050/591.86 host:

- the generated H.264/NV12 clip rendered 900 frames to natural end at about
  29.4-29.8 fps and closed cleanly;
- `legend.mkv` sustained 60 seconds at about 24.1-24.9 rendered fps with
  D3D11VA and RGB10/PQ, then closed cleanly;
- `wednesday.mp4` sustained 60 seconds at about 23.2-24.1 rendered fps with
  D3D11VA, RGB10/PQ, and `decoder-copies=0`, then closed cleanly;
- one additional process completed four seeks split across both files, two
  media replacements, and close while playing without a driver/application
  failure or software decode.

This establishes that native context protection is sufficient for correctness
on this NVIDIA configuration and that the three earlier workarounds are not
required there. It motivated the explicit vendor-neutral policy above: retain
fast parameters for performance and remove the Dolby copy and successful
per-import finish. The all-disabled control used default libplacebo parameters,
which increased warm steady-state `legend.mkv` draw time from roughly 17-19 ms
to 36-40 ms on this host despite retaining source rate; the current policy
therefore keeps fast parameters.

The exact retained-fast policy was then rebuilt and exercised on the same
NVIDIA host. Both ClangCL/Visual Studio 2026 shared and static Release trees
passed all 36 registered CTest tests. In one WinUI process:

- `legend.mkv` retained D3D11VA and active RGB10/PQ, sustained about
  24.2-25.0 rendered fps after startup with warm draw maxima around
  12.2-13.7 ms, and kept `decoder-copies=0`;
- `wednesday.mp4` retained D3D11VA and active RGB10/PQ, sustained about
  22.9-23.8 rendered fps after startup with warm draw maxima around
  11.9-14.7 ms, kept `decoder-copies=0`, and recovered from a seek to 26:09;
- the generated H.264/NV12 control rendered all 900 frames to natural end at
  about 29.5-29.8 fps through D3D11VA with `decoder-copies=0`;
- three media replacements completed, and a final separate P5 run closed the
  main window while playing. The process exited within the three-second
  observation window, and Windows recorded no new QtAV Application Error or
  Error Reporting event during the run.

This accepts the exact current policy on the recorded NVIDIA configuration.
The user then validated the supplied modification on separate Intel and AMD
platforms and reported it usable, closing the cross-vendor AD-007 gate. The
adapter/driver details and objective cadence from those final two runs were not
supplied in this handoff, so that confirmation is a correctness acceptance,
not a performance benchmark.

The later AMD cadence trace and generic scheduling correction are recorded in
AD-008. They do not reopen AD-007: native context protection, asynchronous
submission, direct decoder-slice sampling, and bounded GPU lifetime retention
remain unchanged.

## AD-008: Windows retries transient rendering with a bounded D3D11 handoff

- Date: 2026-08-04
- Amended: 2026-08-08 to carry transient busy reasons in render results and
  make statistics optional
- Status: Accepted
- Scope: Core render snapshots, Windows D3D11 context scheduling, composition
  output retry semantics, and cadence diagnostics

### Context

On the recorded Radeon 880M, both 4K HDR10 and Dolby Vision Profile 5 scheduled
at source rate, used D3D11VA with no decoded-source copy or CPU map, stayed
below their renderer-stage budgets, and reported no `Present()` backpressure.
They still lost one to nine output frames in representative five-second
windows. The first failing boundary was between Player's render request and a
successful backend draw.

The compatibility `Player::renderVideo()` returned one negative value for no
frame, Player state-lock contention, and a renderer that temporarily declined
the frame. `D3D11VideoOutput` counted that value as skipped and did not retry.
A diagnostic blocking Player lock removed nearly all loss but violated the
non-blocking native-render contract. Atomically publishing Player render state
removed that collision, then reason-level diagnostics exposed the remaining
owner: FFmpeg's reservation-aware D3D11VA work could release and immediately
reacquire the recursive immediate-context lock before a timer-only retry.

### Decision

1. Add `Player::renderVideoDetailed()` and `VideoRenderResult`. Each attempt
   reports `Rendered`, `NoFrame`, `PlayerStateBusy`, or `RendererBusy` plus a
   monotonically assigned frame sequence and presentation generation. Keep
   `renderVideo()` as a source-compatible wrapper that returns a timestamp only
   for `Rendered`.
2. Publish immutable current-frame and render-binding snapshots with C++17
   atomic shared-pointer operations. The render hot path does not acquire the
   Player control mutex. Recheck presentation generation after the backend
   call so seek, stop, or media replacement rejects a stale completion.
3. Let `D3D11VideoOutput` retain only one latest retryable frame. A later frame
   supersedes the pending sequence; detach, stop, or a connection-generation
   change cancels it. `NoFrame` is not timer-retried, and a backend failure with
   no structured transient lock/capacity reason is terminal.
4. Preserve recursive immediate-context serialization and the required native
   `ID3D10Multithread` protection, but make FFmpeg/internal acquisitions honor
   render-thread reservations. A reservation owns no D3D11 context and does not
   block ordinary public `contextGuard()` users. Establish it before each
   output pass makes its first non-blocking context attempt, so a contended
   pass enters `tryContextGuardFor()` for an at-most-8-ms handoff without first
   yielding priority back to FFmpeg. Failures return to bounded
   1/2/4/8/16-ms timer backoff without busy spinning.
5. Hold the acquired handoff guard only through the renderer call, never
   through retry classification, statistics reads, or `Present()`. All waits
   run on the private output thread, not the WinUI dispatcher or a Player
   worker.
6. Carry renderer state/serialization/context/in-flight contention and the
   reservation-aware versus unreserved owner class in
   `VideoRenderRetryReason`, independently of optional statistics. Report
   handoff wait/timeout, retry wakeup, supersession, and terminal drop
   separately.
   Retain `skippedRenders` as a compatibility mirror of terminal drops; a
   recovered retry is not a skipped frame.

### Consequences

- Native/UI render integrations can distinguish retryable contention from the
  absence of a current frame without blocking on Player control work.
- The high-level Windows output can recover a transient collision without a
  second decoder callback, but never accumulates an unbounded retry queue.
- FFmpeg decode cannot overtake a contended output pass or its reserved retry.
  Unrelated public D3D11 users keep the pre-existing blocking/non-blocking
  guard behavior, avoiding a reservation-induced graphics/context lock
  inversion.
- The new public result and Windows statistics are additive API. Existing code
  using `renderVideo()` and `skippedRenders` continues to compile, with the
  clarified terminal-drop meaning for the latter.
- D3D11 statistics use `Off`, `Counters`, or `Timing`. The high-level output no
  longer reads, clears, and re-aggregates renderer atomics after every frame;
  retry behavior remains identical when statistics are off.
- The correction was subsequently validated across the recorded Intel,
  NVIDIA, and AMD policy matrices in the
  [frozen plan history](PLAN_HISTORY_2026-08-10.md).

### Windows validation

The final shared Release WinUI build ran on the documented Radeon 880M,
Windows HDR display, and RGB10/PQ output. `wednesday.mp4` sustained about
23.8-24.1 presented fps for roughly 55 seconds. Every settled window reported
zero superseded and terminal frames; one to ten context collisions per window
were all reservation-aware, and every handoff completed without timeout.

The same process replaced the media with `legend.mkv`. The transition reported
one expected `NoFrame` attempt for the new presentation generation, followed
by zero superseded and terminal frames. Settled windows recorded four to
fourteen reservation-aware context collisions, zero handoff timeouts, zero
Player-busy attempts, zero `Present()` busy results, and zero decoder copies.
Both files retained D3D11VA hardware decoding and direct decoder-slice
rendering. Targeted Player-generation, D3D11 device-access fairness/timed-wait,
and composition-output retry/supersession tests provide deterministic coverage
for the new contracts.

A later high-load Release check exposed a narrower scheduling window: because
the reservation was created only after the first failed renderer acquisition,
`legend.mkv` could still supersede one or two pending frames in a five-second
window. Establishing the reservation before each output pass's initial
non-blocking acquisition eliminated renderer-busy, superseded, and terminal
counts for both supplied files; all intercepted handoffs completed within the
same 8-ms bound. The composition-output test then passed twenty consecutive
runs. That observation also recorded a separate, non-accepted throughput state
with 57-75 ms libplacebo draw maxima and later audio underruns after prolonged
build/UI-capture load. After an idle interval, a fresh process restored
`legend.mkv` to 24.9-25.1 fps and `wednesday.mp4` to 23.8-24.1 fps with zero
steady coalescing, busy, superseded, or terminal counts; warm draw maxima were
about 35-43 ms. The
[frozen plan history](PLAN_HISTORY_2026-08-10.md) therefore records the
high-load result as an environmental caution; the active
[`PLAN.md`](PLAN.md) keeps only the Intel performance comparison open.

## AD-009: OHOS external-format guessing is a bounded workaround

- Date: 2026-08-06
- Amended: 2026-08-08 after Huawei's production-policy reply
- Status: Accepted
- Scope: OHCodec `OH_NativeBuffer` import, Vulkan multi-planar formats, and
  libplacebo source wrapping on OHOS

### Context

Surface-mode OHCodec output was presented into a private
`OH_ConsumerSurface`, acquired as the exact retained
`OHNativeWindowBuffer`/`OH_NativeBuffer`, and queried with
`vkGetNativeBufferPropertiesOHOS()`. On the connected Mate 60 Pro/Maleoon 910,
the query returned `VK_SUCCESS` but reported
`VkNativeBufferFormatPropertiesOHOS::format = VK_FORMAT_UNDEFINED`.

The observed buffers were:

```text
H.264/NV12:
  nativeFormat   = NATIVEBUFFER_PIXEL_FMT_YCBCR_420_SP (24)
  format         = VK_FORMAT_UNDEFINED
  externalFormat = 1000156003

HEVC Main10/P010:
  nativeFormat   = NATIVEBUFFER_PIXEL_FMT_YCBCR_P010 (35)
  format         = VK_FORMAT_UNDEFINED
  externalFormat = 1000156013
```

The external IDs are numerically equal to
`VK_FORMAT_G8_B8R8_2PLANE_420_UNORM` and
`VK_FORMAT_G10X6_B10X6R10X6_2PLANE_420_UNORM_3PACK16`, respectively, but
`externalFormat` is an implementation-defined identifier intended for
`VkExternalFormatOHOS`; numerical equality alone is not a portable Vulkan
format contract.

Three connected-device probes separated hardware consumption from interface
representation:

1. The standard opaque path used `VK_FORMAT_UNDEFINED +
   VkExternalFormatOHOS`. Vulkan object creation, NativeBuffer memory import,
   shader sampling, queue submission, and completion all succeeded.
2. A diagnostic path omitted `VkExternalFormatOHOS`, guessed the explicit
   NV12/P010 format from the two allowlisted external IDs, and used that format
   in `VkImageCreateInfo`. Application-owned Vulkan sampling succeeded for 30
   NV12 and 30 P010 frames.
3. The same guessed explicit images were passed to libplacebo 7.351.0.
   `pl_vulkan_wrap()` created the direct Y/UV planes and rendered all 60
   frames with no RGBA source-normalization intermediate.

All paths reported zero decoded-source CPU map, software transfer, staging,
or upload. These results prove current-device capability and libplacebo's
acceptance of the guessed explicit formats. They do not prove that Huawei
supports the numerical mapping across devices, GPUs, system releases, buffer
modifiers, compression modes, usages, dataspaces, or HDR configurations.

### Decision

1. A successful `vkGetNativeBufferPropertiesOHOS()` call with an explicit
   supported `format` remains the standard direct-plane path. That queried
   `VkFormat` may be passed to libplacebo subject to normal feature, import,
   synchronization, and lifetime validation.
2. A successful query with `format == VK_FORMAT_UNDEFINED` does not mean that
   the NativeBuffer is unconsumable. The standards-based fallback imports it
   with `VkExternalFormatOHOS` and samples it through
   `VkSamplerYcbcrConversion`. When needed, a GPU-only representation-
   normalization pass feeds libplacebo. This is zero-CPU-copy, but it is not
   strict raw-plane/no-intermediate zero-copy.
3. Huawei confirmed that the workaround may be enabled by default in
   production, warned that formats beyond NV12/P010 may exhibit the same
   `VK_FORMAT_UNDEFINED` report, and required application-visible controls and
   fallback while its Vulkan driver continues to evolve.
4. The production path therefore recognizes a closed allow-list of standard
   Vulkan packed and multi-planar YCbCr external IDs across 8/10/12/16-bit,
   4:2:0/4:2:2/4:4:4, and two-/three-plane families. It never casts an
   arbitrary external ID. The OH native format must still be an accepted video
   YUV family, and dimensions, texture usage, explicit-format sampled-image
   support, sampler conversion, image creation, NativeBuffer memory
   import/bind, image view, queue synchronization, and actual sampling remain
   runtime gates.
5. `OHCodecVulkanInteropConfig::externalFormatWorkaroundEnabled` defaults to
   `true`. Setting it to `false` uses the standards-based opaque
   `VkExternalFormatOHOS` path and gives applications the required user-facing
   kill switch. The production workaround uses explicit-format Vulkan YCbCr
   sampling followed by the existing GPU normalization pass; the direct
   libplacebo plane mode remains separately diagnosable.
6. A workaround mapping or Vulkan import/sampling failure is fatal to the
   current Vulkan candidate. `MobileVideoRendererSelector` then prepares
   OpenGL ES. Its synchronous application callback first rebinds future
   OHCodec output to OpenGL ES interop; if that hardware interop subsequently
   fails, the callback is invoked again so the application can disable
   hardware decode and continue with software frames. No frame from a retired
   native surface is retried or CPU-mapped.
7. `MobileRendererSelectorConfig::preferredAPI` defaults to Vulkan and may be
   set to `OpenGLES`, providing the required user-facing preference. This is a
   startup preference, not a permanent ban: failure to open the preferred API
   tries the other configured candidate.
8. This workaround remains an OHOS/Huawei compatibility policy, not a portable
   Vulkan guarantee. Strict raw-plane/no-intermediate use, especially the P010
   raw 10-bit guarantee required before Dolby Vision reshaping, remains a
   separate validation gate.

### Consequences

- `VK_FORMAT_UNDEFINED` is no longer treated as evidence of hardware failure.
- The opaque external-format path remains the correctness fallback and keeps
  decoded pixels off the CPU, while its GPU intermediate and converted sample
  semantics remain explicit.
- The bounded compatibility path is production-default, but its allow-list,
  object-creation gates, fallback chain, and application kill switch prevent
  unknown or newly broken external IDs from silently being reinterpreted.
- libplacebo support is no longer the open question: it accepts the evidenced
  explicit NV12/P010 formats. Broader allow-listed formats remain individually
  gated by the actual Vulkan driver operations on each device.
- Production use of the workaround is no longer vendor-confirmation-gated;
  strict Vulkan raw-plane/Dolby Vision claims remain separately gated.

### Current validation evidence

On the Mate 60 Pro, both the application-owned forced-format sampler and the
direct libplacebo path rendered 30 H.264/NV12 plus 30 HEVC Main10/P010 frames.
The libplacebo run reported `directPlanes=60`, `normalization=0`, and zero
decoded-source CPU map, transfer, staging, or upload. The then-default
disabled-workaround build also passed the complete connected OHOS regression,
including opaque Vulkan sampling and Vulkan-to-OpenGL/software fallbacks.

The result was independently reproduced on 2026-08-08 with a HUAWEI Pura X
Max (`HOP-AL00`) running HarmonyOS 6.1.0.135 SP17 / API 24. The standard query
still returned `VK_FORMAT_UNDEFINED` plus external format `1000156003` for
`NATIVEBUFFER_PIXEL_FMT_YCBCR_420_SP` and `1000156013` for
`NATIVEBUFFER_PIXEL_FMT_YCBCR_P010`. Opaque import and sampling passed 60/60
frames. The separately enabled forced-format/libplacebo diagnostic also
passed 60/60 frames with `directPlanes=60`, `normalization=0`, and no decoded-
source CPU map, transfer, staging, or upload. Huawei's later reply permits the
bounded workaround as a production default while explicitly requiring
broader-format handling, Vulkan-to-OpenGL/software fallback, a workaround kill
switch, and an OpenGL ES startup preference. The amended decision implements
those constraints without describing the mapping as a portable Vulkan
contract. The resulting signed production-policy HAP subsequently passed the
same Pura X Max: 60/60 frames used the default workaround and GPU normalization,
an injected Vulkan failure switched from 8 Vulkan frames to 30 OpenGL ES native-
interop frames, and an independent session continued with 30 software-rendered
frames after disabling OHCodec. Every route reported zero decoded-source CPU
map, transfer, staging, or upload.

## AD-010: Windows copies the visible decoder region by default

- Date: 2026-08-06
- Amended: 2026-08-07 to retain imported source frames through GPU completion
  and release them on the bounded recycler
- Status: Accepted and complete; the native cross-vendor policy matrix passed,
  and the user waived repeating it after diagnostic-only probe cleanup
- Scope: Windows D3D11VA decoder-surface lifetime, libplacebo source wrapping,
  post-seek cadence, deterministic shutdown, and direct-sampling policy
- Supersedes: AD-006 item 4 and AD-007's default direct decoder-slice sampling;
  native multithread protection, fast parameters, asynchronous submission, and
  bounded completion lifetime remain accepted

### Context

Commit `bfdcf07` fixed an Intel Iris Xe performance regression by caching the
RTV and two decoder-plane SRVs created by `pl_d3d11_wrap()`. That removed per-
frame view destruction from the D3D11 runtime deletion pool. On the recorded
NVIDIA RTX 3050, however, `legend.mkv` could settle at 25 fps after a seek to
22:48 and then stop permanently. The video decode worker was mapped exactly to
FFmpeg's `ff_dxva2_common_end_frame()` call to
`ID3D11VideoContext::DecoderBeginFrame`, ending in the NVIDIA user-mode
driver's CPU wait. Because decode still owned the reservation-aware shared
immediate-context lock, the output worker accumulated handoff timeouts, audio
entered buffering, and ordered WinUI shutdown waited for the blocked decoder.

Repair-oriented A/B tests isolated resource-view lifetime:

1. Increasing FFmpeg's extra D3D11VA surfaces from 4 to 32 delayed reuse but
   still reached persistent context timeouts.
2. Draining renderer state during seek initially recovered, then failed again
   as the active decoder slices continued to cycle.
3. Returning decoder-plane wrappers to transient completion-query lifetime
   sustained the exact seek scene for 90 seconds and closed in 128 ms.
4. Restoring the persistent wrappers and calling D3D11.1 `DiscardView` after
   completion reproduced the permanent stall after about 28 seconds.

The root cause is retained shader views on reusable decoder-array slices, not
seek queue flushing, surface count, stale pixels, `Present()` backpressure, or
Player shutdown ordering. The close hang is downstream: stopping correctly
waits for a decoder operation that cannot be interrupted after it enters the
driver.

The legacy QtAV path does not shader-sample decoder output directly; it feeds
an `ID3D11VideoProcessorInputView` into a separate output texture. mpv also
defaults to a GPU-to-GPU copy from a D3D11VA decoder surface to a shader
resource, copying only the even-aligned visible rectangle with
`CopySubresourceRegion1()` and `D3D11_COPY_DISCARD`. mpv exposes direct decoder
sampling only as an opt-in and warns that D3D11 does not guarantee this use.

### Decision

1. Decoder arrays do not request `D3D11_BIND_SHADER_RESOURCE` by default.
   `InteropD3D11` validates and retains the exact same-device raw NV12/P010
   decoder slice without submitting a conversion.
2. `RenderD3D11` copies only the even-aligned visible rectangle into a bounded
   three-entry, same-format, shader-readable NV12/P010 ring. D3D11.1 uses
   `CopySubresourceRegion1()` with `D3D11_COPY_DISCARD`; a compatibility context
   uses `CopySubresourceRegion()` with the identical box.
3. Persistent libplacebo plane wrappers belong only to ordinary ring textures.
   The decoder frame and imported interop wrapper remain alive with the selected
   ring entry, borrowed target, and completion query until GPU completion. The
   renderer then destroys its GPU wrappers and transfers the source frame plus
   interop reference to a bounded release worker, keeping final FFmpeg/driver
   destruction off the output render thread.
4. The copy and libplacebo draw remain asynchronous on the natively protected,
   reservation-aware immediate context. Successful frames retain fast render
   parameters and add no per-frame `pl_gpu_finish()`.
5. Raw luma/chroma values and the exact FFmpeg Dolby Vision RPU reach
   libplacebo before semantic color conversion. No CPU map, upload, Video
   Processor RGB conversion, or cross-device copy is introduced.
6. Direct decoder-texture sampling remains available only when both decoder and
   renderer opt in. It requests shader-readable decoder arrays, uses transient
   decoder-plane wrappers, retains the decoder frame through completion, and
   reports zero `decoderSurfaceCopies`. The high-level output configures both
   ends with one option.
7. `HardwareInteropCapabilities::zeroCopy` continues to describe that the
   interop can expose a direct path; it does not mean the renderer selected it.
   `decoderSurfaceCopies` is the runtime evidence: it increments once per
   submitted raw frame in the default policy and remains zero in direct mode.

### Consequences

- The default avoids shader views on decoder-array slices. It still retains the
  source slice until the ordered copy and draw complete, then releases the
  interop wrapper before the source frame on the dedicated recycler.
- The cost is one device-local raw visible-region copy per submitted hardware
  frame. Decoder padding is not copied and the discard flag permits the driver
  to avoid preserving prior destination contents.
- Intel keeps persistent luma/chroma views on a three-texture ordinary-resource
  ring, so the repair does not restore per-frame SRV creation/destruction churn.
- Direct mode is useful for controlled performance A/B testing, but is not
  automatically selected by vendor and retains its documented seek, shutdown,
  padding, and driver-compatibility risk.
- Static/shared CTest and WinUI Release remain build gates. The native policy
  matrix passed on the same Release revision on NVIDIA and AMD with
  `directDecoderTextureSampling` both off and on, using only `legend.mkv`, as
  recorded in the [frozen plan history](PLAN_HISTORY_2026-08-10.md). On
  2026-08-08 the user waived repeating those cells after removal of
  investigation-only probes because that cleanup changed only diagnostics and
  their presentation, not playback behavior.

### Primary references

- [Microsoft D3D11 decoder surface allocation and release](https://learn.microsoft.com/en-us/windows/win32/medfound/supporting-direct3d-11-video-decoding-in-media-foundation)
- [Microsoft `CopySubresourceRegion1`](https://learn.microsoft.com/en-us/windows/win32/api/d3d11_1/nf-d3d11_1-id3d11devicecontext1-copysubresourceregion1)
- [Microsoft D3D11 copy flags](https://learn.microsoft.com/en-us/windows/win32/api/d3d11_1/ne-d3d11_1-d3d11_copy_flags)
- [mpv D3D11VA hardware interop](https://github.com/mpv-player/mpv/blob/master/video/out/d3d11/hwdec_d3d11va.c)
- [mpv `d3d11va-zero-copy` option](https://github.com/mpv-player/mpv/blob/master/DOCS/man/options.rst#d3d11va-zero-copyyesno)

## AD-011: Windows reuses compatible D3D11VA frames contexts and decoders

- Date: 2026-08-07
- Status: Accepted and complete; the NVIDIA/AMD sampling-policy regression
  passed, and its post-probe-cleanup repetition was waived by the user
- Scope: Core FFmpeg hardware-format selection, Windows D3D11VA decoder
  lifetime, the repository FFmpeg overlay ABI, and shared-device teardown
- Extends: AD-010 resource lifetime; the dependency-side decision and patch
  retirement contract are recorded in
  [FFmpeg FD-005](../ffmpeg/DECISIONS.md#fd-005-reuse-a-compatible-d3d11va-decoder-with-its-frames-context)

### Context

HEVC may invoke `get_format` again after an SPS update even when the selected
D3D11 device, D3D11/NV12-or-P010 formats, dimensions, and required fixed pool
remain unchanged. Stock FFmpeg 8.1.2 uninitializes the hardware accelerator at
that boundary. Reinstalling the same initialized `AVHWFramesContext` preserves
the texture array, but stock libavcodec still creates a new
`ID3D11VideoDecoder` and every decoder output view.

Each outstanding frame retains the previous libavcodec decoder. Intel Iris Xe
ETW showed the last old-frame release entering Intel DXVA allocation teardown
from the renderer's frame recycler and serializing the shared D3D11 device.
libplacebo GPU work remained below one millisecond; thermal load amplified the
stall but did not create the redundant decoder lifetime. Moving the release to
another thread protected the render thread from ordinary destruction cost, but
could not prevent a driver-wide shared-device serialization.

### Decision

1. During D3D11 hardware pixel-format selection, Player obtains FFmpeg's
   required hardware-frames parameters and retains one successfully initialized
   frames context on the active video decoder.
2. Player reuses that context only when the native hardware-device identity,
   hardware and software pixel formats, allocation width and height, and fixed
   pool capacity remain compatible. The retained pool must satisfy FFmpeg's
   requested pool plus its three automatic fixed-pool surfaces. Any mismatch or
   initialization failure follows the ordinary new-context path.
3. `QtAV::HWD3D11VA` enables the repository FFmpeg extension
   `AVD3D11VADeviceContext::reuse_decoder`. The extension binds the compatible
   decoder, all output views, and the texture reference to the frames context;
   FFmpeg additionally compares the texture, array size, decoder profile,
   sample dimensions, output format, and complete decoder configuration before
   reuse.
4. The retained frames context belongs to the active Core decoder and is
   released on decoder reset, media replacement, or shutdown. No FFmpeg or
   D3D11 type is exposed through the public Core API.
5. Renderer lifetime remains a separate responsibility. Both default GPU-copy
   and direct-sampling modes retain imported frames through GPU completion.
   Final imported-wrapper and source-frame destruction runs on the renderer's
   fixed-capacity recycler; flush and teardown drain it synchronously.

### Rejected alternatives

- Reusing only the frames context leaves stock libavcodec's decoder and output
  views on the redundant create/destroy path and did not remove the Intel
  teardown stack.
- Relying only on the frame recycler moves ordinary destruction off the render
  thread but cannot prevent the driver from serializing the shared device.
- Enabling decoder reuse for every FFmpeg consumer would change upstream
  behavior and could retain an incompatible decoder. The overlay remains an
  explicit QtAVCore opt-in.
- Software decode or a decoded-pixel CPU copy avoids this lifetime by giving up
  the required D3D11VA zero-map pipeline.

### Consequences

- Supported Windows builds must use the paired repository FFmpeg package. Its
  installed headers contain a small overlay ABI that
  `cmake/verify-install.cmake` verifies; an arbitrary stock FFmpeg binary is not
  compatible with this backend build.
- Compatible repeated format selection keeps one texture pool, decoder, and
  output-view set. An incompatible device, format, size, pool requirement,
  profile, texture, or decoder configuration still reconstructs the resources.
- Android and OHOS behavior is unchanged. The reusable-context policy is
  private to Core's D3D11 decoder path and the FFmpeg extension defaults off.
- The renderer recycler remains bounded independently of decoder reuse. If it
  cannot accept another completed frame, normal rendering applies bounded
  in-flight backpressure rather than releasing that frame on the render thread.
- The directly affected Windows FFmpeg build, install verifier, fresh static
  and shared QtAVCore 37/37 CTest runs, WinUI Release build, lifecycle tests,
  and cooled Intel ETW/cadence evidence are recorded in the
  [frozen plan history](PLAN_HISTORY_2026-08-10.md).

### Retirement condition

Retire the overlay only when the pinned upstream FFmpeg provides equivalent
compatible decoder retention or no longer uninitializes unchanged D3D11VA
state. Remove Core's opt-in, rebuild and verify the Windows dependency package,
run static/shared tests, and repeat the cold Intel seek/ETW regression before
adopting the upstream replacement.

## AD-012: Reject a production Windows Vulkan backend

- Date: 2026-08-08
- Status: Rejected
- Scope: Windows video rendering, presentation, hardware-frame interop, HDR,
  packaging, and support policy

### Context

QtAVCore investigated whether its portable libplacebo/Vulkan renderer should
also become an optional Windows output beside the completed D3D11 path. The
prototype separated Win32 surface/presentation ownership, Vulkan rendering,
and D3D11VA-to-Vulkan capability checks; it was never committed or published
as an API. Testing used an Intel Iris Xe with Windows driver 32.0.101.7088 and
an HDR-capable PHL 27B1U7903 selected as the primary display.

The investigation produced these results:

1. Windows Advanced Color was active on the display. The production D3D11
   path passed native HDR validation with 10-bit output, 240-nit system SDR
   white and a reported 417.712-nit peak. D3D11VA also passed native H.264/NV12
   and HEVC Main10/P010 decode and direct D3D11 sampling. Hardware decode was
   therefore available; it was not the limiting capability.
2. Both `vkGetPhysicalDeviceSurfaceFormatsKHR` and the Win32-monitor/full-
   screen-aware `vkGetPhysicalDeviceSurfaceFormats2KHR` query advertised only
   `VK_COLOR_SPACE_SRGB_NONLINEAR_KHR` with RGBA8/BGRA8 UNORM or sRGB formats.
   No HDR10/PQ, HLG, or extended-linear/scRGB surface format was available.
3. `C:\\test\\legend.mkv` passed the PQ-source gate, and
   `C:\\test\\wednesday.mp4` passed the Dolby Vision Profile 5 RPU gate. Both
   correctly failed a required-HDR Vulkan presentation because the selected
   surface exposed no HDR color space. The prototype could present those
   software-decoded sources only through an SDR swapchain.
4. External-image capability probes rejected exact sampled-image import of
   the retained D3D11VA NV12 and P010 decoder textures. A strict Windows
   D3D11VA-to-Vulkan route therefore had neither a proven native-resource
   import contract nor the required HDR presentation contract on this common
   adapter class.
5. `VK_EXT_hdr_metadata` availability did not change the result. Khronos
   defines that extension as metadata submission; it neither selects nor
   overrides the swapchain color space. Intel support also states that
   `VK_EXT_swapchain_colorspace` is not supported on Iris Xe, consistent with
   the native surface-format evidence.

Vulkan defines HDR color spaces and other Windows GPU vendors or future Intel
adapters may expose them. The finding is not that Vulkan is intrinsically
unable to output HDR. It is that Windows Vulkan does not provide the broad,
capability-complete target matrix QtAVCore requires, while the existing D3D11
path already provides hardware decode, zero-CPU-map rendering, SDR/HDR output,
Dolby Vision processing, display switching, and Windows-native validation.

### Decision

1. D3D11 is QtAVCore's only formal, default, and complete Windows graphics
   backend. Windows packages build and export the D3D11 renderer, D3D11VA
   decoder/interop, and D3D11 composition output; they do not build or export a
   Windows Vulkan surface renderer or high-level output.
2. QtAVCore will not implement D3D11VA-to-Vulkan texture import, cross-API
   synchronization, or a Vulkan-render-to-DXGI presentation bridge on Windows.
   A bridge would retain the D3D11 dependency while adding another resource,
   synchronization, device-loss, HDR, and driver test matrix.
3. The portable Vulkan/libplacebo engine remains supported for Android and
   OHOS. Sharing that engine across mobile targets is not a reason to create a
   Windows Vulkan product backend.
4. The uncommitted investigation implementation is discarded. Its negative
   capability results remain recorded here and in the
   [frozen plan history](PLAN_HISTORY_2026-08-10.md); no public target, API,
   example, package export, or compatibility promise survives it.
5. Reconsideration requires a concrete Windows product requirement plus a new
   architecture decision backed by native multi-vendor hardware-decode,
   external-memory/synchronization, SDR/HDR, lifecycle, and packaging evidence.
   General API symmetry or speculative future driver support is insufficient.

### Consequences

- Windows development and regression coverage remain concentrated on the
  production D3D11/DXGI path used by Intel generations that remain in the
  supported installed base.
- QtAVCore avoids a second Windows presentation stack whose useful capability
  was below the existing backend on the tested adapter.
- Applications requiring a Vulkan-owned Windows compositor are outside the
  supported QtAVCore Windows output contract. They may still consume core
  decoded software frames through application-owned integration, without a
  QtAVCore Windows Vulkan backend claim.

### Primary references

- [Khronos `VkColorSpaceKHR`](https://registry.khronos.org/VulkanSC/specs/1.0-extensions/man/html/VkColorSpaceKHR.html)
- [Khronos `VK_EXT_hdr_metadata`](https://registry.khronos.org/VulkanSC/specs/1.0-extensions/man/html/VK_EXT_hdr_metadata.html)
- [Khronos full-screen-exclusive presentation query](https://registry.khronos.org/vulkan/specs/latest/man/html/VkFullScreenExclusiveEXT.html)
- [Intel Iris Xe Vulkan extension support response](https://community.intel.com/t5/Graphics/Vulkan-extensions-support-request/m-p/1688246)
- [Intel 11th-14th generation Windows graphics driver](https://www.intel.com/content/www/us/en/download/864990/intel-11th-14th-gen-processor-graphics-windows.html)
- [Microsoft DirectX Advanced Color](https://learn.microsoft.com/en-us/windows/win32/direct3darticles/high-dynamic-range)

## AD-013: Packet buffering gates both decoders by usable media time

- Date: 2026-08-09
- Status: Accepted and complete
- Scope: Core demux/decode backpressure, playback time, buffering status, and
  external-input EOF

### Context

Core already demuxed on its control worker and sent selected compressed audio
and video packets through independent fixed-capacity queues to decoder workers.
Those queues isolated codec backpressure, but consumers started immediately.
A blocked network read could therefore empty one decoder without an explicit
packet-fill target or packet-specific progress, while the generic
`MediaStatus::Buffering` also represented seek output and audio-device clock
re-anchoring.

### Decision

1. Keep FFmpeg input ownership on the playback worker. Do not add a second
   demux thread or duplicate format-context synchronization.
2. Measure each existing compressed queue by normalized media time and bytes.
   During initial play, seek, track switch, or confirmed underflow, hold both
   A/V consumers behind one presentation-generation gate while demux continues.
3. Release only when every selected A/V stream reaches its target or its own
   input reaches EOF. Report the minimum usable stream duration, combined
   bytes, progress, reason, and generation through `PacketBufferStatus`.
4. Bound memory and head-of-line blocking by per-stream time, combined bytes,
   and the existing packet-count limits. If a bound prevents the target from
   being reached, release with `capacityLimited` rather than deadlocking an
   interleaved source behind one full stream.
5. Freeze playback time through the existing output-wait contract until the
   released generation actually produces output. Packet buffering does not
   implement low-latency drops, adaptive bitrate selection, or network-error
   retry policy.

### Consequences

- Local and remote playback share deterministic buffering semantics; callers
  can disable buffering or set either fill target to zero.
- A short external audio sidecar can reach EOF independently without blocking a
  longer primary video stream.
- Underflow detection and packet progress may be published from a decode
  worker. Callbacks retain the existing rule that they may request control
  changes but must not destroy `Player` inline.
- The next live-stream milestone can choose an explicit late/drop strategy on
  top of this reservoir without changing its accounting or conflating packet
  loss with buffering progress.

## AD-014: Optional packet disk cache is a bounded temporary spill tier

- Date: 2026-08-09
- Status: Accepted and complete
- Scope: Core compressed-packet storage, temporary-file lifecycle, public
  buffering policy/status, and explicit cache clearing

### Context

The packet reservoir originally retained FFmpeg packet references only in RAM.
Its public five-second and 32 MiB limits are appropriate defaults, but a caller
may need a longer prefetch window without retaining every compressed payload in
memory. Dropping a queued payload is not safe once demux has advanced, and a
persistent media cache has materially different identity, validation, privacy,
and eviction requirements.

### Decision

1. Preserve `maximumBufferMilliseconds` and `maximumBufferBytes` as independent
   public memory limits with their existing five-second/32 MiB defaults.
2. Add an opt-in `PacketDiskCachePolicy` spill tier, disabled by default, with
   separately configurable per-stream time and combined-byte limits. Its
   disabled defaults are 60 seconds and 256 MiB.
3. Store only compressed packet payloads in one player-specific file below the
   system temporary directory. Keep FFmpeg packet properties in the bounded
   in-process queue, materialize the payload on the owning decode worker, and
   serialize file reads, writes, compaction, and removal internally.
4. Compact still-live records before the configured byte boundary so physical
   growth does not accumulate across long playback. Remove the file and its
   dedicated directory as soon as no disk entries remain and at every normal
   generation reset or player destruction.
5. Report total, memory, and disk compressed bytes plus the volatile path. A
   synchronous clear drains current packet work and re-seeks seekable playback
   from its current position; it refuses an active non-seekable source rather
   than silently skipping removed packets.

### Consequences

- Applications can choose a larger network reservoir while keeping the default
  memory footprint unchanged, and can explicitly remove temporary media data.
- The spill file is transient and not encrypted by QtAVCore. It is not a
  persistent download, content-addressed cache, or offline-playback contract.
- Disk I/O can add decode latency and is intentionally opt-in. File failures
  emit `packet.disk_cache.error` once per generation and playback continues on
  the memory tier; corrupt or externally removed live entries are fatal to the
  affected decode generation.
- Clearing an active cache is a control operation and must not run inline from
  a Player callback because it may wait for decoder workers to drain.

## AD-015: Live latency control drops decoded video, not packet history

- Date: 2026-08-09
- Status: Accepted and complete
- Scope: Core decoded-video queue pressure, presentation timing, public policy,
  and playback diagnostics

### Context

The ordinary presentation policy retains a contiguous near-term window: when
its hard queue bound is full, it rejects a farther-future decoded frame, and it
drops a frame more than 250 ms late only when a timely replacement is already
queued. That favors continuity for files and seek preroll, but a slow renderer
or callback can leave live video showing the oldest bounded window instead of
the newest available one. The compressed-packet reservoir cannot safely evict
arbitrary inter-frame video packets without a coordinated decoder/keyframe
generation reset, and dropping audio would break the device-clock contract.

### Decision

1. Add an explicit, disabled-by-default `LivePlaybackPolicy`; do not infer live
   behavior from URL schemes, duration, or FFmpeg seekability.
2. When enabled, cap queued decoded video to the caller-selected depth and keep
   the newest timestamp window. A newer arrival supersedes the oldest queued
   video; a reordered older arrival cannot displace a newer one.
3. Use a caller-selected late threshold. Once the current video is beyond that
   threshold, discard it whenever any newer same-generation video is queued,
   allowing repeated decisions to converge on the newest available frame.
4. Preserve packet buffering, decoder reference history, audio, subtitles,
   device-clock ownership, and playback position. Do not seek or flush merely
   because decoded video presentation is behind.
5. Keep accepted `VideoFrameScheduler` frames outside this policy because the
   application has already assumed direct presentation and output-token
   lifetime. Retain total queue/late counters and add a low-latency queue-drop
   subset for diagnostics.

### Consequences

- File playback remains behaviorally unchanged unless the application opts in.
- Live video can trade visual continuity for a bounded newest-frame latency
  while audio remains continuous and authoritative.
- This policy cannot recover latency already held in a network server, FFmpeg
  demuxer, compressed-packet reservoir, audio device, or an application-owned
  scheduler. AD-016's recoverable-input policy remains separate, and adaptive
  rendition selection remains out of scope.
- Hardware-frame destruction continues through its backend-owned retained
  lifetime, so superseding a queued surface/native-buffer frame does not map or
  copy decoded pixels.

## AD-016: Recover network input in two bounded layers

- Date: 2026-08-09
- Status: Accepted and complete
- Scope: Core network open/read errors, input lifetime, playback generations,
  packet refill, public recovery policy/status, and diagnostics

### Context

QtAVCore already supplied bounded FFmpeg HTTP(S) `rw_timeout` and reconnect
options, but those retries were invisible to applications and a returned
`av_read_frame()` error immediately made the media `Invalid`. The packet
reservoir could mask a short stalled read until queued packets drained, but it
did not own the `AVFormatContext` recovery boundary. Arbitrarily retrying a
failed context is unsafe, while reopening without validating streams or
invalidating decoder work can leave `AVStream*` references dangling and mix
packets from incompatible presentation generations.

### Decision

1. Preserve FFmpeg protocol recovery as the first layer. `avformat.*`
   properties continue to override its HTTP(S) timeout/reconnect options.
2. Add a separate default-enabled `NetworkRecoveryPolicy` for recognized
   network URL schemes after open or read returns a recoverable error. Bound
   it by 1–32 caller-selected fresh-open attempts and capped exponential
   backoff; do not infer adaptive-stream behavior or retry common permanent
   HTTP authorization/not-found errors.
3. Keep every open, demux read, retry wait, and replacement on the existing
   playback worker. Its condition-variable wait must be interruptible by the
   asynchronous control path; do not add a second format-context owner.
4. Before installing a read replacement, require every selected stream from
   that source to retain its stream index, media type, and codec ID. Drain
   in-flight decoder work, invalidate the presentation generation, update all
   decoder `AVStream`/time-base references, and only then close the retired
   format context. Treat the open as provisional until it returns a selected
   non-corrupt packet or clean EOF; retain that packet and charge corrupt
   packets plus an immediate read failure to the same bounded attempt cycle.
5. Seek a seekable replacement to the frozen media position and realign other
   active seekable inputs. For a non-seekable input, resume at the new server
   edge and offset its normalized start time so the public media timeline does
   not restart at zero.
6. Reuse the packet reservoir with
   `PacketBufferingReason::NetworkRecovery` after installation. Freeze the
   output clock until the new generation really produces output; do not count
   protocol-internal retries in Player statistics.
7. Publish `Waiting`, `Reopening`, `Recovered`, `Failed`, and cancellation-to-
   `Idle` snapshots plus attempt/success/failure counters. Exhaustion remains a
   terminal reader/open error and transitions media to `Invalid`. Read failures
   share one attempt budget until the selected demux timeline advances at least
   500 ms; explicit continuity controls reset that progress interval.

### Consequences

- Buffered playback can continue while the playback worker waits or opens a
  replacement; if the reservoir drains, the existing underflow/output-wait
  contract freezes time.
- Recovery never reuses a returned-error context and never leaves decoder
  workers holding a stream pointer owned by the closed context.
- A source that repeatedly opens but cannot return its first relevant packet
  or make 500 ms of forward demux progress cannot create an unbounded chain of
  false-positive recoveries.
- Reopening a seekable compressed stream can repeat keyframe preroll internally,
  but generation and presentation timing prevent retired output from crossing
  the recovery boundary.
- Non-seekable recovery is continuity from a new live edge, not lossless packet
  repair. Adaptive rendition selection, persistent caching, and application-
  level download semantics remain out of scope.
- `onNetworkRecoveryStatus()` follows the playback-worker callback lifetime
  rule: it may request control, but it must not destroy the Player inline.

### Primary reference

- [FFmpeg protocol options](https://ffmpeg.org/ffmpeg-protocols.html#http)

## AD-017: The main input owns the timeline while sidecars share one track namespace

- Date: 2026-08-10
- Status: Accepted and complete
- Scope: Core input ownership, external audio/subtitle sources, track identity,
  timestamp ordering, and playback lifecycle

### Context

Post-load track switching already used each main-input FFmpeg stream index as
the public selector. Adding one external audio input and one external subtitle
input introduced independent format contexts, overlapping stream indices,
different start times, and possibly shorter EOF boundaries. Treating each
sidecar as a separate Player would duplicate clocks and control state; treating
its local stream index as globally unique would select the wrong source. A
sidecar must also not shorten or extend the primary program's range merely
because its duration differs.

### Decision

1. The main input remains the authority for media duration, A-B range, loop,
   seekability, and end-of-media. One optional external audio input and one
   optional external subtitle input may contribute selected tracks.
2. The control worker exclusively owns all active format contexts. It
   timestamp-orders selected packets after normalizing each input against its
   own start time; no second demux owner or cross-context FFmpeg pointer is
   exposed.
3. `MediaInfo::tracks` is one public selector namespace. Main-input
   `TrackInfo::index` values retain their container stream indices. External
   tracks receive non-overlapping selector IDs, while `streamIndex`,
   `sourceUrl`, and `external` preserve their actual diagnostic identity.
4. Track switching, external-source replacement/removal, seek, loop, and main-
   media replacement cross one presentation-generation boundary. Retired
   packets and decoded frames cannot appear after the new selection is
   accepted.
5. Changing a sidecar while media is loaded asynchronously reopens and
   realigns inputs at the current position while preserving play/pause intent.
   A changed audio selection renegotiates the sink format.
6. A short sidecar reaches EOF independently. It neither holds packet
   buffering open nor becomes the program end authority. If the main input has
   no audio or subtitle, the best eligible sidecar track may be selected
   automatically; otherwise main-input best-stream selection keeps priority.

### Consequences

- Applications use the same `setActiveTrack()` contract for main and external
  tracks without learning FFmpeg format-context ownership.
- Runtime sidecar replacement is a bounded asynchronous continuity operation,
  not an in-place mutation of decoder-owned stream pointers.
- The single-timeline model does not provide arbitrary numbers of external
  sources, independent sidecar clocks, playlist concatenation, or sidecar-
  defined playback duration.
- Packet buffering and network recovery must evaluate EOF and replacement
  compatibility per input while still publishing one presentation generation.

## AD-018: Accurate seek and frame stepping use presentation generations and an anchor frame

- Date: 2026-08-10
- Status: Accepted and complete
- Scope: Core seek semantics, frame stepping, presentation queues, audio/subtitle
  suppression, callbacks, and natural-end races

### Context

An ordinary demux seek can report success after positioning near a target, but
it does not identify the exact decoded video frame an editor or paused player
will show. Implementing accurate seek as a synchronous decode call would break
Player's asynchronous control model and bypass bounded decode/presentation
workers. Ordinary late-frame and queue-capacity policies could also discard the
very target frame that completes the request. Backward stepping cannot assume
the previous frame is still retained, especially across keyframe boundaries.

### Decision

1. `SeekFlag::Accurate` refines the existing asynchronous seek. Player seeks to
   a preceding keyframe, decodes forward, suppresses pre-target video, audio,
   and subtitle delivery, and selects the first decoded video frame at or after
   the requested position.
2. The selected frame is installed as the immediate presentation anchor for
   the new generation. It bypasses ordinary late-frame and queue-capacity drops
   but still uses the application scheduler or normal presentation worker.
3. Completion runs on the playback worker only after that frame has been
   handed to presentation, and reports its actual timestamp. With no active
   video track, completion falls back to the demux-seek result.
4. A playing accurate seek preserves play intent and re-enters the normal
   packet/output-clock anchoring path. A paused accurate seek publishes the
   anchor without starting audio and remains paused.
5. `stepForward()` and `stepBackward()` invalidate the retired generation,
   keep the audio device paused, publish exactly one adjacent video frame, and
   leave Player paused. Backward stepping uses retained predecessor history or
   reconstructs it by decoding from the active range start.
6. The control path publishes its interrupt epoch and new presentation
   generation before the playback worker can consume the request. Natural-end
   teardown rechecks accepted control work before committing EOF, so an
   accepted seek or step cannot be lost to a concurrent drain.

### Consequences

- Accurate seek is deterministic at the decoded-frame boundary without adding
  a synchronous decoder API or a second media pipeline.
- Audio and subtitles below the target cannot leak across the seek generation,
  and the target frame cannot disappear because ordinary presentation is late.
- Backward stepping may perform bounded asynchronous reconstruction and is not
  guaranteed to complete at the cost of only one cached frame.
- Applications must use the completion timestamp rather than assuming that the
  requested millisecond exactly names a decoded frame.

## AD-019: Backends remain compile-time modules until a runtime boundary is justified

- Date: 2026-08-10
- Status: Accepted
- Scope: Repository ownership, optional backend linkage, package ABI, runtime
  loading, and possible future repository splits

### Context

QtAVCore's backend contracts are still evolving across three supported target
families and several native SDKs. Compile-time CMake targets already let an
application select only the render, audio, hardware-decode, interop, subtitle,
and high-level output modules it needs. Introducing runtime plugins now would
freeze discovery, versioning, allocation, threading, and lifetime boundaries;
exporting STL or C++ virtual ABI across arbitrary compilers would add a support
promise that the current package does not need.

### Decision

1. Keep optional backends in this repository and expose them as separately
   linkable CMake targets while interfaces evolve. The core does not scan for
   or load runtime plugins.
2. Version the core C++ API and installed CMake package before treating backend
   interfaces as a stable release boundary.
3. If runtime-loaded plugins become necessary, define a versioned C ABI with
   explicit ownership, capability, threading, and error contracts. Do not
   expose STL containers, exceptions, or C++ virtual ABI across arbitrary
   toolchains.
4. Split a backend into another repository only when it has an independent
   license, team, release cycle, or closed-source delivery requirement. A
   directory boundary or optional dependency alone is insufficient.

### Consequences

- Current applications keep deterministic compile-time linkage and installed
  target discovery; no runtime loader or plugin search path enters the core.
- Backend contracts may evolve with the monorepo while static/shared install
  and external-consumer tests remain the compatibility gate.
- A future plugin system requires a separate accepted design and cannot be
  inferred from the current `VideoRenderAPI`, `AudioSink`, or C++ backend
  classes.
- Release CI and API/package versioning remain open tasks in
  [`PLAN.md`](PLAN.md), while the repository/runtime boundary itself is no
  longer an unchecked planning choice.

## AD-020: Audio time stretch is an optional post-conversion stage

- Date: 2026-08-10
- Status: Accepted and complete
- Scope: Playback rate, PCM processing, device clocks, A/V synchronization,
  audio lifecycle, and optional backend linkage

### Context

Changing Player's monotonic video clock without changing device PCM duration
leaves audio at its original speed and makes its device clock an invalid master.
Changing the sink sample rate would alter pitch and overload format conversion
with a semantic processing responsibility. A solution also has to preserve the
core's optional-backend boundary and behave deterministically across seeks,
loops, track changes, and rate changes after PCM has already entered a device
queue.

### Decision

1. `AudioTimeStretcher` is an optional core contract placed after
   `AudioFrameConverter` and before `AudioSink`. It preserves the negotiated PCM
   format and media timestamps while changing the physical sample count. Core
   does not link a mandatory DSP implementation.
2. `QtAV::AudioTimeStretch` is the reference backend. It owns an FFmpeg
   `atempo` filter graph and composes bounded 0.5-2.0 stages for rates outside
   one filter's native range. It accepts the interleaved PCM families already
   produced by `QtAV::AudioResample`.
3. Rate 1.0 bypasses the processor exactly. A non-1.0 rate without a configured
   processor keeps decoded-audio callbacks active but disables device output
   with `audio.time_stretch.unavailable`; unstretched PCM must not become an
   incorrect playback master.
4. A sink clock remains the physical device PCM position anchored to the first
   media timestamp after open or flush. Player maps only the elapsed delta by
   the active rate, caps it at submitted media time, and publishes that mapped
   cache as the A/V master.
5. A live rate change increments the audio-chain generation. Seekable loaded
   media accurately re-decodes from the current position before the sink and
   processor reopen, preventing PCM submitted at the previous rate from being
   skipped or heard after the transition.
6. Pause/resume preserves processor and sink state. Prepare, seek, audio-track
   or external-audio change, loop/range transition, media replacement, stop,
   and discontinuous input reset buffered processing. Natural segment end
   drains the converter, then the time stretcher, then the sink. All calls use
   the existing serialized audio-output/lifecycle boundary.

### Consequences

- Format conversion, semantic time processing, and platform device ownership
  remain independently replaceable responsibilities.
- Windows, Android, and OHOS device backends continue measuring native PCM
  frames and require no rate-specific DSP or platform API changes.
- Applications that offer speed control and device audio should link and inject
  a time-stretch backend; applications using only decoded-frame callbacks do
  not need it.
- Mid-playback rate changes on seekable inputs are continuity operations and
  can incur one accurate-seek/reopen transition rather than mixing two rates in
  a queued device stream.

### Validation

Deterministic tests measure 440 Hz output and media/physical duration at 0.75x
and 1.5x, verify reset, discontinuity, repeated drain, exact 1.0 bypass,
mid-playback changes, seek/stop/natural-end lifecycle, and physical-device-clock
mapping. Windows static/shared tests and install consumers pass, as do OHOS and
Android static/shared cross-builds against their repository dependency
prefixes. Detailed evidence is retained with the dated implementation history
referenced by [`PLAN.md`](PLAN.md).

## AD-021: General processing starts with bounded audio and video contracts

- Date: 2026-08-10
- Status: Accepted and complete
- Scope: Audio effects, video transforms, lifecycle, scheduling, timestamps,
  optional backend linkage, and filter migration

### Context

The legacy tree exposed mutable Qt-oriented audio and video filters, while the
rewrite already had three distinct processing boundaries: decoded-frame
callbacks, direct hardware-frame scheduling, and render-thread graphics APIs.
A single generic frame callback would conflate ownership, backpressure,
timeline, and thread requirements. The first supported use cases are streaming
PCM effects such as gain/equalization and synchronous software-video transforms
such as pixel or geometry changes; delayed cadence conversion and native-GPU
effects require different schedulers.

### Decision

1. `AudioFrameProcessor` is an optional core contract after format conversion
   and `AudioTimeStretcher`, before `AudioSink`. It preserves negotiated PCM
   format, media-timeline order, and total physical samples per completed
   segment, while allowing zero-or-more output views per input for bounded
   buffering and repartitioning.
2. `VideoFrameProcessor` is an optional synchronous, one-input/one-output core
   contract on the video-decode worker. `VideoFrameScheduler` retains first
   refusal for direct codec-surface presentation. An ordinary processed result
   preserves timestamp and duration exactly; format-level and per-frame bypass
   are explicit.
3. Public contracts contain no Qt, FFmpeg, graphics, or platform SDK types.
   Processor-owned audio output lives through the next processor operation;
   copied `VideoFrame` objects retain reference-counted storage normally.
4. Pause/resume preserves state. Timeline discontinuities reset buffered state;
   natural segment end drains it; track/media or live processor replacement
   closes and reopens at a clean generation boundary. Stop resets and closes.
   Contract violations fail closed and publish categorized media events.
5. Queued video cadence conversion is deferred to a future queue/scheduler
   contract. Native graphics-context effects remain `VideoRenderAPI` work on
   the application's render thread.
6. `QtAV::AudioFilter` is the narrow reference backend. It owns an FFmpeg
   `volume` graph and exposes a constant non-negative linear gain without
   accepting an arbitrary graph string or leaking FFmpeg types into core.

### Consequences

- Raw decoded audio callbacks stay upstream of conversion and processing;
  ordinary video callbacks and renderers observe processed frames, except when
  direct scheduling accepted the decoder frame first.
- Audio backpressure is explicit through zero-output process results and
  repeated drain. Video buffering is forbidden by the first contract, keeping
  presentation cadence and generation invalidation deterministic.
- Applications link no new mandatory dependency. They may inject their own
  processors, link the optional reference audio target, or omit processing
  completely.

### Validation

Deterministic player tests cover buffered audio ordering and equal sample
counts, natural drain, pause preservation, seek reset, stop close, video
one-to-one processing, explicit format bypass, and fail-closed processor errors.
The reference volume backend verifies sample values, format/timestamp
preservation, reset/drain, and invalid-format rejection. Supported-target
cross-build and installed-package results are recorded in the dated plan
history referenced by [`PLAN.md`](PLAN.md).

## AD-022: QtAVCore 2.0.0 starts the versioned C++ and CMake package contract

- Date: 2026-08-10
- Status: Accepted and complete
- Scope: Public C++ API versions, shared-library ABI metadata, exported CMake
  targets, package discovery, release increments, and plugin-boundary limits

### Context

The standalone rewrite project has declared version 2.0.0 since its initial
repository commit, and its libraries already carried matching CMake `VERSION`
and major `SOVERSION` properties. The version was not available through a
public C++ header, package variables were not guaranteed when no version was
requested, compatibility promises were undocumented, and exact/compatible/
rejected package requests had no deterministic test. Treating CMake package
selection, C++ source compatibility, arbitrary-toolchain binary compatibility,
and a future plugin ABI as one promise would make the release boundary broader
than the current architecture supports.

### Decision

1. Confirm 2.0.0 as the first formal release version of QtAVCore, independent
   of the legacy QtAV ABI. The root CMake project version is the single source
   for generated public, package, and binary metadata.
2. Generate `<qtav/version.h>` with preprocessor components plus
   `qtav::coreVersion` and `qtav::coreVersionString`. Installed package configs
   always publish `QtAVCore_VERSION` and its major/minor/patch components.
3. Keep CMake package discovery on `SameMajorVersion`: an installed package is
   compatible only when its major matches and its version is not older than the
   request. `EXACT` remains available for deployment locks. Existing exported
   target names remain valid within a major release; additive optional targets
   are compatible, while removal or incompatible redefinition is major.
4. Use patch releases for fixes that preserve public source and shared-library
   interfaces. Minor releases may add APIs or targets while preserving existing
   source contracts and the shared ABI inside one supported ABI domain. Any
   incompatible public signature, layout, virtual interface, ownership rule,
   inline contract, or exported-target change increments the major version.
5. Set every supported shared target's `VERSION` to the full release and
   `SOVERSION` to the major release, and fail configuration if a production
   shared target diverges. Windows uses the resulting PE image version and
   OHOS uses the major ELF soname plus versioned library names. Android keeps
   its NDK-standard unversioned `.so` name and soname, with release identity in
   the public header and package config. None is an arbitrary-toolchain ABI
   guarantee.
6. Limit a shared ABI domain to the same architecture, compiler ABI and
   compatible toolset, C++ standard library/runtime mode, relevant build mode,
   and dependency ABI. Static consumers rebuild. No cross-compiler,
   cross-runtime, or cross-architecture C++ ABI compatibility is promised.
7. Do not infer a runtime plugin ABI from this version. AD-019 remains in force:
   optional backends are compile-time C++ targets, and runtime loading requires
   a separately accepted versioned C ABI with explicit ownership and threading.

### Consequences

- Applications and build systems can compare the same release value without
  importing FFmpeg, graphics, window-system, audio, or CMake implementation
  types into core public headers.
- Compatible package discovery is predictable, but deployment code must still
  respect the narrower shared ABI domain and rebuild static or structurally
  affected consumers.
- Windows shared images and OHOS ELF libraries carry their platform's
  release/major metadata; Android keeps one unversioned native name while
  reporting the same release through its header and package.
- Plugin discovery, allocation, capabilities, and lifetime remain unfrozen.

### Validation

Core API compilation asserts the generated values. CMake configure tests cover
2.0.0 exact acceptance, 2.0 compatible acceptance, 2.1 rejection, and previous-
major rejection. A separate staged-install consumer validates package variables,
the installed header, `QtAV::Core`, compilation, and final linking. Windows
static/shared tests and Android/OHOS static/shared package consumers provide the
supported-target gate; detailed commands and results are retained in the dated
plan history referenced by [`PLAN.md`](PLAN.md).
