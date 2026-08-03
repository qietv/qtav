# QtAVCore implementation plan

Last updated: 2026-08-03

Status legend:

- `[x]` complete and verified
- `[~]` implemented partially or suitable only as a development baseline
- `[ ]` not implemented

## Target support policy

QtAVCore is maintained for Windows, Android, and OHOS targets only. The former
macOS/iOS implementation, tests, and historical notes are preserved under
[`../archived_apple/`](../archived_apple/) and are no longer maintained,
built, tested, packaged, or installed. Linux is not part of the active target
matrix or roadmap. A macOS development machine may still act only as a
cross-compilation host for Android/OHOS.

OHOS remains a supported future target, but its production implementation is
intentionally deferred. Current work stays on Android playback correctness and
performance until the OHOS milestone is explicitly resumed.

The archival removes the former Apple backend options/targets and the
`VideoToolbox`/`Metal` public hardware-device identifiers. The unused Linux
backend placeholders and `VAAPI` identifier are removed as part of the same
three-platform support cleanup.

## Current baseline

The Qt-free core is functional and lives under `modern/`. The legacy QtAV code
has not been refactored or removed. QtAVCore builds independently with:

```sh
cmake -S modern -B build/modern
```

Continuation checkpoint:

- baseline implementation commit: `62e84956` (`Add Qt-free QtAVCore rewrite`);
- the latest completed scope connects `AudioSink` to `Player`, promotes a
  valid device clock to playback master, adds portable render/audio reference
  backends, and completes the Windows D3D11/WASAPI and Android
  Vulkan/OpenGL ES/AAudio/MediaCodec production paths;
- the D3D11VA device/frame/interop design and supplied-device core bridge are
  complete; the native `qtav_hw_d3d11va` decoder backend and
  `qtav_interop_d3d11` Video Processor path are complete, including
  same-device validation, SDR BGRA8 plus HDR RGB10/FP16 conversion, native
  H.264/NV12 and PQ/BT.2020 HEVC Main10/P010 zero-CPU-map rendering,
  Windows Advanced Color swap-chain/display tracking, lifecycle coverage,
  example wiring, installed target export, and strict native H.264/AAC
  playback through an active WASAPI render endpoint; the implementation,
  SDR-state tests, and active-HDR native display validation are complete;
- playback scheduling now isolates the FFmpeg demux/decode worker, bounded
  audio-output queue/worker, and bounded presentation queue/worker; UI/render
  callbacks cannot block device audio submission, `Player::position()` uses a
  cached device-clock snapshot, presentation refresh never waits behind a
  blocking sink write, repeated device samples are monotonically extrapolated
  only through submitted audio, and late video is dropped instead of building
  unbounded presentation latency; A/V startup and playing seeks use bounded
  video preroll, while playing seeks invalidate queues without blocking the
  caller and hold the clock in `Buffering` until actual new-generation output
  arrives; clock-capable audio must re-anchor after flush, audio underruns
  freeze the fallback clock until recovery, and
  HTTP(S) inputs use bounded read-timeout/reconnect defaults instead of
  immediately converting a recoverable disconnect into `Invalid`;
  the D3D11 Video Processor interop now reuses a bounded three-output texture
  pool instead of allocating a full-resolution output texture per frame and
  retires replaced pools without invalidating externally retained imports;
  D3D11 render attempts now use non-blocking player/render/context locks,
  preserve the imminent queued presentation frame under pressure, and release
  imported hardware frames immediately after ordered Video Processor/draw
  submission instead of retaining decoder surfaces behind per-frame GPU event
  queries;
  `QtAV::OutputD3D11` now owns the Windows device, composition swap chain,
  render target, display/HDR tracking, render scheduling thread,
  D3D11VA/interop wiring, `renderVideo()`, `Present()`, resize, and teardown;
  its default FP16 scRGB path resolves the hosting window's monitor without
  relying on unsupported composition-swap-chain `GetContainingOutput()`,
  tracks Windows HDR/SDR-white/luminance changes per frame, and exposes
  prefer-HDR, require-HDR, and SDR-only policies, while the WinUI 3 sample
  only supplies its HWND, binds its `SwapChainPanel`, attaches the player, and
  forwards size changes; the output caps frame latency at one, uses
  non-blocking `Present()` with bounded waitable-object backpressure on its
  private render thread, and exposes render/present plus per-stage timing
  statistics;
  its progress slider observes already-handled thumb pointer events and
  commits only one seek when a drag ends instead of issuing intermediate seeks;
  Milestones 5 and 6 are complete; the OHOS production path is intentionally
  deferred, and the Android example playback-stutter regression recorded
  below is fixed and connected-device verified; the shared Android/OHOS
  responsibility and lifecycle design is now recorded in `MOBILE.md`, and the
  Android arm64 Vulkan checkpoint cross-builds FFmpeg 8.1.2 plus QtAVCore,
  packages a minimal NativeActivity APK, verifies generated software A/V
  decode, and presents decoded software frames through a bounded three-frame
  platform-neutral Vulkan engine plus Android surface/swapchain adapter on a
  connected Android device, including SDR and native-HDR offscreen pixel
  goldens, required HDR10/PQ swapchain selection, static HDR metadata, and
  background/foreground HDR surface recreation; the shared OpenGL ES 3.x
  software renderer and Android EGL/window adapter are also complete with
  all advertised software upload families, SDR color/geometry readback, and
  real-window P010/PQ-to-SDR presentation plus exact RGB10_A2
  BT.2020/PQ native-HDR output recognized by the Android compositor; the
  platform-neutral mobile
  selector now keeps one renderer object attached across Vulkan-preferred
  startup, bounded Vulkan/OpenGL ES recovery, fatal one-way fallback, native
  window replacement, and the no-renderer state, and the Android harness uses
  it for real Vulkan HDR presentation/recreation plus forced initial EGL
  fallback; `QtAV::AudioAAudio` now adds negotiated Float32 device output,
  callback-safe bounded PCM buffering, AAudio presentation-clock and latency
  reporting, pause/flush/drain, route observation, and disconnect-triggered
  stream reconstruction, with the same device harness proving native output
  and device-master timing across background/foreground pause and resume;
  `QtAV::HWMediaCodec` now explicitly selects FFmpeg's H.264/HEVC MediaCodec
  wrappers, binds a versioned application `ANativeWindow`, and exposes
  single-decision present/drop output tokens; the connected harness covers
  seek/flush, media replacement, explicit stop, surface loss/reopen,
  stale-generation rejection, retained decoder lifetime, and shutdown without
  mapping decoded pixels; `QtAV::InteropMediaCodecVulkan` now adds a private
  GPU-sampled `AImageReader` producer, timestamp-correlated
  `AHardwareBuffer`/external-format import, Vulkan YCbCr sampling, explicit
  foreign-queue and sync-fd release fencing, and aligned-allocation cropping
  without decoded-pixel mapping, staging, transfer, or re-upload;
  `QtAV::InteropMediaCodecOpenGL` now supplies the detached
  private-`AImageReader`/AHardwareBuffer/EGLImage raw-YCbCr path, non-blocking
  bounded output scheduling, acquisition slack for coalesced image callbacks,
  and release fencing ordered after Android window presentation; and
  `QtAV::RenderMobile` connects fatal
  Vulkan hardware-frame fallback to an explicit synchronous application
  decision that rebinds subsequent output to compatible OpenGL ES interop or
  selects direct surface, software decode, or no video without retrying or
  mapping the retired frame;
- the Vulkan and OpenGL ES renderers now use libplacebo as their shader and
  color-pipeline authority. The former handwritten color-conversion,
  transfer, tone-mapping, gamut-mapping, and output-encoding shaders are no
  longer used. A shared FFmpeg/libplacebo bridge maps software frames and
  FFmpeg-parsed Dolby Vision RPU metadata; backend-owned shaders are limited
  to unavoidable representation normalization, including crop-aware raw
  Y/Cb/Cr normalization before libplacebo on Android hardware frames;
- QtAVCore now requires FFmpeg 8.0 or newer (libavcodec major 62+); compatibility
  branches for FFmpeg 5–7 are intentionally out of scope;
- `../ffmpeg/` now provides a pinned vcpkg dependency-build subproject for
  Android arm64/API 28 and OHOS arm64/API 23 cross-builds on macOS plus native
  Windows x64/Visual Studio builds. Its FFmpeg 8.1.2 policy enables OpenSSL,
  libsmb2, Vulkan, libass, libplacebo with OpenGL/OpenGL ES and built-in Dolby
  Vision reshaping, dav1d and native VVC decode while avoiding the unrelated
  desktop dependencies pulled by `ffmpeg[all]`; the
  Android, OHOS, and Windows dependency packages have been built, verified,
  and uploaded by self-hosted CI; the Windows validation uses Visual Studio
  18's clang-cl 22.1.3 with lld-link and preserves FFmpeg LTO;
- the root `README.md` and `AGENTS.md` now record the modern entry point and
  FFmpeg 8 minimum; legacy build guidance remains unchanged;
- Homebrew CMake 4.4.1 is installed at `/opt/homebrew/bin/cmake`;
- the last local FFmpeg used for verification was Homebrew FFmpeg 8.1.2.

Current public entry points:

- `modern/core/include/qtav/player.h`
- `modern/core/include/qtav/frame.h`
- `modern/core/include/qtav/color.h`
- `modern/core/include/qtav/media.h`
- `modern/core/include/qtav/video_render_api.h`
- `modern/core/include/qtav/audio_sink.h`
- `modern/core/include/qtav/audio_converter.h`
- `modern/core/include/qtav/hardware_decoder.h`
- `modern/core/include/qtav/hardware_frame.h`
- `modern/backends/audio/file/include/qtav/wav_audio_sink.h`
- `modern/backends/audio/wasapi/include/qtav/wasapi_audio_sink.h`
- `modern/backends/audio/aaudio/include/qtav/aaudio_audio_sink.h`
- `modern/backends/render/d3d11/include/qtav/d3d11_video_renderer.h`
- `modern/backends/output/d3d11/include/qtav/d3d11_video_output.h`
- `modern/backends/render/vulkan/include/qtav/vulkan_video_renderer.h`
- `modern/backends/render/vulkan/android/include/qtav/android_vulkan_video_renderer.h`
- `modern/backends/render/opengl/include/qtav/opengl_video_renderer.h`
- `modern/backends/render/opengl/android/include/qtav/android_opengl_video_renderer.h`
- `modern/backends/render/mobile/include/qtav/mobile_video_renderer.h`
- `modern/backends/hwaccel/d3d11va/include/qtav/d3d11va_hardware_decoder.h`
- `modern/backends/hwaccel/mediacodec/include/qtav/mediacodec_hardware_decoder.h`
- `modern/backends/interop/mediacodec_vulkan/include/qtav/mediacodec_vulkan_interop.h`
- `modern/backends/interop/mediacodec_opengl/include/qtav/mediacodec_opengl_interop.h`
- `modern/backends/interop/d3d11/include/qtav/d3d11_frame_interop.h`
- `modern/MOBILE.md`

Current implementation:

