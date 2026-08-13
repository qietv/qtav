# OHOS 0.5x full-screen freeze root-cause repair

Date: 2026-08-13

## Scope and reproduction

The user-player regression reproduced on the connected Huawei device with:

1. open local `legend.mkv` (3840x2160 HEVC HDR plus E-AC-3 audio);
2. select 0.5x playback;
3. enter landscape full screen.

Audio, the Player clock, and hardware decoding continued, but presentation
stopped. This separated the failure from decoding, playback rate, and media
dimension discovery.

## Measured localization

Temporary counters showed that native render requests and render attempts kept
advancing after the visible freeze. Hardware decoded frames advanced from
1,176 to 1,664 while successful presentations remained at 1,109. Exactly one
OHCodec output had been recorded as queued without a corresponding
frame-available callback:

```text
before freeze: queue=1111 callback=1110
after freeze:  queue=1111 callback=1110
```

The core repeatedly offered the same deferred frame while the interop waited
for that impossible callback. Temporary request/attempt/deferred/queue
instrumentation was removed after localization and is not part of the fix.

## Root cause

Full-screen entry recreates the render target and legitimately requests a
redraw of the latest `VideoFrame`. At 0.5x, the interval before a new decoded
frame is twice as long, so the redraw frequently reuses the same frame whose
OHCodec output token was already presented.

The FFmpeg OHCodec overlay correctly protected the native output with an
atomic one-shot decision, but a duplicate decision returned success. A fresh
`OHCodecFrame` wrapper around another retained view therefore appeared to
queue the old output again. Vulkan/OpenGL ES interop registered a new pending
producer association and waited for a second surface buffer/callback that the
one-shot codec output could never produce.

This was not a surface-size defect. The player already obtains the encoded
track width and height from `MediaInfo` after stream discovery, with the first
decoded frame only as a fallback. Encoded media size, oriented display size,
ArkUI XComponent size, and Vulkan/EGL render-target extent are independent
contracts; full-screen still requires target recreation even when the encoded
resolution never changes.

## Root repair

- The repository FFmpeg overlay returns `AVERROR(EALREADY)` when another view
  repeats the single permitted OHCodec output decision.
- `OHCodecFrame` exposes additive `presentResult()`, `presentAtResult()`, and
  `dropResult()` APIs with `Applied`, `AlreadyDecided`, and `Failed` outcomes.
  The existing Boolean APIs remain wrappers, and the public class layout is
  unchanged.
- OHCodec Vulkan and OpenGL ES interop map `AlreadyDecided` to `Stale`, remove
  any provisional queued association, and do not wait for a second callback.

No decoder reopen, renderer fallback, duplicate acquire, timeout, frame wait,
surface flush, or caller-side retry was added.

Two hypotheses were tested and rejected during localization: clearing an
orphan-drain latch after flush, and attempting a callback-free nonblocking
consumer acquire. Neither restored presentation, and both experiments were
fully reverted before the root fix.

## Validation

The directly affected repository dependency was rebuilt locally:

```powershell
.\ffmpeg\scripts\build-ohos.ps1
```

FFmpeg 8.1.2 for OHOS arm64/API 23 rebuilt with the modified overlay, and the
script's `cmake/verify-install.cmake` installed-package validation passed.

The complete OHOS shared player build, ArkTS compilation, packaging, and HAP
signing passed. The signed HAP overwrite-installed successfully on target
`29QGK24528000160`.

The root-fix diagnostic build first passed ten consecutive full-screen
exit/re-enter cycles at 0.5x, holding approximately 12.4--12.5 FPS with exact
queue/callback balance. After all temporary instrumentation was removed, the
final signed HAP passed the original reproduction and three more cycles:

| Cycle | FPS | Hardware frames | Presentations | Release/callback | Player drops |
| ---: | ---: | ---: | ---: | ---: | ---: |
| 1 | 12.4 | 1,792 | 1,779 | 1,781/1,781 | 0 |
| 2 | 12.4 | 1,892 | 1,876 | 1,878/1,878 | 0 |
| 3 | 12.4 | 1,980 | 1,961 | 1,963/1,963 | 0 |

Decoded frames, presentations, native releases, and callbacks continued to
advance after every target recreation. The former permanent presentation
freeze did not recur.
