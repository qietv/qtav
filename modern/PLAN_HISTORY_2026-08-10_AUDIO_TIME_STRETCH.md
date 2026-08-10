# Audio time-stretch implementation and validation history

Date: 2026-08-10

This record owns the detailed evidence for the completed pitch-preserving
playback-rate audio task. `PLAN.md` retains only concise status and task
ordering.

## Implemented boundary

- Added the optional core `AudioTimeStretcher` contract and
  `Player::setAudioTimeStretcher()` injection point.
- Placed time stretch after `AudioFrameConverter` and before `AudioSink`.
  Decoded audio callbacks continue receiving the original PCM.
- Added the optional `QtAV::AudioTimeStretch` backend using repository FFmpeg
  `libavfilter`/`atempo`; 1.0 playback is an exact bypass.
- Kept sink clocks on the physical PCM timeline and mapped their elapsed time
  back to the media timeline at the active rate in `Player`.
- Reopened and flushed the output chain on rate changes. Seekable media resumes
  through an accurate seek at the current media position so queued old-rate PCM
  cannot leak or skip decoded audio.
- Defined reset/drain behavior for prepare, seek, pause/resume, track switch,
  loop/range boundaries, media replacement, stop, and natural end.
- Wired the console and WinUI 3 examples to the reference backend and exported
  the new target through the installable CMake package.

The accepted design rationale is AD-020 in `DECISIONS.md`; public usage,
threading, timestamp, and migration behavior is documented in `README.md`,
`ARCHITECTURE.md`, and `MIGRATION.md`.

## Deterministic coverage

- Real `atempo` tests cover 0.75x and 1.5x on a two-second 440 Hz stereo signal,
  with output sample-count tolerance below 4%, measured pitch within 8 Hz, and
  exact 0-2000 ms media timestamps.
- Backend tests cover reset, input discontinuity, natural drain, and repeated
  drain.
- Player tests cover exact 1.0 bypass, 1.5x to 0.75x mid-playback changes,
  pause/resume preservation, seek reset, stop/close, and natural drain ordering.
- A physical-duration simulated sink verifies that a 2.0x device clock is
  mapped back to the media timeline instead of being treated as media time.

## Passed validation

- Windows ClangCL/lld shared Release build: passed; CTest 52/52 passed.
- Windows ClangCL/lld static Release build: passed; CTest 52/52 passed.
- Focused static/shared rerun after the final pause/resume assertion:
  `qtav_audio_timestretch` and `qtav_core_audio_time_stretch_player` passed.
- Static and shared installs exported `QtAV::AudioTimeStretch`, the backend
  headers, the core `audio_time_stretcher.h`, and the aggregate `qtav.h` include.
- Separate static/shared external consumers configured against the installed
  package, linked `QtAV::AudioTimeStretch`, called `open(format, 1.5)`, and
  exited successfully.
- WinUI 3 x64 Release build passed with 0 warnings and 0 errors and copied the
  new shared backend beside the executable.
- OHOS arm64/API 23 static and shared `qtav_audio_timestretch` targets
  reconfigured and cross-built successfully with Clang 15 and the repository
  FFmpeg `libavfilter` package.
- On 64-bit Windows, `ffmpeg/scripts/build-android.ps1` selected the existing
  Android Studio SDK, CMake 4.1.2, and NDK `30.0.15729638-beta2`; it did not
  install or replace SDK/NDK components. The local arm64/API 28 dependency build
  completed and `cmake/verify-install.cmake` accepted the FFmpeg 8.1.2 prefix.
- QtAVCore Android arm64/API 28 Release static and shared trees both configured
  against that local prefix and completed their full enabled-target builds.
  Tests/examples were disabled because this was a cross-build contract gate;
  no physical-device installation was required.
- The shared build exposed a Windows-hosted NDK r30 `FindPkgConfig` issue:
  transitive `c++`, `c++abi`, and `unwind` names were resolved to archives under
  `prebuilt/windows-x86_64/lib`. The Android-only imported-target sanitization
  now leaves C++ runtime selection to clang++/`ANDROID_STL=c++_static`; the
  regenerated link metadata contains no Windows-host libc++ archive and all
  enabled shared libraries link successfully.
- Separate Android static/shared installed-package consumers linked
  `QtAV::AudioTimeStretch` together with the mobile renderer, AAudio, and
  MediaCodec interop target set successfully.
- `git diff --check` passed and `modern/core` contains no Qt includes.

The existing OHOS dependency prefix separately fails the current full
`verify-install.cmake` check because it predates the required
`ff_vvc_oh_decoder` symbol. That package issue is not caused by this task; the
time-stretch target itself found and linked its required `libavfilter` and
`libavutil` libraries in both library modes.

## Superseded Android artifact attempt

Before the local-only dependency policy and Windows Android build entry point
were accepted, attempts to restore the missing Android prefix from the newest
workflow artifact ended with `unexpected EOF`. That transfer is not part of the
completed validation path. No partial download was consumed as a package, no
older workflow run or independent FFmpeg package was substituted, and the
finished Android builds above use only the locally produced verified prefix.