- `modern/core/src/player.cpp`
- `modern/core/src/frame.cpp`
- `modern/core/src/backend.cpp`
- `modern/core/src/hardware_decoder.cpp`
- `modern/backends/render/cpu/include/qtav/cpu_video_renderer.h`
- `modern/backends/render/cpu/src/cpu_video_renderer.cpp`
- `modern/backends/audio/resample/include/qtav/swresample_audio_converter.h`
- `modern/backends/audio/resample/src/swresample_audio_converter.cpp`
- `modern/backends/audio/file/include/qtav/wav_audio_sink.h`
- `modern/backends/audio/file/src/wav_audio_sink.cpp`
- `modern/backends/audio/wasapi/src/wasapi_audio_sink.cpp`
- `modern/backends/audio/aaudio/src/aaudio_audio_sink.cpp`
- `modern/backends/render/d3d11/src/d3d11_video_renderer.cpp`
- `modern/backends/render/vulkan/src/vulkan_video_renderer.cpp`
- `modern/backends/render/libplacebo/src/libplacebo_ffmpeg_bridge.c`
- `modern/backends/render/vulkan/android/src/android_vulkan_video_renderer.cpp`
- `modern/backends/render/opengl/src/opengl_video_renderer.cpp`
- `modern/backends/render/opengl/android/src/android_opengl_video_renderer.cpp`
- `modern/backends/render/mobile/src/mobile_video_renderer.cpp`
- `modern/backends/hwaccel/d3d11va/src/d3d11va_hardware_decoder.cpp`
- `modern/backends/hwaccel/mediacodec/src/mediacodec_hardware_decoder.cpp`
- `modern/backends/interop/mediacodec_vulkan/src/mediacodec_vulkan_interop.cpp`
- `modern/backends/interop/mediacodec_opengl/src/mediacodec_opengl_interop.cpp`
- `modern/backends/interop/d3d11/src/d3d11_frame_interop.cpp`
- `modern/backends/output/d3d11/src/d3d11_video_output.cpp`
- `modern/tests/audio_sink_player_test.cpp`
- `modern/tests/simulated_audio_sink.h`
- `modern/tests/simulated_audio_sink.cpp`
- `modern/tests/simulated_audio_sink_test.cpp`
- `modern/tests/simulated_audio_sink_player_test.cpp`
- `modern/tests/cpu_video_renderer_test.cpp`
- `modern/tests/audio_resample_player_test.cpp`
- `modern/tests/wav_audio_sink_test.cpp`
- `modern/tests/wav_audio_sink_player_test.cpp`
- `modern/tests/wasapi_audio_sink_test.cpp`
- `modern/tests/aaudio_pcm_queue_test.cpp`
- `modern/tests/d3d11_video_renderer_test.cpp`
- `modern/tests/vulkan_video_renderer_test.cpp`
- `modern/tests/vulkan_video_renderer_test_support.cpp`
- `modern/tests/opengl_video_renderer_test_support.cpp`
- `modern/tests/mobile_video_renderer_test.cpp`
- `modern/tests/d3d11va_hardware_decoder_test.cpp`
- `modern/tests/d3d11_frame_interop_test.cpp`
- `modern/tests/d3d11_video_output_test.cpp`
- `modern/tests/hardware_decode_device_test.cpp`
- `modern/examples/android/native_activity.cpp`
- `modern/examples/android/build-android.sh`
- `modern/examples/android/run-connected-device.sh`
- `modern/examples/android_player/AndroidManifest.xml`
- `modern/examples/android_player/CMakeLists.txt`
- `modern/examples/android_player/java/org/qtav/core/player/QtAVPlayerActivity.java`
- `modern/examples/android_player/native/player_jni.cpp`
- `modern/examples/android_player/native/android_vulkan_context.cpp`
- `modern/examples/android_player/build-android-player.sh`
- `modern/examples/android_player/run-connected-device.sh`

Current verification:

- after the 2026-08-02 Apple archival, both the Android arm64 NativeActivity
  harness and user-player native library reconfigure and link successfully;
  the changed generic backend/hardware-device tests compile with the Android
  toolchain, and a native macOS target is rejected by the new support gate
  with the archive location in its diagnostic;
- the current static and shared Windows builds pass 34/34 CTest tests,
  including the high-level D3D11 composition-output lifecycle test, Advanced
  Color test, WASAPI device test, and strict native H.264/AAC playback;
- the WinUI 3 sample, now using `QtAV::OutputD3D11`, sustains 24.9-25.1
  scheduled and rendered fps on the exercised 3840x2160 HEVC
  Main10/E-AC-3 HDR file; the migrated output was validated by seeking from
  35:31 back to 07:22 with both operations returning
  `Loaded -> Buffering -> Loaded` and resuming picture/audio. Earlier
  steady-state validation recorded zero coalesced redraws and zero
  video/render gaps over 80 ms, and a 15-second sample stayed within a bounded
  933-960 MiB working-set band instead of growing per frame. The same file now
  validates the completed high-level HDR path as D3D11VA
  P010/BT.2020/PQ -> FP16 scRGB -> an active Windows HDR layer, reporting
  system 240-nit SDR white and a 1405-nit display peak while sustaining
  25.0 fps;
- after removing per-frame D3D11 completion queries, the same 3840x2160
  Main10 HDR URL completed 12 alternating forward/backward seeks without
  audio/video freeze; stable scheduled/rendered cadence remained 24-25 fps,
  maximum draw time fell from the previously observed 155-194 ms to about
  0.5 ms, and maximum total render time stayed about 3-5 ms. Process private
  memory was 965.6 MiB before the seek run, 986.8 MiB after four seeks, and
  984.8 MiB after eight more seeks, while working set returned from a
  transient 886.4 MiB to 332.6 MiB, showing no per-seek linear growth;
- the former macOS/iOS validation record is preserved only in
  `archived_apple/README.md`; it is not current support evidence;
- the all-backends-disabled build passes 11/11 tests on Windows, including the
  Windows platform device-access contract test;
- forcing an unimplemented backend to `ON` fails with a clear diagnostic;
- invalid backend option values are rejected;
- installation and external `QtAV::RenderCPU`, `QtAV::AudioResample`, and
  `QtAV::AudioFile` CMake consumption pass;
- FFmpeg 8 minimum enforcement passes in the source and installed package;
- configuration without `pkg-config` passes;
- runtime linkage contains no Qt;
- core public-header scans contain no Qt, FFmpeg, or platform SDK types;
- MPEG-4/AAC, AC-3, E-AC-3, and TrueHD decode tests pass.
- on Windows with Visual Studio 2026 and vcpkg FFmpeg 8.1.2, static/shared
  Release tests cover the supplied hardware-device bridge, deterministic WARP
  texture/slice/lifetime/locking contracts, D3D11 software/imported/mapped
  rendering, native H.264/NV12 and HEVC Main10/P010 D3D11VA-to-Video-Processor
  presentation with verified pixels and no CPU mapping, plus pause/resume,
  seek, media replacement, stop, surface recreation, and retained-frame use
  after player shutdown;
- Windows Advanced Color coverage now includes deterministic PQ/HLG EOTF,
  BT.2020 conversion, SDR tone mapping, FP16 scRGB and RGB10/PQ numeric
  readback, native HWND and composition flip-model swap chains,
  `IDXGIOutput6`, `SetColorSpace1`, composition-monitor lookup, SDR-white
  lookup, and same-adapter display switching while Windows HDR is disabled
  and enabled. Active-HDR validation on a
  PHL 27B1U7903 reported a 10-bit G2084/P2020 output, system-derived 240-nit
  SDR white, 1405.11-nit peak luminance, and 1000-nit PQ output above scRGB
  `1.0`;
- the strict generated H.264/AAC native example test proves simultaneous
  D3D11VA decode, D3D11 rendering, audio decode, and audible WASAPI output
  through the active render endpoint;
- Windows multi-config FFmpeg imports select matching Debug/Release libraries,
  and project DLLs, tests, and examples share a runnable `bin/<Config>`
  directory;
- installation plus external CMake consumption of `QtAV::PlatformWindows`,
  `QtAV::HWD3D11VA`, `QtAV::RenderD3D11`, `QtAV::InteropD3D11`, and
  `QtAV::OutputD3D11`, and `QtAV::AudioWASAPI` together with the portable
  core, render, and audio targets passes for static and shared builds; the
  installed core token links without installing its private FFmpeg bridge
  header.
- on the recorded arm64 development host, NDK r28c cross-builds the pinned FFmpeg 8.1.2
  minimal libraries and QtAVCore for Android `arm64-v8a`; the signed
  NativeActivity APK passes 16 KB ELF segment-alignment checks and installs on
  an Android 16/API 36 device with an Adreno 830;
- the connected-device harness decodes its generated AVI through QtAVCore and
  reports `PASS` with 180 MPEG-4 video frames, 180 Vulkan-presented frames,
  282 PCM audio frames, a required native HDR10/PQ swapchain with
  `VK_EXT_hdr_metadata`, one background/foreground HDR surface recreation
  without media reopen, and platform-neutral offscreen pixel goldens covering
  three-frame ring reuse, viewport, rotation, target replacement, limited/full
  range, BT.601/BT.709 conversion, P010/BT.2020 PQ/HLG HDR-to-SDR conversion,
  native 10-bit PQ/HLG output, HLG-to-PQ conversion, mastering-display, MaxCLL,
  and default-luminance selection; install
  authorization was confirmed manually on the device after the harness
  stopped on `INSTALL_FAILED_USER_RESTRICTED`.
- the same Android 16/Adreno 830 harness now builds and runs
  `QtAV::RenderOpenGL` plus `QtAV::RenderOpenGLAndroid`: offscreen readback
  covers YUV420/422/444, NV12/NV21, P010, RGB/BGR, RGBA/BGRA/ARGB, Gray8,
  viewport, rotation, target-generation replacement, P010/PQ-to-SDR, and
  explicit BT.2020/PQ plus BT.2020/HLG numeric encoding. The real EGL window
  adapter proves both RGBA8/sRGB fallback and exact RGB10_A2 BT.2020/PQ
  presentation after Vulkan HDR playback and lifecycle validation; Android
  independently reports the EGL surface as an active HDR layer. The latest
  combined run passed with 179 decoded/presented video callbacks, one or more
  audio frames, and one background/foreground surface recreation.
- the Android selector checkpoint cross-builds `QtAV::RenderMobile`, keeps it
  attached during real Vulkan HDR playback and window replacement, emits a
  same-session Vulkan recovery transition, and proves forced
  Vulkan-unavailable startup through the real EGL adapter on the same Android
  16/Adreno 830 device; the connected run passed with 179 decoded/presented
  video frames and one background/foreground surface recreation.
- the Android AAudio checkpoint cross-builds and exports
  `QtAV::AudioAAudio`, links only the logical NDK `aaudio` system library,
  converts the generated PCM through `QtAV::AudioResample`, and passes on the
  same Android 16 device with 282 decoded audio frames and native output,
  48 kHz mono Float32 negotiation, a monotonic presentation clock, non-negative
  combined queued/device latency (334 ms in the final run), pause/resume,
  drain, background/foreground recovery, and no OpenSL ES fallback. The
  installed static target and external `find_package(QtAVCore)` consumer also
  pass.
- the Android MediaCodec checkpoint cross-builds and exports
  `QtAV::HWMediaCodec` against FFmpeg 8.1.2 with explicit
  `h264_mediacodec`/`hevc_mediacodec` wrappers. The final Android 16 device run
  produced 309 H.264 and 90 HEVC callbacks, explicitly presented 376 outputs
  and dropped 23, sought and flushed H.264, replaced it with HEVC, stopped
  HEVC at frame 90, replaced the surface once, rejected the stale generation,
  and completed NativeActivity shutdown without a crash or decoded-pixel map;
