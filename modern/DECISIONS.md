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
