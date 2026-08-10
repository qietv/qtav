# General processing contracts implementation and validation history

Date: 2026-08-10

This record owns the detailed evidence for the completed general audio/video
processing task. `PLAN.md` retains only concise status and task ordering.

## Implemented boundary

- Added optional core `AudioFrameProcessor` and `VideoFrameProcessor`
  contracts plus `Player::setAudioFrameProcessor()` and
  `Player::setVideoFrameProcessor()` injection points.
- Placed audio processing after negotiated format conversion and optional time
  stretch, but before `AudioSink`. It preserves the negotiated format,
  media-timeline ordering, and total physical sample count across each drained
  segment while permitting bounded zero-or-more output per input.
- Kept decoded-audio callbacks upstream and unchanged.
- Placed synchronous video processing on the video-decode worker after
  `VideoFrameScheduler` declines direct hardware-frame handling and before
  ordinary video callbacks or rendering. Successful transforms preserve exact
  timestamp and duration; format-level and per-frame bypass are explicit.
- Defined pause preservation, discontinuity reset, natural-end drain,
  seek/loop/range reset, track/media/processor replacement, stop, close, and
  fail-closed behavior before integrating the stages.
- Added the optional `QtAV::AudioFilter` reference backend. Its public
  `VolumeAudioFrameProcessor` accepts a fixed non-negative linear gain while
  its private implementation owns the FFmpeg `abuffer`/`volume`/`aformat`/
  `abuffersink` graph. No arbitrary filter-graph string or FFmpeg type crosses
  the core boundary.
- Exported the core contracts and reference backend through the installable
  CMake package and extended Android/OHOS installed-package consumers.

The accepted rationale is AD-021 in `DECISIONS.md`; responsibility, threading,
lifetime, public usage, and migration behavior are documented in
`ARCHITECTURE.md`, `README.md`, and `MIGRATION.md`.

## Deterministic coverage

- A buffered audio processor verifies input/output ordering, one-buffer
  latency, bounded backpressure, timestamp preservation, equal segment sample
  counts after drain, and processor-owned output lifetime.
- Player integration covers natural drain, pause/resume preservation, seek
  reset, stop/close, repeated loop-segment drain/reset, and fail-closed audio
  processing errors.
- Video integration covers synchronous one-to-one transformation, explicit
  format bypass, timestamp/duration preservation, reset/close lifecycle, and
  fail-closed invalid-frame contract results.
- The volume backend covers supported interleaved sample families, exact gain
  application, timestamp/duration preservation, reset and repeated drain, and
  planar/invalid-format rejection.

## Passed validation

- Windows ClangCL/lld static Release build: passed; CTest 54/54 passed.
- Windows ClangCL/lld shared Release build: passed; CTest 54/54 passed.
- Static and shared installs export `QtAV::AudioFilter`, its public header, the
  two core processor headers, and their aggregate `qtav.h` includes.
- Separate Windows static/shared ClangCL consumers configured against the
  installed packages, linked `QtAV::AudioFilter`, instantiated both processor
  contracts, and exited successfully.
- Android arm64/API 28 static/shared `qtav_core` and `qtav_audio_filter` builds,
  installs, and separate installed-package consumers passed against the local
  verified repository dependency prefix.
- OHOS arm64/API 23 static/shared target builds, full installable target sets,
  installs, and separate installed-package consumers passed with the cached
  DevEco toolchain and repository dependency prefix.
- No Android or OHOS application was installed on a physical device; this task
  required compile, package, and deterministic host-test coverage only.

The first temporary Windows consumer used the generator's default MSVC linker,
which cannot consume this repository's ClangCL/lld FFmpeg archives. Repeating
the same consumer with the supported `-T ClangCL` toolchain passed in both
library modes; the unsupported attempt is not validation evidence and caused
no source-tree change.

Final source-boundary checks passed after the plan update: `git diff --check`,
UTF-8 without BOM, LF-only changed text, and the forbidden-dependency scan over
new core public headers and implementation found no Qt, FFmpeg, graphics, or
platform SDK exposure.