- Android install plus external CMake consumption of
  `QtAV::RenderMobile`, `QtAV::RenderOpenGLAndroid`,
  `QtAV::RenderOpenGL`, `QtAV::AudioAAudio`, `QtAV::HWMediaCodec`, and
  `QtAV::InteropMediaCodecVulkan` passes; the post-change install consumer
  also links and consumes `QtAV::InteropMediaCodecOpenGL`;
  the exported static targets use logical
  `nativewindow`/`EGL`/`GLESv3`/`aaudio`/`android`/`mediandk`/`vulkan` link
  names and contain no producer NDK or host path;
- the Android MediaCodec/Vulkan checkpoint cross-builds
  `QtAV::InteropMediaCodecVulkan` and enables Android hardware-buffer external
  memory, external semaphore fd, sampler YCbCr conversion, and the foreign
  queue family on the application-owned device. The Android 16/Adreno 830 run
  rendered both H.264 and HEVC from private `AImageReader` surfaces, importing
  89 external-format `AHardwareBuffer` images per codec and returning 89
  release sync fds per codec. The maximum pending image count was one and the
  decoded-source CPU-map, software-transfer, staging-copy, and renderer-upload
  counters all remained zero; 160x90 decoded crops were sampled correctly
  from the device's aligned 160x96 native allocations.
- the Android MediaCodec/OpenGL ES checkpoint cross-builds and exports
  `QtAV::InteropMediaCodecOpenGL`. On the same Android 16/Adreno 830 device,
  H.264 and HEVC decoded through private GPU-sampled `AImageReader` producers,
  imported retained AHardwareBuffers as EGLImages, sampled raw Y/Cb/Cr with
  `GL_EXT_YUV_target`, normalized the visible crop into RGBA16F, and then
  rendered through libplacebo. Independent phases completed 99 H.264 and 180
  HEVC imports with matching release fences, seek/surface-recreation coverage,
  and zero decoded-source CPU-map, software-transfer, staging-copy, or upload
  calls.
- the explicit MediaCodec renderer-fallback checkpoint adds
  `MobileHardwareFrameFallbackRoute` and a synchronous selector callback.
  Deterministic tests cover compatible OpenGL ES interop, direct-surface,
  software-decode, no-video, missing-policy, retired-surface rejection, and
  retryable asynchronous interop without a CPU map. The connected Android
  run injected a fatal Vulkan error after 30 successful AImageReader imports,
  rebound the same H.264 media session to a new OpenGL AImageReader producer,
  and continued with 180 OpenGL ES raw-YCbCr imports and matching release
  fences without retrying the retired Vulkan image or introducing a
  decoded-source map, transfer, staging, or upload.
- the Android player and NativeActivity builds now use the repository
  libplacebo package with Vulkan and OpenGL enabled (`pl_has_opengl=1`), while
  libdovi remains disabled because FFmpeg supplies parsed RPU metadata. Real
  Profile 5 playback of `/sdcard/Download/Wednesday.mp4` was validated through
  both AHardwareBuffer paths: native BT.2020/PQ HDR output when HDR is enabled,
  and libplacebo SDR tone mapping when HDR is disabled. Color/RPU/raw-import
  diagnostics remained active and decoded-source zero-copy counters remained
  zero.

## Milestone 0 — Qt-free playback core

- [x] Standalone modern CMake project.
- [x] C++17 public API without Qt or FFmpeg types.
- [x] PIMPL `qtav::Player`.
- [x] Asynchronous playback state machine.
- [x] Interruptible FFmpeg I/O.
- [x] FFmpeg send/receive audio and video decoding.
- [x] Reference-counted audio/video frame lifetime.
- [x] Prepare, play, pause, resume, seek, and stop.
- [x] Playback rate, range, and loop.
- [x] Media information and track enumeration.
- [x] Frame callbacks.
- [x] Render scheduling and pull-style `renderVideo()`.
- [x] Headless example and automated tests.
- [x] Installable CMake package.

## Milestone 1 — Repository and target decomposition

Goal: establish the long-term monorepo layout before platform code is added.

- [x] Move the current library implementation to `modern/core/`.
- [x] Preserve installed header paths under `include/qtav/`.
- [x] Preserve the `qtav_core` and `QtAV::Core` target names.
- [x] Add empty/optional backend roots:
  - `modern/backends/render/`
  - `modern/backends/audio/`
  - `modern/backends/hwaccel/`
  - `modern/backends/interop/`
- [x] Add `modern/platform/` for small shared OS helpers.
- [x] Add CMake `AUTO/ON/OFF` backend options.
- [x] Verify that configuring unsupported backends as `ON` fails clearly.
- [x] Verify the core still builds when every backend is disabled.

Acceptance:

- the existing API remains source-compatible and its tests continue to pass;
- no platform SDK header is reachable from core headers;
- static, shared, sanitizer, and install-consumer tests still pass.

## Milestone 2 — Backend contracts

Goal: define stable compile-time backend APIs before implementing a native
renderer or audio device.

### Video

- [x] Add a generic `VideoRenderAPI` contract.
- [x] Define surface size, viewport, aspect ratio, rotation, and redraw
  lifecycle.
- [x] Define whether a backend owns or borrows its device/context/surface.
- [x] Add backend capability reporting.
- [x] Keep concrete platform native-handle types out of generic public
  headers.
- [x] Add backend-specific headers for strong native types where useful.
- [x] Allow multiple renderer instances keyed by application opaque pointer.

### Audio

- [x] Add an `AudioSink` contract.
- [x] Define open, close, pause, flush, write, drain, latency, and clock
  reporting.
- [x] Define negotiated device format separately from decoded frame format.
- [x] Bind an optional sink to `Player` and use a valid device clock as the
  playback master with monotonic fallback.

### Hardware frames

- [x] Add a generic hardware-frame handle/lifetime representation.
- [x] Define CPU-map and native-handle queries.
- [x] Define interop capabilities without including D3D, Vulkan, OpenGL,
  Android, or OHOS headers in the core API.

Acceptance:

- mock video and audio backends can run in tests;
- backend callbacks can safely request player state changes;
- no C++ dynamic plugin ABI is committed yet.

### Review focus

- `VideoRenderAPI` lifecycle and resource-ownership fields;
- coexistence of the legacy `setVideoRenderer()` callback with multiple
  `VideoRenderAPI` instances keyed by application opaque pointers;
- `AudioSinkOpenResult` separating decoded and negotiated device formats;
- synchronous, non-owning `AudioBufferView` lifetime and plane layout;
- `HardwareFrameData`, reference-counted CPU mappings, and opaque
  `NativeHandle` representation;
- keeping backend calls outside the player mutex so backend callbacks may
  safely request player state changes.

## Completed portable reference backends

Completed scope:

1. [x] Add a `qtav_render_cpu` target backed by libswscale.
2. [x] Define an application-owned CPU image-buffer target and deterministic
   pixel-format conversion tests.
3. [x] Add a libswresample conversion backend that can satisfy a negotiated
   interleaved PCM `AudioFormat`.
4. [x] Connect that conversion path between decoded frames and `AudioSink`.
5. [x] Keep native renderers deferred until the portable reference paths pass
   tests.

Completed CPU rendering checkpoint:

- `QtAV::RenderCPU` is optional under `QTAV_RENDER_CPU=AUTO/ON/OFF` and links
  libswscale without adding it to `QtAV::Core`;
- `CpuImageBuffer` describes an application-owned positive-stride packed
  destination without exposing FFmpeg types;
- `CpuVideoRenderer` implements the `VideoRenderAPI` lifecycle and reports its
  initial full-surface `Stretch`/`Rotate0` capability;
- deterministic lossless-frame tests cover scaling to BGRA, RGBA, and Gray8,
  invalid stride rejection, and untouched row padding.

Completed audio conversion checkpoint:

- `AudioFrameConverter` keeps PCM conversion optional and injected, so
  `QtAV::Core` does not link libswresample;
- `QtAV::AudioResample` converts sample format, sample rate, and channel layout
  to an interleaved device format;
- `Player` opens the converter only when sink negotiation requires it, resets
  it on flush/seek, drains it at natural end, and closes it with the sink;
- deterministic PCM tests cover 8 kHz mono to 16 kHz stereo S16 conversion,
  exact drained sample count, continuous timestamps, and seek reset.

Completed audio integration checkpoint:

- `Player::setAudioSink()` binds or removes an optional sink;
- the first decoded audio frame opens it, exact-format frames are written
  without disturbing `onAudioFrame()`, and unsupported conversion reports a
  media event;
- pause/resume, seek, media replacement, stop, sink replacement, and shutdown
  drive the sink lifecycle without holding `Player::Impl::mutex_`;
- a supported, valid device clock is the playback master with monotonic
  fallback;
- deterministic mock tests cover device-clock use, backend event reentrancy,
  pause, seek, media replacement, stop, and shutdown.

Completed playback scheduling isolation checkpoint:

- FFmpeg demux and audio/video decode remain together on the asynchronous
  playback worker so control interruption and codec ownership stay simple;
- decoded audio crosses a bounded 500 ms queue to a dedicated audio-output
  worker, preventing render callbacks and UI work from starving the device;
- decoded video and frame/render notifications cross a bounded,
  timestamp-ordered presentation queue; stale late video is discarded when the
  application render path falls behind;
- device-clock reads are sampled by the audio-output worker and published as a
  generation-checked cache; presentation uses a try-lock refresh plus
  submitted-audio-bounded monotonic extrapolation, so neither video scheduling
  nor UI calls to `Player::position()` wait behind a sink write;
- a deterministic regression test blocks both the first sink write and the
  render callback, verifies that `position()` remains non-blocking, and verifies
  that audio writes continue while presentation is blocked;
- another deterministic regression holds a later sink write after a device
  clock has been established and verifies that video presentation continues
  from the cached clock rather than waiting on the sink serialization lock;
- shutdown pairs the quitting predicate with each worker condition-variable
  mutex before notification, closing the lost-wake window exposed by repeated
  audio-player destruction;
- initial output distinguishes an advertised but not-yet-valid device clock
  from seek/underrun re-anchoring: startup falls back after the first delivered
  buffer, while recovery continues waiting for a valid post-flush sample;
- a sink open that completes after stop, seek, or media-generation
  invalidation is closed instead of being published as the active output;
- callback-audio pre-presentation pacing can enqueue the first new-generation
  item without consuming the output-prime permit; only the presentation worker
  claims that permit, avoiding a frozen-clock deadlock on short range loops;
- separate packet-demux and per-stream decoder workers are intentionally
  deferred: the current bounded post-decode queues remove the observed
  cross-layer blocking without duplicating FFmpeg ownership. Revisit packet
  queues when production buffering, track switching, or live-stream recovery
  requires independent decode back-pressure.

## Completed deterministic portable audio validation

Completed before starting production platform backends:

1. [x] Add a test-only simulated `AudioSink` with configurable negotiated
   format, queue capacity, latency, and device-clock position.
2. [x] Model deterministic buffer consumption, pause/resume, flush, underrun,
   and end-of-stream drain without sleeping on wall-clock time.
3. [x] Add A/V master-clock tests covering resampling, simulated device
   latency, seek, loop, media replacement, and monotonic fallback.
4. [x] Decide whether the simulated sink should remain test-only or become an
   optional installed `qtav_audio_null` backend.
5. [x] Add an optional file/PCM diagnostic sink only after the timing contract
   is proven by the simulated sink.

Decision checkpoint:

- keep `SimulatedAudioSink` under `modern/tests/`; its manual clock controls,
  snapshots, and test synchronization API are intentionally not an installed
  playback backend;
- `AudioSink::drain()` now has a source-compatible default no-op, and `Player`
  drains queued sink audio after every completed playback segment, including
  loop boundaries, after draining the converter;
