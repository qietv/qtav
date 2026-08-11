# OHOS user player demo implementation record

Dates: 2026-08-10 through 2026-08-11

## Scope

Implemented a manual, user-facing OHOS ArkUI player separately from the
automated XComponent validation harness. The demo owns only application shell
responsibilities and consumes existing QtAVCore contracts for playback,
rendering, audio, time stretch, tracks, subtitles, buffering, and network
recovery.

Implemented controls and presentation:

- local document picker input through a retained duplicated descriptor and
  `/proc/self/fd/<fd>` FFmpeg path;
- direct URL input;
- play/pause/stop, accurate seek, ten-second skip, and 0.5x through 2.0x
  pitch-preserving rates;
- audio-track and subtitle-track selection, subtitle disable, and timed plain-
  text subtitle display;
- Vulkan-preferred software presentation with bounded recovery and OpenGL ES
  fallback, plus OHAudio/libswresample/atempo output;
- normal and landscape full-screen layouts;
- system PiP through the ArkUI XComponent controller, PiP play/pause and seek
  controls, and automatic PiP when returning home;
- a closeable upper-left 1 Hz diagnostics panel with source, media/track,
  renderer, presentation FPS, decoded/presented, and Player drop counters.

## Validation completed on the Windows cross-build host

- Reused the verified repository OHOS arm64/API 23 FFmpeg dependency prefix and
  repository-local host `glslc`.
- `qtav_ohos_player_demo` compiled and linked as ELF64 AArch64
  `libqtav_player.so`.
- Dynamic dependencies include the expected QtAVCore render-mobile,
  Vulkan/OpenGL OHOS adapters, OHAudio, libswresample, and audio-time-stretch
  shared targets plus OHOS N-API/window system libraries.
- ArkTS compilation and HAP packaging completed successfully.
- The generated package was
  `player-hap/entry/build/default/outputs/default/entry-default-unsigned.hap`;
  Hvigor explicitly reported that no signing configuration was found.
- HAP inspection found `module.json`, compiled `ets/modules.abc`, and the
  packaged arm64 `libs/arm64-v8a/libqtav_player.so`.

## Signed-device validation

After the user configured DevEco automatic signing, product `default` was
connected to signing configuration `default`. `hap-sign-tool verify-app`
validated the v3 HAP signing block, debug profile, native-library code
signatures, and SHA-256 digest before each deployment.

The first signed launch exposed a missing runtime boundary: the packaged
`libqtav_audio_timestretch.so.2` required `libc++_shared.so`, so the XComponent
could not reach N-API initialization. The staging script now packages the
official OHOS arm64 C++ shared runtime. The verified HAP contains eleven native
libraries, and device logs then recorded native module initialization,
XComponent callback registration, and Vulkan renderer selection.

Connected-device results:

- HTTPS W3C Sintel playback identified 854x480 H.264/AAC, sustained about 24
  successful Vulkan presentations per second with zero drops in the initial
  sample, accepted 1.5x playback, and exercised ten-second skip.
- Landscape full screen changed the root to 2720x1260 and restored to
  1260x2720. PiP showed a live Sintel frame on the desktop, retained the
  process while the main ability was backgrounded, and stopped through the
  explicit main-player control with the WMS PiP window destroyed.
- A generated seekable 640x360 H.264/two-AAC/SubRip Range fixture switched the
  active audio track from 1 to 2 while remaining loaded. Subtitle selection
  changed 3 to -1 and back to 3, cleared the old cue, and rendered the Chinese
  cue after re-enable. Presentation continued with zero reported Player drops.
- The close/reopen media-information controls, visible top-right `X`, 1 Hz FPS
  panel, progress UI, and system safe-access Document Picker were exercised.
  The picker was immediately dismissed without reading or selecting private
  device media.

Physical audibility, perceived pitch preservation, and selection of an actual
private local media file remain subjective/user-owned confirmations; the
application paths and objective native state changes are implemented and
device exercised.
