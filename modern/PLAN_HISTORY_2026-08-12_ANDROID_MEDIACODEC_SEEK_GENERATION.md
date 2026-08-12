# Android MediaCodec seek-generation isolation — 2026-08-12

This record closes the Android MediaCodec/AImageReader seek-generation task
formerly tracked in `PLAN.md`.

## Implementation

- Vulkan and OpenGL MediaCodec AImageReader interops share a bounded producer
  epoch tracker. An output released before invalidation remains represented
  until its late image is acquired and discarded; timestamp proximity alone
  cannot promote an unproven old image into the current epoch.
- Player presentation invalidation reaches both Android renderer adapters and
  interops. It wakes Vulkan's bounded image wait and seals a render overlapping
  seek or surface replacement without waiting for submitted GPU work.
- The Android NativeActivity regression and player demo no longer flush
  interop objects before seek. Public `flush()` remains for standalone teardown
  and follows the same epoch contract.
- The player demo removed its application-owned exact-frame, pending-frame,
  reservation, and deadline queues. Its native render thread calls
  `Player::renderVideoDetailed()`; Player owns an exact deferred frame, while a
  bounded timer is used only for explicit busy results.
- Android raw AHardwareBuffer/EGLImage input converts the top-left AImage crop
  to OpenGL's bottom-left sampling origin in its crop matrix. The generic raw
  normalization FBO keeps `plane.flipped = true` for libplacebo. No Java or
  `SurfaceView` transform is involved.

## Deterministic and build validation

- Windows Visual Studio 2026/ClangCL Release shared: 61/61 CTest passed after a
  complete rebuild, including `qtav_mediacodec_image_epoch` and
  `qtav_render_mobile_selector`.
- Windows Visual Studio 2026/ClangCL Release static: 61/61 CTest passed after a
  complete rebuild.
- The repository Android CI script passed dependency verification, arm64/API
  28 shared and static builds and installs, plus both external package
  consumers. The Android player native library and signed sidecar APK also
  rebuilt successfully.
- The lifecycle test covers repeated timestamps, invalidated late callbacks,
  media replacement, bounded retirement, and cancelled/unproven images for
  Vulkan and OpenGL independently. The mobile selector test covers an
  invalidation overlapping a render call.

## Connected-device validation

Device `2410DPN6CC` (`haotian`) remained authorized throughout the run. The
sidecar package `org.qtav.core.player.audit` was used so the existing signed
player package was not replaced.

- `qtav-player-h264.mp4`: MediaCodec H.264 completed through Vulkan and OpenGL
  AImageReader paths. Non-endpoint forward/backward seek was exercised in both;
  the OpenGL decoded image changed from the embedded time near 4.967 seconds
  back to 1.867 seconds with core queue/late drops `0/0` and bounded depth two.
- `/sdcard/Download/legend.mkv`: HEVC Main10/HDR presented upright at 25 fps in
  Vulkan and fixed OpenGL raw-YCbCr paths. OpenGL forward/backward seek reached
  33:49 then 06:44; H.264-to-HEVC Vulkan media replacement recovered at 25 fps
  with queue/late drops `0/0`.
- `/sdcard/Download/wednesday.mp4`: Dolby Vision/HEVC presented upright at 24
  fps after an in-session OpenGL media replacement. Forward/backward seek
  reached 43:55 then 08:40 with queue/late drops `0/0`; Dolby Vision RPU and raw
  YCbCr counters continued advancing.
- Direct Surface was validated separately on `wednesday.mp4`: forward/backward
  seek, pause/resume, background/foreground surface recreation, and stop all
  completed. MediaCodec logged successive surface generations, presentation
  resumed at 23.9–24 fps with drops `0/0`, and no AndroidRuntime/native crash
  was observed. Direct Dolby Vision dataspace/color output remains an Android
  compositor/certification concern and is not evidence for the OpenGL path.

The Vulkan/OpenGL routes retained their zero decoded-source CPU
map/transfer/staging/upload contract. Differences between decoded callbacks and
successful presents are bounded render scheduling or in-flight ownership and
must not be reported as Player drop counts; explicit queue/late and stale-image
counters remain the relevant evidence.