- deterministic tests cover capacity rejection, dynamic queued latency,
  pause/resume, flush/re-anchor, underrun de-duplication, drain, resampling,
  A/V device-master timing, seek, loop, media replacement, and invalid-clock
  monotonic fallback;
- a future `qtav_audio_null` target, if needed, should be a separate production
  pacing/diagnostic backend without exposing the simulator's test controls.

Completed file-output checkpoint:

- `QtAV::AudioFile` is optional under `QTAV_AUDIO_FILE=AUTO/ON/OFF`, has no
  dependency beyond `QtAV::Core`, and installs as an exported package target;
- `WavAudioSink` derives a requested interleaved PCM format from decoded audio,
  allowing the injected converter to handle planar input, resampling, and
  channel conversion;
- the sink writes little-endian RIFF/WAVE output, updates header sizes on close,
  flushes on seek without discarding captured samples, and intentionally
  reports no device clock;
- deterministic tests verify header fields, sample byte order, invalid planar
  negotiation, and an exact 64,000-byte converted player capture.

## Android manual final-test player gate

This user-requested Android player gate is complete. Its results remain the
baseline for the active Android stutter regression; the Milestone 7 OHOS gate
is now deferred:

1. [x] Add a user-facing standard Android activity under
   `modern/examples/android_player/` with an upper video surface, current
   time/seek/duration row, local and remote file opening, play/pause/stop, and
   live Vulkan, HDR, ZeroCopy, software/hardware decode, and Vulkan validation
   layer controls.
2. [x] Connect the switches to real native paths: software decode through
   Vulkan/OpenGL ES, MediaCodec direct-Surface output, private-AImageReader
   Vulkan ZeroCopy, private-AImageReader/AHardwareBuffer/EGLImage OpenGL ES
   ZeroCopy, explicit SDR/HDR output policy, and exact optional
   `VK_LAYER_KHRONOS_validation` enablement.
3. [x] Add a reproducible arm64 NDK, OpenSSL 3.5.7, and FFmpeg 8.1.2 build,
   Java/D8 packaging, debug signing, 16 KB ELF alignment, and an install
   script that stops before `adb install` unless the user explicitly confirms
   the device is ready.
4. [x] Cross-build and package
   `build/android-player/qtav-core-player.apk`; verify the API
   28/target-36 manifest, launchable activity, DEX/native payload, system-only
   dynamic linkage, 16 KB load-segment alignment, v3 debug signature, and the
   default pre-install stop. No device installation was performed.
5. [x] Rebuild the player with checksum-pinned OpenSSL 3.5.7 and FFmpeg
   networking enabled, pass HTTP/HTTPS URLs directly to QtAVCore/FFmpeg,
   enforce HTTPS peer/host verification against Android's system CA
   directory, and re-run the APK/package checks without installing it. The
   packaged static backend reports OpenSSL 3.5.7 and FFmpeg
   `file/http/https/tcp/tls/udp`, has system-only dynamic dependencies and
   16 KB ELF load alignment, includes the third-party license texts, passes
   the API 28/target-36 manifest and v3-signature checks, and stops at the
   pre-install gate. No device installation was performed.
6. [x] After the user confirmed that only the first physical-device approval
   was required, install once and run the local/remote, seek/lifecycle,
   renderer, HDR, ZeroCopy, software/hardware decode, and debug-layer matrix.
   The connected Android 16/Adreno 830 device opened the 4.93 GB
   `Download/legend.mkv` through the SAF `fd:` protocol and streamed the same
   3840x2160, 45:44 HDR file directly from
   `https://2dland.cn/test/legend_of_the_magnate.mkv` through FFmpeg/OpenSSL.
   OpenSSL peer/host verification succeeded with an app-private PEM bundle
   assembled from Android's system roots. Vulkan/AImageReader ZeroCopy showed
   visible video and a native RGB10/BT.2020-PQ compositor layer with HDR
   metadata; a remote sample reached 874 decoded/436 presented frames, while
   MediaCodec direct-Surface reached 186/186. The 8-bit H.264 sample passed
   SurfaceTexture/OpenGL ES ZeroCopy at 180/179, software OpenGL ES at 180/180,
   and software Vulkan at 103/103 when paused mid-file. HDR-off rebuilt an SDR
   Vulkan surface, pause/resume, slider seek, stop/replay, SAF descriptor
   rewind, and background/foreground surface recreation passed. The device has
   no `VK_LAYER_KHRONOS_validation`; enabling Debug layer produced the exact
   unavailable-layer error, and disabling it recovered. P010/HDR
   SurfaceTexture and planar `yuv420p10le` software decode now stop with clear
   capability errors instead of continuing behind a black surface.
7. [x] Diagnose and fix the long-form 4K frame-loss path. MediaCodec sustained
   the source rate, but the demo rendered inline on the playback worker,
   ignored the interop `RedrawRequested` event, and later replaced an
   AImageReader-pending frame with the player's newer current frame. The demo
   now owns a serialized native render thread, queues reference-counted frames,
   retains the exact active frame across asynchronous import retries, and
   reports callback, presentation, and render-attempt counters separately.
   AImageReader now accepts the valid zero timestamp used by the first frame;
   SurfaceTexture explicitly drops that one ambiguous output rather than
   retrying forever. On the connected Android 16/Adreno 830 device,
   `suzume.mkv` reached 1292/1292 callbacks/presentations at 00:57 and
   `legend.mkv` reached 1307/1307 at 00:52 through 4K MediaCodec/AImageReader
   Vulkan HDR ZeroCopy, with about 4% app CPU. The H.264 SurfaceTexture
   regression completed at 180/179 after its documented zero-PTS drop.
8. [x] Fix the OpenGL ES ZeroCopy reopen/long-form regression without changing
   the completed core worker split. The player now reserves four exact-frame
   slots including the graphics-thread in-flight attempt; OpenGL releases
   MediaCodec output non-blockingly; AImageReader keeps two acquisition slots
   beyond its four-image correlation window and immediately returns callback
   evictions; and `OpenGLPresentCallback` submits the EGL window before the
   interop exports its native release fence. On the same Android 16/Adreno 830
   device, a Release APK reopened `wednesday.mp4` and sustained 3,632 callbacks
   / 3,626 presents at 23.8 fps with core queue/late drops `0/0` and no AAudio
   underrun. Pause/resume, a seek to 13:17, and background/foreground surface
   recreation resumed at 24.0 fps. `legend.mkv` sustained 25.0 fps with
   1,454/1,450 callbacks/presents and core drops `0/0`; Debug-off playback
   continued beyond 01:42. The full connected NativeActivity matrix also
   passed, including OpenGL H.264/HEVC raw-YCbCr import, seek, surface
   recreation, native release fences, Vulkan-to-OpenGL fallback, zero decoded
   source map/transfer/staging/upload, and clean shutdown.

## Next task

The Android example playback-stutter task is complete:

1. [x] Reproduced the issue in a Release APK with Debug visible and hidden,
   using ordinary HDR `legend.mkv` and Dolby Vision Profile 5
   `wednesday.mp4`, including the failure after fully reopening the app.
2. [x] Measured the failing stage: successful GL work stayed within budget,
   while callbacks outran presents, AImageReader acquisition/release depth
   grew to its bound, the EGL release fence excluded the later window swap,
   and the resulting video-packet backpressure eventually produced an AAudio
   underrun.
3. [x] Implemented the bounded, non-blocking correction while preserving exact
   frame correlation, graphics-thread ownership, native-buffer/fence lifetime,
   libplacebo processing, and zero decoded-source CPU copies.
4. [x] Validated sustained source-rate playback, app close/reopen, Debug
   on/off, pause/resume, seek, and surface recreation on the connected device
   for both representative files.

No further Android correctness task is currently queued. OHOS production work
remains intentionally deferred until the user explicitly resumes Milestone 7;
until then, preserve this OpenGL reopen/long-form run as an Android regression
gate.

Completed platform prerequisites:

Windows Advanced Color validation is complete: on a PHL 27B1U7903,
`qtav_render_d3d11_advanced_color_test` passed with
`QTAV_REQUIRE_ACTIVE_HDR=1`, active G2084/P2020 output, an FP16 G10/P709 scRGB
swap chain, system SDR white and panel luminance queries, and preservation of
the 1000-nit PQ sample above scRGB `1.0`; static and shared Windows CTest runs
passed 34/34.

1. [x] Complete the shared Android/OHOS mobile design checkpoint below before
   adding either platform's hardware decoder.
2. [x] Establish reproducible Android NDK and FFmpeg 8+
   cross-builds, package a minimal native test application, and run it on a
   connected arm64 Android device.
3. [x] Add the reusable Vulkan renderer engine and Android surface adapter,
   keeping window, device, and swapchain ownership outside core.
4. [x] Add native Vulkan HDR output before advancing the mobile plan: make the
   current-target color space explicit, implement HDR10/PQ, HDR10/HLG, and
   extended-linear shader output, select Android HDR swapchains, submit static
   HDR metadata, and validate 10-bit goldens plus a real HDR device lifecycle.
5. [x] Add the shared OpenGL ES 3.x renderer and Android EGL/window adapter
   defined in `MOBILE.md`, sharing color/geometry semantics with Vulkan where
   practical without hiding incompatible API lifecycles.
6. [x] Add the application/platform renderer selector. Validate
   Vulkan-unavailable, initial-failure, fatal-runtime-failure, recoverable
   same-API recreation, one-way fallback, and both-backends-unavailable cases.
7. [x] Extend the OpenGL ES fallback to native HDR: make the render-target
   color space explicit, encode BT.2020/PQ and BT.2020/HLG, select and verify
   exact RGB10_A2 EGL surfaces and Android dataspaces, retain explicit
   RGBA8/sRGB tone-mapping fallback, and verify compositor HDR-layer
   recognition on the connected device.
8. [x] Add AAudio output and device-clock/latency validation.
9. [x] Add MediaCodec H.264/HEVC hardware decode and direct-surface
   presentation with deterministic present/drop, seek/flush, stop, media
   replacement, surface recreation, stale-generation rejection, and shutdown.
10. [x] Add the confirmed private, GPU-sampled
    `AImageReader`/`AHardwareBuffer` Vulkan zero-CPU-copy texture path.
11. [x] Add the confirmed private-AImageReader/AHardwareBuffer/EGLImage raw
    YCbCr OpenGL ES zero-CPU-copy texture path.
12. [x] Connect the explicit renderer-fallback policy: on a fatal Vulkan
    transition, reconfigure subsequent MediaCodec output through compatible
    GLES native interop or select the caller's direct-surface,
    software-decode, or no-video policy without mapping a hardware frame.

### Shared Android/OHOS mobile design checkpoint

Complete this checkpoint once and reuse it for both mobile production paths:

1. [x] Define a platform-neutral Vulkan renderer engine for software-frame
   mapping, libplacebo-generated color/render shaders, viewport, aspect ratio,
   rotation, synchronization, and retained in-flight resources.
2. [x] Keep Android `ANativeWindow`/EGL/Vulkan objects and OHOS
   `OHNativeWindow`/XComponent/EGL/Vulkan objects in separate platform or
   backend-specific adapters; do not expose either SDK through core public
   headers or merge the two SDK lifecycles into one platform class.
3. [x] Reuse the FFmpeg/libplacebo frame bridge, output-color contracts,
   geometry rules, capability rules, golden test vectors, and renderer
   contract tests between Android and OHOS. Color-conversion and tone-mapping
   shaders come from libplacebo; share only representation-normalization code
   where the API and resource-lifetime behavior actually match.
