# Android connected-device harness

This harness proves the first Android production-path slice without Qt or a
Gradle dependency. It cross-builds a pinned minimal FFmpeg 8.1.2 configuration
and QtAVCore for `arm64-v8a`, packages a platform `NativeActivity`, and checks
software MPEG-4 plus PCM decode on one connected device.

Requirements:

- macOS host;
- Android SDK under `ANDROID_SDK_ROOT`, `ANDROID_HOME`, or the standard
  macOS user SDK directory;
- NDK `28.2.13676358`, platform 36, and build-tools 37.0.0 by default;
- SDK CMake 4.1.2/Ninja and Android Studio's bundled JBR;
- CMake, Ninja, curl, and host FFmpeg;
- exactly one authorized arm64 Android device for deployment.

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
