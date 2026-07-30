# Android connected-device harness

This harness proves the first Android production-path slice without Qt or a
Gradle dependency. It cross-builds a pinned minimal FFmpeg 8.1.2 configuration
and QtAVCore for `arm64-v8a`, packages a platform `NativeActivity`, and checks
software MPEG-4 plus PCM decode, a bounded three-frame Vulkan submission ring,
native HDR swapchain presentation, an OpenGL ES 3.x/EGL SDR fallback, and
background/foreground surface recreation on one connected device.

Requirements:

- macOS host;
- Android SDK under `ANDROID_SDK_ROOT`, `ANDROID_HOME`, or the standard
  macOS user SDK directory;
- NDK `28.2.13676358`, platform 36, and build-tools 37.0.0 by default;
- SDK CMake 4.1.2/Ninja and Android Studio's bundled JBR;
- CMake, Ninja, curl, and host FFmpeg;
- the NDK `glslc` shader compiler and an Android Vulkan device;
- exactly one authorized, awake, unlocked arm64 Android device with OpenGL ES
  3.x and a Vulkan HDR surface-format pair for deployment.

Build:

```sh
modern/examples/android/build-android.sh
```

Deploy and collect the result:

```sh
modern/examples/android/run-connected-device.sh
```

Generated sources, libraries, APKs, signing keys, media, and device reports
stay under `build/android/`. Override the SDK or pinned installed tool
selection with `ANDROID_SDK_ROOT`, `QTAV_ANDROID_NDK_VERSION`,
`QTAV_ANDROID_API`, `QTAV_ANDROID_COMPILE_SDK`, or
`QTAV_ANDROID_BUILD_TOOLS`. `QTAV_ANDROID_CMAKE_VERSION` selects another
installed SDK CMake package.

The deployment script runs one non-streaming `adb install` command. If
installation or replacement fails and the device may be waiting for
authorization, stop and approve the prompt manually on the device before
asking to retry.
It wakes the device and asks Android to dismiss a non-secure keyguard. If the
device remains asleep or securely locked, the script stops for manual unlock
instead of repeatedly retrying window creation.

The application creates its own Vulkan instance, logical device, and
graphics/present queue, enables `VK_EXT_swapchain_colorspace`, and enables
`VK_EXT_hdr_metadata` when the device exposes it.
`QtAV::RenderVulkanAndroid` retains the current `ANativeWindow`, requires a
native HDR target in this harness, and owns only its surface/swapchain
generation. A successful result reports decoded video frames,
Vulkan-rendered frames, decoded audio frames, selected HDR format/color space,
metadata-extension state, Android compositor recognition of an active HDR
layer, presentation of a synthetic P010/BT.2020/PQ frame carrying mastering
and MaxCLL metadata, and at least one HDR surface recreation. The deployment
script sends the application to the launcher once and resumes the same
activity; playback is paused while its window generation is absent and
continues without reopening the media after the Vulkan surface/swapchain is
rebuilt. The harness also renders deterministic offscreen
goldens for ring reuse, limited/full-range BT.601/BT.709 conversion,
P010/BT.2020 PQ/HLG HDR-to-SDR numeric output, native 10-bit HDR10/PQ and
HDR10/HLG encoding, HLG-to-PQ conversion, FP16 extended-linear-sRGB and
BT.2020-linear output above reference white,
mastering-display/MaxCLL/default-luminance selection, viewport, rotation, and
target recreation.
The same run creates an offscreen OpenGL ES 3 context and verifies actual
uploads/readback for YUV420/422/444, NV12/NV21, little-endian P010,
RGB/BGR/RGBA/BGRA/ARGB, and Gray8 together with viewport, rotation, and target
generation. After the Vulkan HDR playback/lifecycle checks finish, the Android
EGL adapter owns a real window surface and presents a synthetic
P010/BT.2020/PQ frame through the documented SDR tone-mapping fallback. The
automatic Vulkan-to-OpenGL ES selector is intentionally a separate next step.

`install-consumer/` is a standalone Android CMake consumer for validating an
installed package. It includes the installed Android EGL header and links only
`QtAV::RenderOpenGLAndroid`, which also proves that the exported target brings
in `QtAV::RenderOpenGL` and logical Android `EGL`/`GLESv3` dependencies without
embedding the producer machine's NDK sysroot path.