4. [x] Define a surface-backed hardware-decode presentation contract shared
   by MediaCodec and FFmpeg 8 OHCodec: explicit decoder selection, frame
   present/drop, playback-clock scheduling, bounded outstanding buffers,
   seek/flush, stop, media replacement, surface loss/recreation, and retained
   frame lifetime.
5. [x] Preserve separate targets for hardware decode, hardware-frame interop,
   rendering, and audio output. Android uses MediaCodec/AAudio; OHOS uses
   OHCodec/OHAudio. Shared code must not introduce a lowest-common-denominator
   platform ABI.
6. [x] Create reusable device-test media and lifecycle scenarios, plus thin
   platform-specific APK/HAP launch, signing, deployment, log collection, and
   result adapters for connected-device validation from development hosts.
7. [x] Define Vulkan as the preferred Android/OHOS software renderer and
   OpenGL ES/EGL as its required fallback. Keep recoverable surface recreation
   within the active API, switch one-way to OpenGL ES after fatal Vulkan
   failure, keep playback alive if both renderers fail, and leave selection in
   the application/platform layer rather than core.
8. [x] Define zero-CPU-copy native-buffer interop for both Vulkan and OpenGL ES:
   no CPU map, software-frame transfer, staging copy, or re-upload; explicit
   native-buffer lifetime and fence synchronization; capability-gated format
   support; and independent decoder, interop, and renderer fallback policies.

Accepted design and Android foundation checkpoint:

- [`MOBILE.md`](MOBILE.md) fixes the shared Vulkan engine, the OpenGL ES
  fallback engine and selection/recovery policy, separate Android/OHOS
  Vulkan/EGL surface adapters, direct-surface hardware-output state machine,
  Vulkan/OpenGL ES zero-CPU-copy interop contract, audio boundaries, reusable
  lifecycle scenarios, and on-device installation authorization gate;
- the Android harness pins FFmpeg 8.1.2 by checksum, NDK r28c, API 28,
  compile SDK 36, SDK CMake 4.1.2, and build-tools 37.0.0 while allowing
  explicit installed-tool overrides;
- its minimal LGPL-compatible FFmpeg configuration enables AVI, MPEG-4 video,
  PCM S16 audio, file I/O, libswscale, and libswresample, then statically links
  QtAVCore into one NativeActivity shared library without Qt;
- AAPT2, zipalign, Android Studio's JBR, and apksigner produce a debug APK
  without a Gradle dependency; generated sources, libraries, media, keys,
  package output, logs, and device facts remain under `build/android/`;
- deployment requires exactly one authorized device, runs installation once,
  and stops for manual device confirmation on failure rather than retrying or
  bypassing a modern Android/OHOS authorization prompt;
- the first connected device is model `2410DPN6CC`, Android 16/API 36,
  `arm64-v8a`, Adreno 830, Vulkan 1.3.284, and OpenGL ES 3.2; generated
  software playback now passes with 180 video and 282 audio frames plus one
  background/foreground surface recreation.

Completed Android Vulkan implementation checkpoint:

- `QtAV::RenderVulkan` is now a platform-neutral backend target with borrowed
  physical/logical device and queue handles plus an application-supplied
  current-image target; Vulkan declarations remain in its backend-specific
  header and do not reach core public headers;
- the shared FFmpeg/libplacebo bridge maps YUV420/422/444, NV12/NV21, P010,
  RGB/BGR/RGBA/BGRA/ARGB, and Gray8 software frames, including structured
  color and Dolby Vision RPU metadata. libplacebo generates the Vulkan shaders
  and owns color conversion, scaling, reshape, tone/gamut mapping, and final
  encoding; the current-target contract carries `VkColorSpaceKHR` and selects
  SDR sRGB, native HDR10/PQ, native HDR10/HLG, extended-linear sRGB, or linear
  BT.2020 output;
- the engine uses a bounded three-frame in-flight ring and retains each source
  `VideoFrame` until its slot fence completes;
- `QtAV::RenderVulkanAndroid` retains the active `ANativeWindow` generation
  and owns only its Android `VkSurfaceKHR`, swapchain, image views, and
  per-frame acquire/present semaphores while borrowing the
  application-created Vulkan instance, device, and graphics/present queue; it
  exposes prefer-HDR, require-HDR, and SDR-only policies, reports its selected
  surface format/HDR state, and submits frame-derived `VK_EXT_hdr_metadata`
  when that borrowed-device extension was enabled;
- the NativeActivity harness now selects a presentation-capable queue, creates
  the borrowed Vulkan context, renders decoded YUV420P frames through the
  Android adapter, and passes on the recorded Adreno 830 device with 180/180
  frames submitted and presented plus 282 decoded audio frames;
- the deployment test backgrounds and resumes the same NativeActivity,
  invalidates and rebuilds the active window/swapchain generation, and
  continues playback without reopening media;
- platform-neutral offscreen image readback checks exercise six queued
  submissions across the three-frame ring, Fit letterboxing, custom viewport,
  180-degree rotation, target-generation replacement, limited/full-range
  YUV, BT.601/BT.709 matrices, and numeric P010/BT.2020 PQ/HLG
  HDR-input-to-SDR goldens, then switch to packed 10-bit targets to verify
  native HDR10/PQ, HLG-to-PQ, and native HDR10/HLG code values, plus FP16
  targets to verify extended-linear sRGB and linear BT.2020 values above the
  100-nit reference white;
- HDR checks verify mastering-display maximum luminance takes precedence over
  MaxCLL, MaxCLL is used when mastering luminance is absent, and HDR input
  without either uses the renderer's documented 1000-nit default;
- the NativeActivity enables `VK_EXT_swapchain_colorspace`, enables
  `VK_EXT_hdr_metadata` when exposed, requires an implemented HDR
  format/color-space pair, and passes on the recorded Adreno 830 with an
  `A2B10G10R10`/HDR10-ST2084 swapchain, Android compositor HDR-layer
  recognition, presentation of a synthetic P010/BT.2020/PQ frame carrying
  mastering/MaxCLL metadata, and surface recreation;
- a standalone Android install exports `QtAV::RenderVulkan` and
  `QtAV::RenderVulkanAndroid` with their installed headers and Vulkan
  dependency metadata without embedding the local NDK or build-tree path; an
  external Android consumer compiles the installed HDR preference and
  selected-surface query API.

Completed Android OpenGL ES fallback checkpoint:

- `QtAV::RenderOpenGL` is a reusable OpenGL ES 3.x `VideoRenderAPI` target
  that draws into a caller-supplied current framebuffer and keeps EGL/window
  ownership outside the engine;
- the shared FFmpeg/libplacebo bridge covers YUV420/422/444, NV12/NV21,
  little-endian P010, RGB/BGR/RGBA/BGRA/ARGB, and Gray8 software frames.
  libplacebo's OpenGL backend generates the color/render shaders and owns
  range, matrix, transfer, primaries, scaling, reshape, tone/gamut mapping,
  and final encoding while QtAVCore supplies the common Fit/Fill/Stretch,
  custom viewport, and right-angle rotation contract;
- `OpenGLRenderTarget` explicitly selects SDR sRGB, BT.2020/PQ, or
  BT.2020/HLG output. libplacebo tone maps HDR/Dolby Vision to SDR when the
  target is SDR; HDR targets preserve HDR intent and emit the selected native
  transfer/primaries encoding;
- `QtAV::RenderOpenGLAndroid` retains the active `ANativeWindow` generation
  and owns its EGL display, OpenGL ES 3.x context, window surface, and swap
  while keeping Android/EGL declarations out of core public headers. Its
  prefer/require/SDR-only policy tries exact RGB10_A2 BT.2020/PQ, considers
  HLG when exposed, and falls back explicitly to RGBA8/sRGB, verifying the EGL
  colorspace and Android buffer dataspace;
- Android offscreen readback covers every advertised software family,
  viewport, rotation, target-generation replacement, P010/PQ-to-SDR, and
  numeric BT.2020/PQ plus BT.2020/HLG output. The real window adapter proves
  both the RGBA8/sRGB fallback and exact RGB10_A2/BT.2020-PQ presentation
  after the Vulkan HDR playback/lifecycle checks, and Android reports the EGL
  surface as an active HDR layer;
- the engine and Android adapter do not implement automatic API selection.
  `QtAV::RenderMobile` implements startup probing, bounded same-API recovery,
  fatal one-way fallback, and the no-renderer state without absorbing either
  graphics API's native lifecycle.

Completed mobile renderer selector checkpoint:

- `QtAV::RenderMobile` is a platform-neutral, optional target whose factories
  create a fully prepared Vulkan or OpenGL ES `VideoRenderAPI` for the current
  application-owned native-window generation;
- each new session prefers Vulkan, records explicit startup-unavailable/open
  failure reasons, and selects OpenGL ES without exposing either graphics API
  or a platform SDK through its installed header;
- `SurfaceLost` performs a bounded number of complete same-API recreations,
  while `Error` is fatal; fatal or repeatedly unrecoverable Vulkan is retired
  for the session before the retained frame is retried through OpenGL ES;
- `suspendSurface()` and `recreateSurface()` preserve the active API across
  an application-led native-window replacement, and the selector remains
  attached to `Player`, so recovery and fallback do not reopen media;
- Android EGL context, display, native-window, and surface loss are classified
  as recoverable lifecycle events before the selector gives up on OpenGL ES;
- deterministic mock-adapter tests cover Vulkan unavailable, initial Vulkan
  open failure, fatal runtime failure, recoverable Vulkan and OpenGL ES
  recreation, recovery exhaustion, one-way fallback, window replacement, and
  both backends unavailable;
- the Android NDK harness cross-builds the installed API, uses the selector for
  real Vulkan HDR playback and background/foreground recovery, and validates
  forced initial fallback through the real OpenGL ES adapter.

Completed Android AAudio checkpoint:

- `QtAV::AudioAAudio` is Android-only, optional under
  `QTAV_AUDIO_AAUDIO=AUTO/ON/OFF`, installable as an exported package target,
  and available on API 26 or newer without adding Android declarations to
  core public headers;
- `AAudioAudioSink` negotiates low-latency shared-mode Float32 mono/stereo PCM
  against the selected or default output device, allowing
  `QtAV::AudioResample` to convert decoded sample format, rate, or layout;
- accepted PCM crosses a fixed-capacity SPSC ring into the AAudio data
  callback. The callback allocates no memory, acquires no lock, sleeps never,
  and performs no stream lifecycle or application callback operation;
- AAudio presentation timestamps map the native frame position to the media
  timeline; reported latency combines native pipeline frames and queued PCM,
  and underrun/flush recovery invalidates the clock until a new audio buffer
  establishes an anchor;
- pause, flush, drain, xrun diagnostics, transparent route-ID observation,
  close, and disconnect handling are implemented. A non-callback worker
  rebuilds a disconnected default-route stream with the same negotiated PCM
  format and reports buffering or terminal device loss explicitly;
- portable queue tests cover capacity, wraparound, timestamp continuity,
  rejection, and reset. The Android 16 device harness passes with
  generated-audio device output, 48 kHz mono Float32 output, 282 decoded audio
  frames,
  monotonic clock samples, non-negative combined latency, pause/resume,
  background/foreground transition, drain, and clean close;
- API 28 device coverage does not require OpenSL ES. Static install/export and
  external `find_package(QtAVCore)` consumption of `QtAV::AudioAAudio` pass
  without embedding an NDK or build-tree path.

Completed Android MediaCodec direct-surface checkpoint:

