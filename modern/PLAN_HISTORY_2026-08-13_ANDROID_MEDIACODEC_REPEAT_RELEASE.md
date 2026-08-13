# Android MediaCodec repeat-release and fullscreen audit

Date: 2026-08-13

## Scope

Audit Android MediaCodec direct-Surface, Vulkan AImageReader, and OpenGL ES
AImageReader ownership for the same class of failure as the OHOS
`legend.mkv`, 0.5x, fullscreen stall. Fix the underlying release contract if
present; do not add redraw suppression, timeouts, dimension probes, decoder
reopen loops, or another presentation workaround.

## Finding

The same root ownership ambiguity was present in Android source.

FFmpeg 8.1.2 shares an atomic `released` flag across retained
`AVMediaCodecBuffer` views. `av_mediacodec_release_buffer()` and
`av_mediacodec_render_buffer_at_time()` invoke the native MediaCodec release
only for the first valid decision and suppress both later decisions and a
first decision after decoder flush changed the serial. The suppressed path
returned zero, indistinguishable from a real native queue operation.

QtAVCore's Vulkan and OpenGL interops begin a provisional producer-epoch
association before calling that helper. Android's renderer invalidates pending
frames during `surfaceChanged()` even when a fullscreen/size refresh republishes
the same `ANativeWindow`. The retained latest frame can consequently be offered
again in the new epoch. The old FFmpeg result made the interop wait for an
`AImageReader` callback even though the duplicate native release was
suppressed. The producer-epoch tombstones protect late callbacks but cannot
recover information erased by FFmpeg's false success.

This is independent of coded size. The AImageReader starts with a minimal
default and MediaCodec supplies the real decoded allocation; every acquired
image carries its actual dimensions and crop. Playback rate only changes the
race window in which the retained frame remains current.

## Repair

- Added FFmpeg overlay `0059-mediacodec-repeat-release-status.patch` so both
  MediaCodec release helpers return `AVERROR(EALREADY)` for an already-decided
  or flush-retired output. Zero now means the current call actually performed
  the native release.
- Added the overlay to the port and made the Android install verifier require
  its public header contract.
- Added `MediaCodecFrameDecisionResult` plus `presentResult()`,
  `presentAtResult()`, and `dropResult()`. Existing bool methods remain source
  compatible and are true only for `Applied`; class object layout is unchanged.
- Vulkan and OpenGL AImageReader interops cancel the provisional epoch
  association and return stale for `AlreadyReleased`. They neither wait for an
  image nor retry the frame.
- Direct-Surface presentation counts only `Applied`; `AlreadyReleased` is
  retired as stale rather than reported as a successful presentation or a
  pipeline failure.

## Validation

Dependency and target builds:

- `ffmpeg/scripts/build-android.ps1 -NdkRoot
  "$env:LOCALAPPDATA/Android/Sdk/ndk/29.0.14206865"`: passed; the patch applied,
  the arm64/API 28 package rebuilt, and post-build verification passed.
- `cmake -DINSTALL_ROOT=... -DTRIPLET=arm64-android-28-static -P
  ffmpeg/cmake/verify-install.cmake`: passed with the new Android contract gate.
- `modern/scripts/ci/build-android.ps1 -SkipDependencies -Parallel 8`: passed
  for NDK 29 shared/static Release builds, installs, MediaCodec Vulkan/OpenGL
  interops, and both external installed-package consumers.
- The Android player native library rebuilt and the v3-signed audit APK
  (`versionCode=7`, min/target SDK 28/36) installed successfully.

Connected device: model `2410DPN6CC`, Android 16/API 36, Adreno 830.

- `legend.mkv` with MediaCodec/Vulkan AImageReader entered fullscreen and kept
  presenting. A paused snapshot after the transition reported
  callbacks/presented `3567/3555`, core queue/late drops `0/0`, and interop
  queued/acquired/imported `3555/3555/3555`, with zero CPU
  map/transfer/staging/upload.
- The same file was reconfigured to MediaCodec/OpenGL ES AImageReader, resumed,
  and entered fullscreen. Its paused snapshot reported callbacks/presented
  `606/606`, core queue/late drops `0/0`, and interop
  queued/acquired/imported `606/606/606`.
- SurfaceFlinger presentation timestamps advanced across the Vulkan fullscreen
  transition, and logcat reported the first successful frame for both renderer
  generations with no crash-buffer entry.

The current Android example has no playback-rate control, so the device pass
does not claim an exact 0.5x UI reproduction. Source tracing establishes that
the ambiguous release and pending-callback failure are rate-independent; the
lower rate only makes the retained-frame redraw window easier to hit.
