# OHOS player full-screen and PiP regression

Date: 2026-08-12

This bounded follow-up repaired three device-visible faults in the OHOS user
player without advancing the active Android task in `PLAN.md`:

- entering or leaving landscape full screen restarted loaded media at the
  playback-range origin;
- full-screen video was rotated relative to the UI;
- the rotated video used the wrong geometry and was stretched.

## Root cause

The normal and full-screen ArkUI branches each created a different
`XComponent`. Changing branches destroyed the old native surface, cleared the
hardware-decode configuration, and installed a new OHCodec surface token.
`Player::setHardwareDecodeConfig()` invalidated the loaded generation but did
not create a position-bearing prepare request, so the asynchronous reopen used
the range start.

On the connected ALN-AL80, landscape rotation also differs by rendering path.
The Vulkan XComponent surface needs a 90-degree renderer compensation, while
the EGL NativeWindow is already transformed by the compositor. Applying one
shared rotation therefore leaves one backend sideways. Vulkan additionally
replaced the requested viewport with an empty viewport whenever it recreated
the swapchain, which prevented an inverse-transform `Fit` rectangle from
surviving the size change.

PiP did not share the original size fault. Before the repair, RenderService
already reported a 1182x665 XComponent surface, a 672x378 displayed rectangle,
`TransformType=0`, and zero absolute rotation. Its source and displayed
rectangles were both effectively 16:9.

## Repair

- ArkUI now owns one persistent XComponent and only changes the surrounding
  controls and size for full screen.
- A hardware-decode configuration change while media is active schedules its
  asynchronous reopen at the observed player position.
- The OHOS player exposes a small N-API rotation control. Landscape Vulkan
  uses 90-degree renderer compensation plus an inverse-transform viewport;
  OpenGL ES keeps renderer rotation at zero and uses ordinary `Fit`.
- The OHOS Vulkan adapter retains a valid application viewport across
  swapchain recreation.
- Entering PiP restores zero renderer rotation, and leaving PiP restores the
  current normal/full-screen policy.

## Validation

The core regression was built and run in Debug with assertions enabled:

```text
cmake --build build/modern-shared-clang --config Debug \
  --target qtav_core_hardware_decode_device_test
build/modern-shared-clang/bin/Debug/qtav_core_hardware_decode_device_test.exe \
  build/modern-shared-clang/tests/qtav-core-test.mp4
PASS
```

The complete OHOS shared build, native player link, ArkTS compilation, signed
debug HAP packaging, overwrite install, and launch all passed. Device-local
`legend.mkv` then produced these layout observations:

| Path | Before | After | XComponent | Geometry and orientation |
| --- | ---: | ---: | --- | --- |
| OHCodec/Vulkan full screen | 0:14 | 0:18 | `91:7` -> `91:7` | upright 2240x1260 content in 2720x1260, about 240 px side bars |
| OHCodec/OpenGL ES full screen | 0:21 | 0:26 | `95:7` -> `95:7` | upright 2240x1260 content in 2720x1260, about 240 px side bars |
| exit full screen | 0:18 | 0:57 | `91:7` -> `91:7` | upright portrait host, no restart |
| system PiP | 0:57 | 1:05 | same transferred surface | 1182x665 surface, 672x378 display, transform 0, rotation 0 |

The final signed-package smoke repeated automatic OHCodec/Vulkan from 0:12 to
0:28 with XComponent hash `96:7` unchanged and the same upright 2240x1260
content geometry.

System screenshots confirmed upright, non-stretched output for both full-screen
renderers and PiP. The PiP result is a negative finding: it did not have the
same size-distortion defect before the repair and remained correct afterward.

`git diff --check`, Qt-dependency scanning of the changed core and native
sources, and UTF-8/LF checks complete the local acceptance.

## Follow-up root-cause correction

The device-visible result above was real, but the first repair retained two
workarounds which were later found to be below the required continuity bar:

- `OnSurfaceChanged` still suspended and recreated the mobile renderer
  candidate. That also replaced the OHCodec interop surface generation, so
  `Player::setHardwareDecodeConfig()` reopened media and sought to a keyframe.
  Restoring the observed position hid a full restart but could still stall and
  display an earlier frame.
- Full-screen ArkUI state drove a demo-level Vulkan/GLES rotation branch and
  inverse viewport. Vulkan WSI `currentTransform` belongs to the OHOS Vulkan
  adapter, not to media rotation or the player page.
- Subtitle-track replacement used the same global queue invalidation,
  audio-sink close, and primary keyframe seek as audio/video track changes.

The follow-up implementation therefore keeps the selector candidate and
OHCodec surface generation across a same-window resize, recreates only Vulkan
swapchain/render targets (or refreshes the existing EGLSurface geometry), and
handles the WSI transform in the Vulkan adapter. Subtitle-only switching now
keeps A/V queues and the audio sink alive and uses a bounded recent subtitle
packet cache to restore a current cue without seeking the primary input.

Windows Debug tests cover no subtitle-induced Buffering, audio open/close/
flush stability, and monotonic video timestamps. The OHOS arm64 shared player
cross-build passes. After deployment authorization, the signed HAP was rebuilt
and overwrite-installed on the ALN-AL80. Five consecutive full-screen/restore
cycles with device-local `legend.mkv` kept the XComponent at 2720x1260 in
landscape and 1260x1102 in portrait; 16:9 content remained 2240x1260 with
240-pixel side bars in full screen and approximately 1260x709 in the normal
layout. Playback advanced continuously from 1:57 to 2:34 with no restart,
backward jump, or stop. The final page uses no transition mask or artificial
delay.