- `QtAV::HWMediaCodec` is Android-only, optional under
  `QTAV_HW_MEDIACODEC=AUTO/ON/OFF`, installable as `QtAV::HWMediaCodec`, and
  keeps `ANativeWindow`, NDK Media, and FFmpeg declarations outside core
  public headers;
- `MediaCodecSurface` retains the application window and assigns a new
  nonzero generation to every published token. Its configuration creates the
  supplied FFmpeg MediaCodec hardware device and selects the explicit
  `*_mediacodec` wrapper rather than the ordinary software decoder;
- each decoded output is exposed through a move-only `MediaCodecFrame`.
  Immediate present, `CLOCK_MONOTONIC` timed present, and drop are explicit,
  single-decision operations; undecided final frame release drops through
  FFmpeg;
- generic hardware-frame storage retains the decoder context through copied
  frame lifetime. Queue invalidation can therefore release old MediaCodec
  outputs after seek, stop, media replacement, or surface loss without
  dereferencing a destroyed codec;
- the Android 16/Adreno 830 device harness passes generated H.264 and HEVC
  hardware decode with present and drop, H.264 seek/flush, H.264-to-HEVC media
  replacement, explicit HEVC stop, background/foreground surface reopen,
  stale-generation rejection, and clean NativeActivity shutdown. No decoded
  pixel map, transfer, staging copy, or renderer upload occurs in this direct
  path;
- Android cross-build, APK signing, install/export metadata, and external
  `find_package(QtAVCore)` consumption include `QtAV::HWMediaCodec` and its
  logical `android`/`mediandk` dependencies.

Completed Android MediaCodec/Vulkan interop checkpoint:

- `QtAV::InteropMediaCodecVulkan` is an independent Android target under
  `QTAV_INTEROP_MEDIACODEC_VULKAN=AUTO/ON/OFF`; it depends on
  `QtAV::HWMediaCodec` and `QtAV::RenderVulkan` without combining decoder,
  renderer, and platform-window ownership;
- `MediaCodecVulkanInterop` owns a private `AIMAGE_FORMAT_PRIVATE`
  `AImageReader` created with `AHARDWAREBUFFER_USAGE_GPU_SAMPLED_IMAGE`, gives
  its versioned producer window to the MediaCodec backend, and correlates the
  codec token timestamp with the asynchronously acquired `AImage` timestamp;
- supported retained `AHardwareBuffer` images are imported through
  `VK_ANDROID_external_memory_android_hardware_buffer`, external-format
  sampler YCbCr conversion, and foreign-queue ownership barriers. The Vulkan
  renderer accepts the imported image as a combined sampler, applies the same
  color/geometry/output path, and uses the `AImage` crop rectangle when the
  native allocation has padded dimensions;
- acquire sync fds are imported as temporary Vulkan semaphores when the
  producer supplies them. Every submitted image signals an exportable release
  semaphore whose sync fd is returned through `AImage_deleteAsync()`; the
  imported image, buffer, conversion resources, and codec output stay retained
  through GPU submission completion;
- timestamp queues and acquired images are bounded by the reader's configured
  image count; stale and unsupported inputs are rejected without
  `AHardwareBuffer_lock*()`, software-frame transfer, CPU staging, or renderer
  upload;
- the connected Android 16/Adreno 830 harness requires the four Vulkan
  capabilities, decodes generated H.264 and HEVC, and records 89
  external-format imports plus 89 returned release fences for each codec with
  a maximum pending depth of one and all four CPU-copy counters at zero;
- Android static/shared arm64 cross-builds pass; static install/export and
  external `find_package(QtAVCore)` consumption of
  `QtAV::InteropMediaCodecVulkan` pass.

Completed structured color metadata checkpoint:

- `VideoFrame` exposes toolkit-independent structured range, primaries,
  transfer, matrix, and chroma-location values while retaining the diagnostic
  `colorSpace()` string;
- HDR10 mastering-display chromaticities/luminance and content-light levels
  are copied from FFmpeg frame side data into reference-counted public values;
- active Windows and Android renderers consume the same values for
  limited/full range, BT.601/709/2020 conversion, PQ/HLG, target encoding, and
  deterministic HDR validation.

The completed former Apple checkpoints were moved to
[`../archived_apple/`](../archived_apple/) and are no longer active milestones.

Next active implementation order:

1. [x] Add backend-specific Windows headers for strong borrowed D3D11 types.
2. [x] Add `qtav_render_d3d11` and render software frames first.
3. [x] Add resize, viewport, aspect-ratio, rotation, surface recreation, and
   device-loss tests.
4. [x] Add WASAPI.
5. [x] Complete the D3D11VA device, frame-lifetime, and interop design
   checkpoint below.
6. [x] Add D3D11VA decode and D3D11 zero-copy interop.

Completed D3D11 software-frame checkpoint:

- `QtAV::RenderD3D11` is Windows-only, optional under
  `QTAV_RENDER_D3D11=AUTO/ON/OFF`, and keeps D3D11/DXGI/WRL types and headers
  inside its backend target and backend-specific public header;
- `BorrowedD3D11Device` and `BorrowedD3D11DeviceContext` make native resource
  roles explicit, while `D3D11CurrentTargetCallback` obtains the current
  application-owned render-target view for every frame;
- the renderer uploads YUV420/422/444, NV12/NV21, P010,
  RGB/BGR/RGBA/BGRA/ARGB, and Gray8 software frames into D3D11 shader
  resources and applies limited/full-range BT.601/BT.709/BT.2020 conversion,
  PQ/HLG EOTF, linear primaries conversion, and display-aware tone mapping;
- BGRA8/RGBA8 SDR, FP16 scRGB, and RGB10/PQ targets are supported; an optional
  borrowed `IDXGISwapChain3` enables per-frame `IDXGIOutput6` capability
  discovery, SDR-white lookup, `SetColorSpace1`, and display-switch handling;
- Fit, Fill, Stretch, custom viewports, all right-angle rotations, resize,
  render-target recreation, foreign-device rejection, and missing-surface or
  device-removal event classification are implemented;
- deterministic WARP offscreen tests cover RGB24, YUV420P, NV12, PQ/HLG P010,
  SDR tone mapping, FP16 scRGB/RGB10 numeric output, viewport, aspect,
  rotation, resize, surface recreation, and error handling; a native
  flip-model test covers the current output and same-adapter display moves;
- Windows multi-config discovery now maps vcpkg FFmpeg Debug and Release
  libraries correctly, and a common runtime directory makes shared-library
  tests and examples directly runnable.

Completed WASAPI checkpoint:

- `QtAV::AudioWASAPI` is Windows-only, optional under
  `QTAV_AUDIO_WASAPI=AUTO/ON/OFF`, installable as an exported package target,
  and keeps Windows SDK, COM, and WASAPI types out of core public headers;
- `WasapiEndpointId` owns an optional endpoint identifier so the sink can
  select a device without transferring an apartment-bound `IMMDevice`; an
  empty identifier follows the current default multimedia render endpoint;
- the sink negotiates shared-mode interleaved Float32 mono/stereo PCM at the
  endpoint mix rate and uses the injected `QtAV::AudioResample` backend for
  decoded planar, sample-rate, or channel-layout conversion;
- a dedicated MMCSS thread owns COM and the event-driven audio client while a
  bounded backend queue copies accepted PCM independently of decoded-frame
  lifetime;
- pause/resume, seek flush, natural-end drain, endpoint invalidation, bounded
  backpressure, underrun recovery, and close are synchronized without calling
  native interfaces from `Player::position()`;
- cached `IAudioClock` position is anchored to media timestamps, reported
  latency combines engine padding and stream latency, and an underrun
  invalidates the device clock until the next accepted buffer re-anchors it;
- device integration tests cover capabilities, invalid input, shared-mode
  format negotiation, pause/resume, write, clock/latency, drain, flush,
  re-anchor, and close; the Windows console example exercises
  `Player`/libswresample/WASAPI playback with generated MPEG-4/AAC media.

D3D11VA and zero-copy design checkpoint:

Completed after WASAPI and before implementing `qtav_hw_d3d11va` or
`qtav_interop_d3d11`. The accepted contract and implementation order are in
[`D3D11VA.md`](D3D11VA.md).

1. [x] Decide how the hardware decoder receives the application-selected
   D3D11 device. Prefer decoding on the same borrowed device used by the
   renderer so the normal path does not require a cross-device copy.
2. [x] Define backend-specific retained-frame access to both the
   `ID3D11Texture2D` and its array slice while keeping D3D11 and FFmpeg types
   out of core public headers.
3. [x] Define device-context locking, playback-worker access, render-thread
   access, and frame-pool lifetime using the FFmpeg 8 D3D11VA device and
   frames-context contracts.
4. [x] Keep hardware decode, D3D11 texture interop, D3D11 Video Processor
   operations, and final rendering as separate responsibilities and targets
   where the existing module boundaries require them.
5. [x] Specify explicit software-map/copy fallback, foreign-device rejection,
   seek and flush behavior, device removal, surface recreation, and retained
   frame lifetime after player shutdown.
6. [x] Define deterministic WARP coverage for contracts and error paths plus
   real-GPU integration coverage for hardware decode and zero-copy texture
   rendering.
7. [x] Treat Aleksoid1978/VideoRenderer as an isolated GPL-3.0 behavioral
   reference only. Do not vendor it, link it, or copy its C++, shaders, data
   tables, or vendor-specific extensions; implement against FFmpeg and
   Microsoft public APIs.

Accepted design summary:

- a shared `D3D11DeviceAccess` object retains the selected device/immediate
  context and supplies the recursive lock used by FFmpeg, interop, and
  rendering;
- an opaque core hardware-device token lets the D3D11VA backend supply a
  referenced FFmpeg device context without exposing FFmpeg or Windows types in
  core installed headers;
- the backend-specific retained frame view carries the decoder
  `ID3D11Texture2D`, array slice, source-device identity, and source
  `HardwareFrame`;
- because decoder texture arrays cannot be shader-resource views, the initial
  zero-CPU-copy path consumes the decoder slice through the D3D11 Video
  Processor and returns a same-device shader-readable intermediate to the
  renderer;
- hardware-decode fallback and renderer software-map fallback are separate
  explicit policies; foreign devices are rejected before context access and
  device removal requires application-led device recreation;
- WARP covers deterministic API/lifetime/error contracts while opt-in
  real-GPU tests cover native decode and zero-CPU-copy rendering.

Next implementation slice:

1. [x] Add the common Windows D3D11 device-access target and shared recursive
   context guard.
2. [x] Add the opaque supplied-hardware-device token and private FFmpeg bridge
   in core.
3. [x] Prove device identity, COM lifetime, locking, and install/export
   behavior with deterministic tests before opening the native decoder.

Completed Windows D3D11 device-access checkpoint:

- `QtAV::PlatformWindows` verifies that a selected context is the chosen
  device's immediate context, retains both COM interfaces, and exports
  `D3D11DeviceAccess` plus a move-only recursive `D3D11ContextGuard`;
- the D3D11 renderer accepts shared device access while preserving its
  borrowed device/context convenience constructor, and it holds the common
  guard for immediate-context rendering calls;
- deterministic WARP tests cover null, foreign, and deferred-context
  rejection, retained COM lifetime, same-thread recursion, cross-thread
  exclusion, guard lifetime, and renderer participation in the shared lock;
- static/shared builds and installed external consumption of
  `QtAV::PlatformWindows` and `QtAV::RenderD3D11` pass.

Completed supplied hardware-device bridge checkpoint:

- public `HardwareDecodeDevice` is a cheap, PIMPL-backed reference-counted
  token that exposes only `HardwareDeviceType` and an opaque native identity;
- `HardwareDecodeConfig` optionally carries that token while preserving the
  existing FFmpeg-created-device path when no token is supplied;
- an uninstalled core bridge retains and returns referenced FFmpeg
  `AVHWDeviceContext` buffers for in-tree hardware backends without placing
  FFmpeg declarations in installed headers;
- `Player` takes its own device-context reference before decoder open, rejects
  a token/requested-type mismatch through the existing fallback policy, and
  asynchronously reopens loaded media when the supplied token changes;
- deterministic tests cover invalid construction, reference ownership, copy
  identity, independent tokens, player config copying, and type-mismatch
  software fallback plus disabled-fallback failure.

Completed D3D11VA hardware-decode checkpoint:

- `QtAV::HWD3D11VA` is Windows-only, optional under
  `QTAV_HW_D3D11VA=AUTO/ON/OFF`, installable as an exported package target,
  and depends on `QtAV::PlatformWindows` without depending on the renderer;
- `d3d11vaHardwareDecodeConfig()` allocates FFmpeg's D3D11VA device on the
  application-selected retained device and immediate context, installs
  callbacks using the shared recursive context lock, and requests a bounded
  zero-to-64 extra decoder surfaces with a default of four;
- a required-supplied-device flag prevents a failed selected-device setup from
  silently opening a different FFmpeg-created device while preserving the
  explicit software-fallback policy;
- core `NativeHandle` carries the D3D11 decoder texture-array slice without
  exposing D3D11 or FFmpeg types, while the Windows-only `D3D11VAFrame`
  validates and retains NV12/P010 texture, slice, dimensions, and device;
- deterministic tests cover device/context identity, shared-lock exclusion,
  option bounds, native frame validation, invalid slice/format/size/type, and
  retained synthetic texture lifetime;
- the current Windows adapter passes generated H.264 native decode, CPU
  mapping, seek, media replacement, stop, and retained frame access after
  player shutdown.

Next implementation slice:

1. [x] Add decoder-independent `D3D11HardwareFrameInterop` and retained
   `D3D11TextureFrame` interfaces to `QtAV::RenderD3D11`.
2. [x] Add renderer capability reporting plus explicit enabled/disabled
   software-map fallback using mock interop tests.
3. [x] Implement `QtAV::InteropD3D11` with same-device validation and a D3D11
   Video Processor pass into shader-readable SDR BGRA8 or HDR RGB10/FP16
   intermediates.
4. [x] Add WARP contract tests, native zero-CPU-copy H.264 rendering coverage,
   example wiring, and install-consumer validation.

Completed D3D11 renderer interop-contract checkpoint:

- `QtAV::RenderD3D11` exposes decoder-independent
  `D3D11HardwareFrameInterop` and `D3D11TextureFrame` interfaces without
  depending on `QtAV::HWD3D11VA`;
- an interop object identifies the retained `D3D11DeviceAccess` whose shared
  recursive guard protects its immediate/video-context work;
- an imported texture frame reports its dimensions, packed pixel format, DXGI
  format, and color space and keeps its borrowed `ID3D11Texture2D` and
  `ID3D11ShaderResourceView` valid for the texture-frame lifetime;
- deterministic WARP coverage proves capability/source-device reporting,
  import dispatch, shared device-access identity, and COM resource retention
  after the original texture and view references are released.

Completed D3D11 renderer interop-consumption checkpoint:

- `D3D11VideoRenderer` advertises hardware devices only while a compatible
  interop object using the same retained `D3D11DeviceAccess` is installed;
- imported SDR BGRA8/RGBA8, FP16 scRGB, or RGB10/PQ shader-readable texture
  frames feed the final viewport, aspect-ratio, rotation, and color pass
  without CPU mapping;
- software mapping is disabled by default and can be explicitly enabled
  independently of decoder fallback; successful use emits an observable
  detail event, while disabled or failed mapping makes rendering fail;
- mock WARP tests cover direct import with zero map calls, capability changes,
  enabled/disabled mapping fallback, mapped pixel output, and mapping failure.

Completed D3D11 Video Processor interop checkpoint:

- `QtAV::InteropD3D11` validates D3D11VA NV12/P010 texture-array slices,
  exact source/target device identity, device health, format support, and
  dimensions before entering the shared recursive context guard;
- `D3D11FrameInterop` caches the Video Processor enumerator/processor and
  reuses a bounded pool of up to three SDR BGRA8, FP16 scRGB, or RGB10/PQ
  output textures plus shader-resource views without CPU mapping or a
  cross-device copy; overlapping retained imports remain independent and may
  use a transient output when every pool entry is retained;
- the renderer passes structured range/matrix/transfer/chroma metadata through
  a backward-compatible color-aware interop overload; Direct3D 11.1 color
  spaces preserve PQ/BT.2020 as RGB10/PQ (or FP16 scRGB) and HLG/BT.2020 as
  FP16 scRGB (or RGB10/PQ), with legacy SDR BT.601/709 fallback;
- WARP covers texture-array/slice extraction, retained lifetime, sequential
  output reuse, independent overlapping outputs, recursive locking, and safe
  Video Processor unavailability while mock WARP tests cover renderer
  consumption and error contracts;
- the current hardware adapter renders generated H.264/NV12 and PQ/BT.2020
  HEVC Main10/P010 D3D11VA frames with zero map calls, verified red/blue pixel
  readback, and FP16 scRGB values above `1.0`; the H.264 path also covers
  pause/resume, seek, media replacement, explicit stop, surface recreation,
  and retained source/import lifetime after `Player` shutdown;
- the console example wires D3D11VA, `QtAV::InteropD3D11`, and offscreen D3D11
  rendering; the strict H.264/AAC test passes through an active WASAPI render
  endpoint and still makes unavailable endpoint coverage an explicit skip;
  the installed CMake package exports `QtAV::InteropD3D11`.

Following platform slice:

1. [ ] Resolve the Android example playback-stutter regression described in
   `Next task`.

Platform implementation order after the contracts are stable:

1. Windows reference path.
2. Android production path with connected-device validation.
3. Android playback performance/regression work.
4. OHOS production path with connected-device validation when resumed.

## Milestone 3 — Portable reference backends

- [x] CPU video-frame conversion using libswscale.
- [x] CPU renderer or image-buffer target for deterministic tests.
- [x] Audio conversion/resampling using libswresample.
- [x] Null/mock audio sink with deterministic latency.
- [x] Optional file/PCM diagnostic sink.
- [x] Pixel-format and channel-layout conversion tests.
- [x] A/V clock tests with simulated device latency.

Acceptance:

- renderer and audio contracts are proven without a platform SDK;
- deterministic golden tests cover format conversion and timing.

Status: complete and verified.

## Milestone 4 — archived Apple production path

Archived and no longer maintained. Its former implementation, tests,
acceptance record, and integration notes live under
[`../archived_apple/`](../archived_apple/) and are outside the active target
matrix.

## Milestone 5 — Windows production path

### D3D11

- [x] `qtav_render_d3d11`.
- [x] `qtav_output_d3d11` high-level composition output for ordinary
  application-owned native surfaces.
- [x] Borrowed `ID3D11Device`, context, and render target.
- [x] Software-frame texture upload.
- [x] Resize, viewport, aspect ratio, rotation, and redraw.
- [x] Windows Advanced Color SDR, FP16 scRGB, and RGB10/PQ output.
- [x] Per-frame display/HDR-state switching, SDR reference white, PQ/HLG,
  primaries conversion, and display-aware tone mapping.
- [x] Library-owned D3D11 device, composition swap chain, render target,
  redraw-coalescing render thread, `renderVideo()`, `Present()`, resize,
  D3D11VA/Video Processor setup, per-frame composition-monitor/Advanced Color
  tracking, FP16 scRGB HDR presentation, and teardown, retaining the
  borrowed-target renderer as the advanced external-context path.

### Audio and hardware decode

- [x] `qtav_audio_wasapi`.
- [x] Shared-mode PCM negotiation and audio clock.
- [x] Complete the D3D11VA device/frame/interop design checkpoint.
- [x] `qtav_hw_d3d11va`.
- [x] `qtav_interop_d3d11` for zero-copy decoder textures.

Acceptance:

- [x] Windows native example plays A/V without Qt. Generated H.264/AAC proves
  D3D11VA decode, zero-copy rendering, audio decode, and audible WASAPI output
  together.
- [x] Software and D3D11 hardware decode both work.
- [x] Device-loss and surface-recreation paths are tested.
- [x] A composition-output integration test covers WARP creation, native
  swap-chain binding/unbinding, player attach/detach, hardware-config restore,
  decoded-frame presentation, redraw, resize, statistics, and teardown.
- [x] The high-level composition path resolves its explicit current monitor,
  configures FP16 scRGB through `SetColorSpace1`, exposes active HDR/display
  luminance diagnostics, falls back to SDR under `PreferHdr`, and rejects
  inactive HDR under `RequireHdr`.
- [x] Active-HDR native display validation with the Windows HDR setting
  enabled, including HDR numeric readback and HDR-disabled native
  swap-chain/display-switch tests.
- [x] No Windows type leaks into core public headers.

Status: complete; Milestone 6 is also complete. The Android playback-stutter
regression is the active next task, and Milestone 7 is deferred.

## Milestone 6 — Android production path

### Toolchain and application shell

- [x] Reproducible Android NDK build for QtAVCore and the required
  FFmpeg 8+ libraries, initially targeting arm64.
- [x] Minimal APK/native application shell that owns activity, lifecycle,
  permissions, and current rendering surfaces without adding Android types to
  core. NativeActivity creation/destruction, packaged media, active-window
  retain/release, background/foreground, and replacement-surface scenarios
  are proven.
- [x] Connected-device deployment, logging, generated-media playback, and
  automated result collection.

### Rendering

- [x] Shared Vulkan renderer engine from the mobile design checkpoint.
- [x] Android Vulkan surface/swapchain adapter using application-owned native
  resources.
- [x] Native Vulkan HDR target contract and Android HDR swapchain selection,
  including HDR10/PQ, HDR10/HLG, extended-linear output, static metadata,
  deterministic 10-bit goldens, and connected-device lifecycle validation.
- [x] Shared OpenGL ES 3.x renderer plus Android EGL/window adapter as the
  required Vulkan fallback, without an SDL3 dependency.
- [x] Native OpenGL ES HDR target contract and Android exact RGB10_A2
  BT.2020/PQ-or-HLG EGL selection, with explicit RGBA8/sRGB tone-mapping
  fallback, numeric output checks, and connected-device compositor HDR-layer
  validation.
- [x] Application/platform renderer selector implementing the accepted
  startup, recovery, fatal-error, one-way fallback, and no-renderer behavior.
- [x] Software YUV/NV12/P010/RGB mapping through the shared FFmpeg/libplacebo
  bridge, libplacebo-generated color/render shaders, viewport, aspect ratio,
  rotation, resize, redraw, and surface recreation.
- [x] Replace the Vulkan and OpenGL ES handwritten color pipelines with
  libplacebo backends for color conversion, Dolby Vision RPU reshaping, tone
  and gamut mapping, scaling, and SDR/HDR output encoding.

### Audio and hardware decode

- [x] `qtav_audio_aaudio`, with OpenSL fallback only if the selected minimum
  Android API or real-device coverage requires it.
- [x] Device format negotiation, bounded callback-safe buffering, device clock,
  latency, pause, flush, drain, route change, and disconnect handling.
- [x] `qtav_hw_mediacodec` with explicit wrapper-decoder selection and
  application-supplied surface/device lifetime.
- [x] Direct-surface presentation with explicit present/drop behavior before
  texture interop.
- [x] Android Vulkan interop using an application-owned private,
  GPU-sampled `AImageReader`: supply its `ANativeWindow` to MediaCodec,
  correlate codec and acquired-image timestamps, import the retained
  `AHardwareBuffer` through
  `VK_ANDROID_external_memory_android_hardware_buffer`, apply
  YCbCr/external-format capability checks, bridge acquire/release fences, and
  return the release fence through asynchronous `AImage` deletion without
  `AHardwareBuffer_lock*()` or a staging upload.
- [x] Android MediaCodec OpenGL ES interop using a private GPU-sampled
  `AImageReader`, retained `AHardwareBuffer`/EGLImage imports, explicit
  timestamp/generation and fence lifetime, and crop-aware raw Y/Cb/Cr
  normalization before libplacebo. The normalization shader performs no color
  conversion, reshape, tone/gamut mapping, or output encoding; the path never
  maps or re-uploads decoded pixels.
- [x] On Vulkan-to-OpenGL ES renderer fallback, attempt compatible GLES native
  interop for subsequent frames; otherwise follow an explicit direct-surface,
  software-decode, or no-video policy without implicit hardware-frame mapping.
- [x] Software fallback independent of renderer mapping/interop fallback.

Acceptance:

- a cross-build installs and runs on at least one connected arm64
  Android device;
- software decode prefers Vulkan, falls back to OpenGL ES for unavailable or
  fatally failed Vulkan, and AAudio produces synchronized audible output;
- connected-device or deterministic adapter tests cover initial fallback,
  recoverable Vulkan recreation without an API switch, fatal one-way fallback
  without media reopen, and the both-renderers-unavailable error path;
- MediaCodec H.264 and HEVC paths cover pause/resume, seek, media replacement,
  stop, background/foreground transition, surface recreation, and shutdown;
- direct-surface hardware output is verified before any texture-interoperable
  path is described as complete;
- on capable devices, H.264 and HEVC native frames render through both Vulkan
  and OpenGL ES with zero CPU map/transfer/upload calls, correct fence ordering,
  bounded retained buffers, color/format validation, and lifecycle coverage;
- pixel-validation tests may read back the final render target, but decoded
  source map, transfer, staging-copy, and re-upload counters remain zero;
- an unsupported Vulkan or OpenGL ES import capability is reported explicitly
  as unavailable or skipped, not counted as zero-CPU-copy success;
- Android SDK types remain outside core public headers.

Status: complete. Resolve the active Android playback-stutter task first;
Milestone 7 remains deferred until explicitly resumed.

## Deferred milestone 7 — OHOS production path

This milestone remains in the supported roadmap, but it is not the active next
task. Do not begin its target-clarification or implementation work until the
Android playback regression is resolved and the user explicitly resumes OHOS.

Target clarification gate:

- [ ] Record whether the initial target is a HarmonyOS NEXT commercial device
  application, a specific OpenHarmony distribution/device, or both; record the
  SDK/API version, signing requirements, available system capabilities, and
  connected-device workflow before fixing backend availability rules.

### Toolchain and application shell

- [~] A reproducible OHOS arm64/API 23 FFmpeg 8+ dependency cross-build is
  implemented and locally verified under `../ffmpeg/`; the QtAVCore target
  build, CI execution on an SDK-equipped runner, and device validation remain
  pending.
- [~] Add the `modern/platform/ohos/` root; small shared helpers are still
  pending, and media, graphics, and audio implementations remain in their
  responsibility-specific backend targets.
- [ ] Minimal HAP/native application shell using ArkUI/XComponent only at the
  integration boundary.
- [ ] Connected-device deployment, signing, logging, generated-media playback,
  and automated result collection through thin OHOS-specific adapters to the
  shared mobile test scenarios.

### Vulkan and OpenGL ES rendering

- [ ] Reuse the Android-proven platform-neutral Vulkan renderer engine,
  shaders, color conversion, geometry, synchronization rules, golden vectors,
  and renderer contract tests.
- [ ] Add the OHOS `OHNativeWindow`/XComponent Vulkan surface and swapchain
  adapter as a separate target or platform helper.
- [ ] Reuse OpenGL ES renderer internals where compatible, with a separate OHOS
  EGL/window adapter and explicit capability checks.
- [ ] Reuse the Android-proven renderer selector and one-way Vulkan-to-OpenGL
  ES policy while keeping OHOS window, EGL, and error classification in its
  own adapter.
- [ ] Validate software YUV/NV12/P010/RGB upload, viewport, aspect ratio,
  rotation, resize, redraw, surface loss/recreation, SDR, and supported HDR
  output behavior on a real device.

### Audio and hardware decode

- [ ] Add an OHOS OHAudio sink target with negotiated PCM, bounded callback-safe
  buffering, device clock/latency, pause, flush, drain, route change, and
  disconnect behavior.
- [ ] Add an OHOS OHCodec hardware-decode target using FFmpeg 8
  `AV_HWDEVICE_TYPE_OHCODEC` and explicit H.264/HEVC wrapper-decoder selection.
- [ ] Reuse the shared surface-backed presentation contract for playback-clock
  scheduling, present/drop, outstanding-buffer bounds, flush, stop, and surface
  recreation.
- [ ] Implement direct `OHNativeWindow` presentation first.
- [ ] Add the confirmed OHOS OpenGL ES path: supply the `OHNativeWindow`
  produced by `OH_NativeImage` to OHCodec surface output, update the surface
  image, and sample its bound `GL_TEXTURE_EXTERNAL_OES` texture while
  enforcing the selected SDK's token, generation, and image-lifetime rules.
- [ ] Before claiming OHOS Vulkan zero-CPU-copy interop, add a backend or
  narrowly scoped FFmpeg bridge that exposes and retains the decoded
  `OH_AVBuffer`/`OH_NativeBuffer` through GPU completion. The current FFmpeg 8
  OHCodec buffer branch calls `OH_AVBuffer_GetAddr()` and `av_image_copy2()`
  and is therefore disallowed; its surface branch exposes only present/drop
  tokens, not a Vulkan-importable native buffer.
- [ ] After that bridge exists, add the OHOS Vulkan adapter using the target
  SDK's native-buffer/external-memory path and release the codec output only
  after GPU completion. Record exact format, lifetime, protected-content, and
  fence capabilities on the target device; keep the feature unavailable when
  any required capability is absent.
- [ ] On Vulkan-to-OpenGL ES renderer fallback, attempt compatible GLES native
  interop for subsequent frames; otherwise follow an explicit direct-surface,
  software-decode, or no-video policy without implicit hardware-frame mapping.
- [ ] Keep software decode fallback independent of Vulkan/OpenGL ES interop
  fallback.

Acceptance:

- a cross-build installs and runs on the recorded connected arm64 OHOS
  target;
- software decode prefers Vulkan, falls back to OpenGL ES for unavailable or
  fatally failed Vulkan, and OHAudio produces synchronized audible output;
- connected-device or deterministic adapter tests cover initial fallback,
  recoverable Vulkan recreation without an API switch, fatal one-way fallback
  without media reopen, and the both-renderers-unavailable error path;
- OHCodec H.264 and HEVC paths cover pause/resume, seek, media replacement,
  stop, background/foreground transition, surface recreation, and shutdown;
- on a capable target, H.264 and HEVC native frames render through both Vulkan
  and OpenGL ES with zero CPU map/transfer/upload calls, correct fence ordering,
  bounded retained buffers, color/format validation, and lifecycle coverage;
- the OHOS Vulkan result specifically proves that the active decode path did
  not call `OH_AVBuffer_GetAddr()` or `av_image_copy2()`; final-render-target
  readback for pixel validation is permitted;
- unsupported native-buffer import capability is reported explicitly as
  unavailable or skipped, not counted as zero-CPU-copy success;
- Android/OHOS share renderer engines and deterministic tests without sharing
  platform SDK types or incorrectly treating their native lifecycles as ABI
  compatible;
- OHOS SDK types remain outside core public headers.

## Milestone 9 — Playback feature parity

- [ ] Active audio/video track switching after load.
- [ ] Subtitle packet decode.
- [ ] Plain-text subtitle callback.
- [ ] libass renderer as an optional module.
- [ ] External audio and subtitle sources.
- [ ] Packet buffering policy and buffering status.
- [ ] Low-latency/live-stream drop policy.
- [ ] Reconnect and recoverable network errors.
- [ ] Frame stepping and accurate seek.
- [ ] Audio time-stretch for playback rate without pitch change.
- [ ] Filter/plugin contracts.

## Milestone 10 — Dolby and HDR scope

- [x] Generic FFmpeg AC-3 software decode.
- [x] Generic FFmpeg E-AC-3 software decode.
- [x] Generic FFmpeg TrueHD software decode.
- [ ] Multichannel PCM device-output validation.
- [ ] IEC 61937 compressed passthrough contract.
- [ ] Windows HDMI/WASAPI passthrough.
- [ ] Atmos object-metadata preservation/rendering feasibility.
- [x] HDR10 metadata propagation.
- [x] Dolby Vision Profile 5 application-rendered playback: FFmpeg parses RPU
  metadata, the Vulkan and OpenGL ES backends pass raw base-layer components
  to libplacebo for reshaping, and libplacebo performs target-aware SDR tone
  mapping or native HDR output. Connected-device validation covers both GPU
  backends without decoded-source CPU copies. Enhancement-layer residual
  reconstruction and product certification remain out of scope.
- [ ] Licensing and certification review before product claims.

Codec decoding must never be described as Dolby certification or Atmos
rendering.

## Release and plugin strategy

- [ ] Keep all modules in this repository while interfaces evolve.
- [ ] Version the core C++ API and CMake package.
- [ ] Add continuous integration for supported host platforms.
- [ ] Define a C ABI only when runtime-loaded plugins become necessary.
- [ ] Split a backend into another repository only when it has an independent
  license, team, release cycle, or closed-source delivery requirement.

## Deferred final task — guarded Android HDR external-OES fallback

Do not begin this task until every preceding milestone, acceptance item, and
release/plugin-strategy item in this plan is complete.

- [ ] Evaluate an experimental
  `MediaCodecOpenGLInteropConfig::hdrExternalOesSamplingEnabled = true` path
  for Android P010/HDR `SurfaceTexture`/`GL_TEXTURE_EXTERNAL_OES` input.
- [ ] Treat the flag as a guarded trial only when the application has selected
  that policy. If the first HDR frame produces an explicit capability,
  import, sampling, or presentation failure, disable HDR external-OES for the
  current device/codec/session and perform exactly one one-way pipeline
  fallback.
- [ ] Reconfigure MediaCodec at the current playback position to direct-Surface
  presentation so Android composition preserves HDR. Do not merely set the
  flag back to `false`, retry the retired SurfaceTexture generation, map the
  failed hardware frame, or enter a rebuild loop.
- [ ] Preserve pause/play intent, seek position, audio synchronization, HDR
  metadata, and surface lifecycle across the fallback, and report the selected
  fallback explicitly in player status and diagnostics.
- [ ] Keep explicit runtime failure detection separate from color validation:
  successful external-OES presentation does not prove correct P010 precision,
  range, BT.2020 primaries, PQ/HLG transfer, luminance, or absence of duplicate
  conversion. Enable a persistent device/codec allowlist only after independent
  color golden and Android compositor dataspace/HDR-metadata validation.
- [ ] Add deterministic policy tests plus connected-device coverage for trial
  success, each explicit failure class, single fallback, stale-generation
  rejection, seek, background/foreground recreation, and clean shutdown.
