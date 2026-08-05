# QtAVCore implementation plan

Last updated: 2026-08-05

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
cross-compilation host for Android/OHOS; 64-bit Windows is also a supported
OHOS cross-compilation host through the DevEco native SDK.

OHOS is now the active local development target. AD-007 is closed after the
current vendor-neutral D3D11VA/libplacebo policy was accepted on NVIDIA, Intel,
and AMD platforms.

The user's separate AMD integrated-GPU 4K frame-loss report is objectively
located, corrected, and fully verified by reason-aware render retry plus
proactive bounded D3D11 context handoff, without changing AD-007's
imported-frame policy. A controlled cold rerun restored both supplied files to
source cadence with zero terminal context drops; the intervening stressed
draw-throughput shortfall was a transient prolonged-build/UI-capture load
state rather than a retained regression. The same commit and objective
cadence/lifecycle matrix now also pass on NVIDIA. The separate Intel 4K
post-seek performance investigation remains incomplete and has been transferred
to another Intel Windows machine where Codex can run with administrator rights
for WPR/GPUView capture and an evidence-backed fix. By explicit user priority,
that external Windows track no longer blocks local OHOS work; its fix and full
cross-vendor validation are still required before the Intel/Windows matrix can
close. Local development begins with the OHOS target-clarification/toolchain
gate and the portable render-result semantics required by its Vulkan/OpenGL
adapters, without copying the Windows-only D3D11 reservation mechanism.

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
  raw `qtav_interop_d3d11` texture/slice path are complete, including
  same-device validation, shader-readable NV12/P010 decoder surfaces, native
  H.264/NV12 and PQ/BT.2020 HEVC Main10/P010 zero-CPU-map rendering through
  libplacebo's D3D11 backend to SDR BGRA8, HDR RGB10, or FP16 scRGB targets,
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
  D3D11 interop now retains only the decoder texture and exact array slice;
  it performs no Video Processor conversion, output-texture pooling, context
  submission, or wait before libplacebo samples the raw NV12/P010 planes;
  Player render attempts now use atomically published immutable frame/binding
  snapshots and reason-aware results. Before the high-level D3D11 output makes
  its first non-blocking context attempt, it creates a render-thread
  reservation; contention uses a bounded handoff before timer backoff. The
  output keeps one latest retryable frame and preserves the imminent queued
  presentation frame under
  pressure and retains imported hardware frames plus borrowed targets through
  a bounded GPU-completion queue; the immediate context enables native
  multithread protection before decoder/render sharing, imported frames retain
  libplacebo's fast parameters, and successful per-frame submissions remain
  asynchronous without a Dolby decoder-surface copy;
  `QtAV::OutputD3D11` now owns the Windows device, composition swap chain,
  render target, display/HDR tracking, render scheduling thread,
  D3D11VA/interop wiring, `renderVideoDetailed()`, `Present()`, resize, and
  teardown;
  its default FP16 scRGB path resolves the hosting window's monitor without
  relying on unsupported composition-swap-chain `GetContainingOutput()`,
  tracks Windows HDR/SDR-white/luminance changes per frame, and exposes
  prefer-HDR, require-HDR, and SDR-only policies. Opaque video hosts can select
  RGB10/PQ explicitly; the WinUI 3 sample does so, supplies its HWND, binds its
  `SwapChainPanel`, attaches the player, and forwards size changes. The output
  caps frame latency at one, uses
  non-blocking `Present()` with bounded waitable-object backpressure on its
  private render thread, and exposes render/present, retry/handoff, terminal
  drop, and per-stage timing statistics;
  its progress slider observes already-handled thumb pointer events and
  commits only one seek when a drag ends instead of issuing intermediate seeks;
  Milestones 5 and 6 are complete; the OHOS production path is now the active
  local development track, and the Android example playback-stutter regression
  recorded below is fixed and connected-device verified; the shared Android/OHOS
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
- Windows now follows the same color-authority boundary through libplacebo's
  D3D11 backend. FFmpeg-parsed Dolby Vision metadata, Dolby Vision reshaping,
  transfer handling, gamut conversion, HDR tone mapping, and target encoding
  are owned by libplacebo; QtAVCore has no alternate native Windows shader for
  those operations. Windows exposes only the QtAVCore D3D11 renderer and does
  not build its OpenGL or Vulkan renderer targets;
- QtAVCore now requires FFmpeg 8.0 or newer (libavcodec major 62+); compatibility
  branches for FFmpeg 5–7 are intentionally out of scope;
- `../ffmpeg/` now provides a pinned vcpkg dependency-build subproject for
  Android arm64/API 28 cross-builds on macOS, OHOS arm64/API 23 cross-builds
  on macOS or Windows, and native Windows x64/Visual Studio builds. Its FFmpeg
  8.1.2 policy enables OHCodec on OHOS plus OpenSSL,
  libsmb2, Vulkan, libass, libplacebo with OpenGL/OpenGL ES and built-in Dolby
  Vision reshaping, dav1d and native VVC decode while avoiding the unrelated
  desktop dependencies pulled by `ffmpeg[all]`; the
  Android, OHOS, and Windows dependency packages have been built, verified,
  and uploaded by self-hosted CI; the Windows validation uses Visual Studio
  18's clang-cl 22.1.3 with lld-link and preserves FFmpeg LTO; the Windows
  entry point was also rerun locally from a clean target install on 2026-08-03,
  completing all 25 packages and the installed-package verifier;
- the root `README.md` and `AGENTS.md` now record the modern entry point,
  FFmpeg 8 minimum, and local-first dependency resolution followed by a newest
  successful `main` artifact fallback, then local compilation if both fail;
  changes under `ffmpeg/**` always require the affected local native build for
  Android, OHOS, or Windows validation; legacy build guidance remains
  unchanged;
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
- `modern/backends/audio/ohaudio/include/qtav/ohaudio_audio_sink.h`
- `modern/backends/render/d3d11/include/qtav/d3d11_video_renderer.h`
- `modern/backends/output/d3d11/include/qtav/d3d11_video_output.h`
- `modern/backends/render/vulkan/include/qtav/vulkan_video_renderer.h`
- `modern/backends/render/vulkan/android/include/qtav/android_vulkan_video_renderer.h`
- `modern/backends/render/vulkan/ohos/include/qtav/ohos_vulkan_video_renderer.h`
- `modern/backends/render/opengl/include/qtav/opengl_video_renderer.h`
- `modern/backends/render/opengl/android/include/qtav/android_opengl_video_renderer.h`
- `modern/backends/render/opengl/ohos/include/qtav/ohos_opengl_video_renderer.h`
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
- `modern/backends/audio/ohaudio/src/ohaudio_audio_sink.cpp`
- `modern/backends/render/d3d11/src/d3d11_video_renderer.cpp`
- `modern/backends/render/vulkan/src/vulkan_video_renderer.cpp`
- `modern/backends/render/libplacebo/src/libplacebo_ffmpeg_bridge.c`
- `modern/backends/render/vulkan/android/src/android_vulkan_video_renderer.cpp`
- `modern/backends/render/vulkan/ohos/src/ohos_vulkan_video_renderer.cpp`
- `modern/backends/render/opengl/src/opengl_video_renderer.cpp`
- `modern/backends/render/opengl/android/src/android_opengl_video_renderer.cpp`
- `modern/backends/render/opengl/ohos/src/ohos_opengl_video_renderer.cpp`
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
- before the libplacebo D3D11 migration, the static and shared Windows builds
  passed 34/34 CTest tests, including the high-level D3D11 composition-output
  lifecycle test, Advanced Color test, WASAPI device test, and strict native
  H.264/AAC playback. After the migration, both static and shared Release
  builds pass all 13 tests registered without a host FFmpeg media-generator
  executable; the existing RGB/YUV420P/NV12 media fixtures, Advanced Color,
  installed-package consumers, and bounded Profile 5 URL test also pass;
- a fresh Windows shared Release configure with the repository
  `x64-windows-static-md` prefix now bootstraps vcpkg's host `pkgconf` before
  constructing the FFmpeg imported targets, so the first configure preserves
  the static `swresample`, `libsmb2`, and remaining transitive link closure.
  The full build and all 36 host-FFmpeg-enabled tests pass. The native
  HEVC Main10/P010 zero-copy test also covers aligned D3D11VA decoder textures:
  libplacebo receives the real allocation/plane dimensions and the decoded
  160x90 image as the visible crop, avoiding sampling the aligned padding;
- the WinUI 3 Release sample rebuilds from that fresh shared tree with zero
  MSBuild warnings or errors. A local generated H.264 red/blue clip reached
  end of playback with the complete visible image and the process then exited
  cleanly;
- Windows FP16 output now converts libplacebo's 203-nit normalized linear
  convention to Windows' absolute scRGB convention (`1.0 == 80 nits`) after
  color management. The synthetic 1000-nit PQ readback lands at 12.48 scRGB,
  but this mathematical check did not establish subjective parity with a
  native-PQ reference player. The WinUI 3 sample therefore now selects an
  opaque RGB10/PQ swap chain while QtAVCore retains scRGB as its default;
- D3D11 libplacebo submissions now retain decoder slices, texture wrappers,
  and borrowed targets through a bounded GPU-completion queue, with an
  explicit drain before swap-chain resize. On the exercised Intel adapter,
  Dolby Vision NV12/P010 frames are copied GPU-to-GPU from the selected decoder
  array slice into a pooled single-slice shader-resource texture. A subsequent
  manual run disproved the assumption that this copy alone fixed the crash:
  at 19:39:57 the process again failed in `igd10um64xe.dll` at offset
  `0x5e56b`. A subsequent `legend.mkv` run at 20:18:59 reproduced that exact
  driver module and offset through ordinary HDR10 direct import, proving the
  hazard is not Dolby-Vision-only. The same complete workaround was then
  exercised on an AMD Radeon 880M: both representative files and a generated
  H.264/NV12 clip crashed in `amdxx64.dll` without it, while six alternating
  cold starts, sustained 120-second and 90-second runs, and 20 combined seeks
  passed with no software decode or decoded-source CPU map. The user then
  reproduced the crash on the prior asynchronous path on an NVIDIA adapter.
  The then-current workaround was therefore made vendor-neutral: every
  successfully imported D3D11VA frame used libplacebo's fast parameters and
  `pl_gpu_finish()` before copied or direct decoder resources could be
  recycled. Software frames retained the default parameters and did not take
  the per-frame wait. Under that corrected Intel validation policy,
  `legend.mkv`
  reached 01:12 with 25 fps scheduled and mostly 23.3-24.4 fps rendered;
  `wednesday.mp4` reached 02:05 with 24 fps scheduled, mostly 21-23.5 fps
  rendered, and a scene-dependent low near 16 fps before recovery. Both used
  active RGB10/PQ output and were closed normally. No new QtAV application
  error was recorded after 20:20. These local observations pass the original
  six-second failure point. The user subsequently confirmed that brightness
  matches MPC-BE on the same display and accepted the Intel fix for closure;
- the vendor-neutral imported-frame AD-007 build completed with Visual Studio
  2026 and all 36 registered CTest tests passed. Fresh 15-second debugger
  observations on the AMD host kept both `legend.mkv` and `wednesday.mp4` on
  D3D11VA and completed without a crash; the Dolby Vision run reported active
  decoder-surface copies. On the later NVIDIA host, the same build failed in
  `nvwgf2umx.dll+0x59f589` for H.264/NV12, HDR10/P010, and Dolby Vision. The
  shared access now enables native D3D11 immediate-context multithread
  protection before decoder/render sharing. The RTX 3050 then passed
  sustained playback, four seeks, two media replacements, and
  close-while-playing with D3D11VA and no decoded-source CPU transfer.
  A further NVIDIA control passed with fast parameters, the Dolby copy, and
  per-import finish all disabled. The current vendor-neutral policy therefore
  keeps native protection, the recursive guard, completion-query retention,
  lifecycle drains, and imported-frame fast parameters while removing the
  successful per-import `pl_gpu_finish()` and Dolby decoder-surface copy.
  The updated Windows shared/static Release suites both pass 36/36. The user
  subsequently confirmed that this exact current policy is usable on Intel and
  AMD platforms, closing AD-007 across all three vendors. Those final two
  confirmations did not include exact adapter/driver and objective cadence
  data, so they are not performance baselines. A subjective report of 4K
  dropped frames on an AMD integrated GPU is tracked separately in `Next
  task`; that AMD repair and the NVIDIA same-commit performance confirmation
  are now complete, with Intel remaining;
- `legend.mkv` confirms that the brightness report is not Dolby-Vision-only:
  its decoded metadata is BT.2020/PQ and the current sample reports active
  RGB10/PQ output with 240-nit system SDR white and a 1405-nit display peak.
  Those facts verify the selected transport rather than perceived brightness;
  the user's side-by-side comparison with MPC-BE confirmed matching output;
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
- the earlier unretained-submit implementation completed 12 alternating
  forward/backward seeks on the same 3840x2160 Main10 HDR URL without
  audio/video freeze and showed no per-seek linear memory growth. The current
  renderer replaces that unsafe lifetime assumption with the bounded
  completion policy above; seek regression coverage remains in the Windows
  native tests;
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
  rendering through libplacebo, native H.264/NV12 and HEVC Main10/P010 raw
  D3D11VA plane presentation with verified pixels and no CPU mapping, plus pause/resume,
  seek, media replacement, stop, surface recreation, and retained-frame use
  after player shutdown;
- Windows Advanced Color coverage now includes libplacebo-governed PQ/HLG,
  BT.2020 conversion, SDR tone mapping, FP16 scRGB and RGB10/PQ semantic
  readback, native HWND and composition flip-model swap chains,
  `IDXGIOutput6`, `SetColorSpace1`, composition-monitor lookup, SDR-white
  lookup, and same-adapter display switching while Windows HDR is disabled
  and enabled. Active-HDR validation on a
  PHL 27B1U7903 reported a 10-bit G2084/P2020 output, system-derived 240-nit
  SDR white, 1405.11-nit peak luminance, and 1000-nit PQ output above scRGB
  `1.0`;
- the supplied 8.6-GB Dolby Vision Profile 5 URL is covered by a bounded
  network integration run: 49 HEVC Main10/P010 D3D11VA frames rendered through
  libplacebo, all 49 carrying FFmpeg-parsed Dolby Vision metadata, with zero
  decoded-source CPU mapping;
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
- [x] Render scheduling, reason-aware `renderVideoDetailed()`, and the
  pull-style compatibility `renderVideo()` wrapper.
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
Android regression baseline. The unfinished Intel Windows check is transferred
to a separate administrator-capable machine; on this machine the portable
Android/OHOS render-result follow-up and Milestone 7 OHOS work are now active:

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

There are now two explicit work tracks. The Intel Windows track remains open
and is transferred to another Intel machine for administrator-only GPU tracing,
root-cause confirmation, repair, and cross-vendor regression. The active local
track is OHOS: its Milestone 7 target/toolchain gate, portable Android/OHOS
render-result contract, HAP shell, Vulkan/OpenGL ES software-rendering fallback,
and native OHAudio output are complete, so the next slice starts OHCodec
hardware decode with direct `OHNativeWindow` presentation first.

Active local OHOS execution order:

1. [x] Complete the Milestone 7 target-clarification gate: record HarmonyOS
   NEXT versus OpenHarmony target, SDK/API version, signing, device, and
   connected deployment workflow.
2. [x] Audit and validate the existing uncommitted OHOS/FFmpeg toolchain work
   without discarding it, using the repository arm64/OHOS dependency contract.
3. [x] Evaluate and implement portable detailed render-attempt semantics for
   Android and OHOS Vulkan/OpenGL before an OHOS adapter depends on the result.
   Keep the common Player immutable snapshot, sequence, and presentation-
   generation contract, but do not port D3D11 context reservation or the
   Windows latest-frame mailbox. Distinguish presented, deferred until redraw,
   retry after backoff, stale/discarded, surface lost, and fatal outcomes.
4. [x] Add the minimal ArkUI/XComponent HAP shell and the first OHOS native
   window adapter, beginning with software-frame Vulkan presentation through
   the shared renderer engine and the result contract above.
5. [x] Add the OHOS EGL/OpenGL ES window adapter, then connect the shared
   mobile selector so Vulkan-unavailable and fatal-failure sessions have the
   accepted one-way OpenGL ES fallback without reopening media.
6. [x] Add the OHOS OHAudio sink with negotiated PCM, bounded callback-safe
   buffering, playback clock/latency, lifecycle control, and device recovery.

The HAP/XComponent Vulkan slice completed on 2026-08-05. It adds the exported
`QtAV::RenderVulkanOHOS` target, retains the active `OHNativeWindow`, owns the
OHOS Vulkan surface/swapchain synchronization, and delegates color and geometry
work to `QtAV::RenderVulkan`. A minimal ArkUI/N-API shell, signed-project
staging script, and HDC result collector build generated MPEG-4 media through
the repository arm64/API 23 FFmpeg package. On the connected Mate 60 Pro, a
fresh build installed and ran as `com.qtav.feasibility`, selected Maleoon 910
and a 1260x2375 SDR swapchain, and software-decoded/presented the required 30
frames twice. Shared and static QtAVCore variants compile the adapter and HAP
native entry, the install exports `QtAV::RenderVulkanOHOS`, and a separate
OHOS CMake consumer links that installed target.

The OHOS EGL/OpenGL ES and selector slice completed on 2026-08-05. It adds the
exported `QtAV::RenderOpenGLOHOS` target, retains the active XComponent
`OHNativeWindow`, owns EGL display/context/surface/swap lifecycle, and verifies
an exact RGBA8/sRGB native-window contract. Required native HDR fails
explicitly until its format/colorspace/compositor gate is validated. The HAP
now forces initial OpenGL ES selection for 20 frames, starts a fresh selector
session on Vulkan, injects a fatal Vulkan result after 12 frames, and presents
30 further frames through one-way OpenGL ES fallback while keeping exactly one
Player media open. The connected Mate 60 Pro produced the exact marker
`QTAV_OHOS_RESULT PASS software selector initialGLES=20 fatalVulkan=12
fatalGLES=30 mediaOpen=1`; both adapters also recreated their surface for new
native-window generations. Shared and static arm64/API 23 builds install and
export the new target, and separate installed-package consumers link it with
`QtAV::RenderMobile` and `QtAV::RenderVulkanOHOS`.

The OHOS OHAudio slice completed on 2026-08-05. It adds the optional exported
`QtAV::AudioOHAudio` target and `OHAudioAudioSink`, which negotiates 48 kHz
mono/stereo Float32 PCM, shares the Android-proven allocation-free SPSC queue,
uses hardware-committed frame timestamps for the playback-master clock, and
reports combined native/backend latency. The real-time write callback performs
only bounded copies, silence fill, and atomic updates; route changes, forced
interruptions, errors, event delivery, and stream reconstruction stay on a
backend worker. The signed `com.qtav.feasibility` HAP now packages a one-second
MPEG-4/AAC clip, pauses/resumes, seeks/flushes, and loops through segment-end
drain while preserving the renderer-selector scenario. The connected Mate 60
Pro produced `audioDecoded=40 audioRendered=191904 audioClock=85
audioLatencyMs=507 audioStarts=4 audioFlushes=3 audioDrains=3
audioRestarts=0` in the final PASS marker. This proves native PCM delivery and
hardware timing; subjective audibility remains a manual listening check, and
no physical route change occurred in that run. Shared arm64/API 23, HAP, and
installed-target builds link `libohaudio`; the next local slice is OHCodec
hardware decode and direct `OHNativeWindow` presentation.

The portable render-attempt slice completed on 2026-08-05. The public
`VideoRenderAttemptResult` now carries `Presented`, `DeferredUntilRedraw`,
`RetryAfterBackoff`, `Discarded`, `SurfaceLost`, and `FatalError`; Player maps
those outcomes to reason-aware `VideoRenderResult` statuses with frame
sequence, presentation generation, retry delay, and detail. Vulkan, OpenGL ES,
their Android surface adapters, and `MobileVideoRendererSelector` implement the
contract directly. Deterministic selector and playback tests cover every
status, stale-generation rejection, same-API recovery, and fatal one-way
fallback. Windows all-backends-disabled CTest passed 12/12, and a clean OHOS
arm64/API 23 shared build resolved the standard repository dependency prefix
and compiled/linked all 63 core, Vulkan, OpenGL ES, mobile-selector, and test
steps. Both Android native adapters passed an NDK r28c/API 28 syntax
cross-check, and the Windows Release `qtav_output_d3d11` target compiled with
the expanded Player status set. No Windows D3D11 reservation or mailbox policy
was copied into the portable layer.

Transferred Intel Windows checklist (not the next local task):

AD-007 is complete: the current protected, asynchronous imported-frame policy
is accepted on NVIDIA, Intel, and AMD. The AMD WinUI 3/D3D11 counted loss is
traced to generic render retry/context-handoff semantics, corrected by
proactive bounded handoff, and fully verified on the Radeon 880M with both
supplied 4K files. The same-commit NVIDIA cadence and lifecycle matrix is also
complete; the remaining Windows task consists only of the transferred Intel
post-seek performance investigation.

Earlier correctness observations used older
workaround combinations and do not satisfy this performance baseline. Do not
treat a vendor-specific performance difference as an AD-007 correctness
regression unless new synchronization or driver-failure evidence appears.

1. [x] Record all final test systems: the completed AMD system plus NVIDIA and
   Intel GPU names, PCI vendor/device/subsystem IDs, driver and Windows
   versions, CPU and system-memory configuration, power mode, display
   resolution and refresh rate, Windows HDR state, output color mode, and
   whether each system is on AC power. Record the tested commit/build
   configuration and the exact files' codec, pixel format, dimensions, frame
   rate, color metadata, duration, and audio layout.
2. [x] Reproduce on the AMD integrated GPU in a Release WinUI build with a
   local 4K file. Capture at least two consecutive settled cadence windows and
   one 60-120 second run, recording source fps, scheduled and rendered fps,
   coalesced redraws, `present-busy`, `render-skipped`, gaps over 80 ms, and
   color/interop/buffer/draw stage maxima. Confirm D3D11VA and zero
   decoded-source CPU map.
3. [~] Run the NVIDIA and Intel confirmations from the same commit and Release
   configuration, using the same source scenes, duration, output resolution,
   color mode, window state, and cadence/stage counters. Match display refresh,
   HDR, and power conditions where practical and record every unavoidable
   difference. Capture at least two settled windows and one 60-120 second run
   on each device; a subjective "looks smooth" result is not a baseline. The
   NVIDIA confirmation is complete. Intel now has a same-commit objective
   checkpoint, but remains open because the `legend.mkv` 22:48 seek scene did
   not consistently retain the settled pre-seek cadence/render-gap baseline.
4. [x] Locate the first AMD stage that misses the frame budget. Separate input,
   decode, audio-clock, and Player scheduling loss from renderer/context loss
   and swap-chain/compositor backpressure. Compare vendors only after
   accounting for source frame rate, display refresh, output mode, and power;
   a visual comparison or raw cross-device fps alone is not a diagnosis.
5. [~] Run the common matrix on the remaining NVIDIA and Intel devices:
   generated 1080p H.264/NV12 control, 4K SDR when available, HDR10/P010
   `legend.mkv`, and Dolby Vision Profile 5 `wednesday.mp4`. Include cold
   start, sustained playback, seek, media replacement, and close while
   playing, while preserving the accepted AD-007 synchronization policy. The
   AMD supplied-file repair and lifecycle validation are complete. NVIDIA is
   complete for the generated H.264 control and both supplied HDR files; no
   separate 4K SDR control was available. Intel passed the H.264 control and
   Dolby Vision workload, but its HDR10 `legend.mkv` seek-scene result remains
   open.
6. [~] Run controlled diagnostic A/Bs only after the failing stage is known.
   Candidate renderer-side isolates are native-size versus reduced-size output,
   HDR RGB10/PQ versus SDR BGRA8, and windowed versus display-sized
   presentation. The Intel `suzume.mkv` RGB10/PQ-versus-SDR-BGRA8 A/B is
   complete: both modes reproduce the same post-seek D3D11 pass-submission
   stalls, so HDR output is not a necessary condition. Record GPU
   decode/3D/copy utilization, clocks, power, and memory pressure alongside
   cadence. Do not disable native D3D11 multithread protection or infer a fix
   from lower image quality.
7. [~] Compare the same scenes with a trusted player on each device under the
   recorded display and power conditions, with interpolation and
   post-processing differences recorded. If QtAVCore is slower, identify the
   responsible backend stage and implement only an evidence-backed optimization
   with regression coverage on AMD, NVIDIA, and Intel.
AMD checkpoint on 2026-08-04, before selecting a fix:

- commit `ffc4139`, shared Release QtAVCore built with the Visual Studio 2026
  ClangCL/lld configuration and the Release WinUI 3 player; Windows 11 Home
  China 25H2 build `26200.8875`; Lenovo 83LR with Ryzen AI 9 H 365, 32 GiB
  LPDDR5X-8000, Radeon 880M
  (`PCI\\VEN_1002&DEV_150E&SUBSYS_380217AA`, driver
  `32.0.22029.9039`), balanced power plan, and AC power online. The player was
  windowed with a 1708x814 composition surface on the primary Dell U2720Q at
  3840x2160/60 Hz. Windows HDR was active and the output reported RGB10/PQ,
  240-nit SDR white, and a 400-nit display peak.
- `wednesday.mp4` is 3840x2160 HEVC Main 10/yuv420p10le Dolby Vision Profile 5
  at 24000/1001 fps with 5.1 E-AC-3; `legend.mkv` is 3840x2160 HEVC Main
  10/yuv420p10le limited-range BT.2020/PQ at 25 fps with 5.1 E-AC-3. Both local
  files retained D3D11VA, RGB10/PQ output, zero decoder-surface copies, and no
  decoded-source CPU map.
- settled `wednesday.mp4` windows scheduled 23.8-24.2 fps but commonly rendered
  22.8-23.8 fps with 1-7 retryable skips per five seconds and repeated
  80-138 ms render gaps. Settled `legend.mkv` windows scheduled 24.9-25.2 fps
  but commonly rendered 23.3-24.8 fps with 1-9 retryable skips. `Present()`
  never reported busy, redraw coalescing was normally zero, and hardware-path
  draw maxima were normally about 19-31 ms, below the 40-41.7 ms source-frame
  budgets. Twelve-second process GPU samples were also far below saturation:
  the video-codec engine averaged about 19.5% and the 3D engine about 1.2-1.5%.
- the first repeatable loss is therefore after Player schedules a video frame
  and requests a render, but before a successful renderer draw or swap-chain
  present. Disabling D3D11VA did not remove the skips, increasing the retained
  GPU-completion ring from three to sixteen did not remove them, and reduced
  window output did not remove them. These controls reject decoder throughput,
  the three-frame completion bound, and compositor backpressure as the primary
  cause on this system.
- a diagnostic-only control changed the Player state acquisition inside
  `renderVideo()` from its single `try_lock` to a blocking lock. With all other
  conditions restored, `legend.mkv` then produced consecutive settled windows
  at 24.8-25.1 rendered fps with zero skips and no greater-than-80-ms gaps;
  `wednesday.mp4` produced consecutive 23.9-24.0 fps windows with the same
  result apart from one isolated later retryable skip. No control change is
  retained. This identifies transient Player state-lock contention as the
  dominant cause: `renderVideo()` returns a generic negative result and
  `D3D11VideoOutput` consumes that request as `render-skipped` without a
  bounded retry, so a momentary lock collision becomes a visibly missing
  frame. The remaining rare skip needs reason-level retry instrumentation.
- this evidence rejects an AMD renderer-throughput bottleneck under the
  recorded conditions and points to a generic cross-vendor retry-semantics
  defect. A final correction must preserve non-blocking render/control and
  surface-lock ordering rather than simply making the Player lock blocking.
  The 60 Hz display also imposes ordinary uneven 23.976/25 fps display cadence,
  but that cannot explain QtAVCore's counted skipped renders. The Intel
  same-build regression, trusted-player cadence comparison, reason-level
  residual-skip trace, and validated bounded correction remain pending.
- all diagnostic controls were removed and the exact current non-blocking
  policy was rebuilt. Its final shared Release CTest run passed 36/36. An
  earlier run had passed 35/36 because the H.264/NV12 zero-copy lifecycle test
  reached its 15-second stop wait and deliberately aborted in `ucrtbase.dll`;
  both an immediate isolated rerun and the final full run passed. Treat this as
  an intermittent scheduling signal to reproduce, not as a new GPU-driver
  crash or an AD-007 synchronization failure without further evidence.

AMD correction checkpoint on 2026-08-04:

- reason-level Stage A instrumentation showed the original loss was dominated
  by `PlayerStateBusy`. Replacing the Player hot-path state lock with atomically
  published immutable frame and renderer-binding snapshots reduced that reason
  to zero and exposed the next boundary: D3D11 immediate-context contention.
  A timer-only retry was not sufficient; the FFmpeg/D3D11VA worker could
  release and immediately reacquire the recursive context lock, producing
  roughly 80-94 context-busy attempts and 9-14 terminal frames in representative
  five-second windows.
- the retained correction adds `VideoRenderResult` with status, frame sequence,
  and presentation generation; rejects backend completions invalidated by seek,
  stop, or media replacement; keeps a one-frame latest retry mailbox; and
  separates retry wakeups, supersession, and terminal drops. The shared D3D11
  context now supports a move-only render-thread reservation honored by
  FFmpeg/internal acquisitions plus bounded timed acquisition. The output's
  first retry enters an at-most-8-ms handoff immediately, then uses
  1/2/4/8/16-ms timer backoff only after a timeout. It does not busy-spin or
  block the UI thread, and it releases the context before statistics or
  `Present()`.
- in the final shared Release WinUI build, `wednesday.mp4` ran for roughly
  55 seconds at about 23.8-24.1 presented fps. Every visible window from startup
  onward reported `superseded/terminal=0/0`; context contention was 1-10 per
  window, every owner was reservation-aware, and all 1-10 handoffs completed
  without timeout. Player-busy, present-busy, and decoder-copy counters remained
  zero.
- the same process replaced the media with `legend.mkv`. The generation switch
  produced one expected no-frame attempt, then every visible window retained
  `superseded/terminal=0/0`; context contention was 4-14 per window, all owners
  were reservation-aware, and every handoff completed without timeout.
  D3D11VA, RGB10/PQ, zero decoder copies, zero decoded-source CPU mapping, zero
  Player-busy attempts, and zero present-busy results remained intact.
- deterministic coverage now exercises detailed result compatibility, stale
  render-generation rejection, reservation fairness and recursion, zero and
  bounded timed acquisition, retry recovery without a second redraw request,
  and latest-frame supersession. The D3D11 device-access and composition-output
  tests passed five and twenty repeated Release runs respectively; the core
  playback test also passed five repeated runs. Fresh ClangCL shared and static
  Release trees each passed all 36 registered CTest tests. Both install layouts
  configured, built, and ran external consumers of `QtAV::Core`,
  `QtAV::PlatformWindows`, `QtAV::RenderD3D11`, and `QtAV::OutputD3D11`. The
  Release WinUI player rebuilt with zero warnings and zero errors. A Debug
  attempt reached the existing dependency-package limitation: the repository
  prefix supplies only Release `placebo.lib` (`_ITERATOR_DEBUG_LEVEL=0`) and no
  debug variant, so ClangCL correctly rejected linking it with Debug objects
  (`_ITERATOR_DEBUG_LEVEL=2`); no ABI-unsafe override was retained.
- the AMD evidence rejects a renderer-throughput bottleneck and validates the
  generic retry/handoff correction on the supplied Dolby Vision Profile 5 and
  HDR10 workloads. The same-build Intel regression, remaining common media and
  lifecycle matrix, and trusted-player comparison remain open; item 4 was
  provisionally complete at this checkpoint, while the overall
  user-prioritized performance task was not.

AMD proactive-handoff refinement on 2026-08-04:

- a final rebuilt-Release check under heavier sustained load reproduced one to
  two superseded/terminal frames per five seconds on `legend.mkv` even though
  every retry used the reservation-aware FFmpeg owner path. The reservation
  had still been established only after the renderer's first failed context
  attempt, leaving that initial pass open to decode-side overtaking;
- the output now establishes its non-owning reservation before every pass's
  first non-blocking context attempt. An uncontended pass still proceeds
  immediately. A contended pass waits at most 8 ms, and only a timeout reaches
  the existing one-frame mailbox and timer backoff. The context remains
  released before statistics and `Present()`;
- the shared Release composition test passed twenty consecutive runs after
  this refinement. In subsequent Radeon 880M runs, both supplied files kept
  renderer-busy, retry/superseded/terminal, Player-busy, Present-busy, and
  decoder-copy counters at zero; intercepted handoffs completed without
  timeout;
- fresh post-refinement ClangCL shared and static Release builds each passed
  all 36 registered CTest tests. Both install layouts then reconfigured and
  rebuilt the external `QtAV::Core`, `QtAV::PlatformWindows`,
  `QtAV::RenderD3D11`, and `QtAV::OutputD3D11` consumer, and both executables
  exited successfully. The Release WinUI player also rebuilt with zero
  warnings and zero errors before the final connected playback runs;
- that same observation window did not reproduce the earlier throughput
  baseline: `legend.mkv` commonly presented about 19-21 fps with 57-75 ms draw
  maxima and roughly 70% process 3D-engine utilization, while the longer
  process later entered repeated WASAPI underrun/buffering transitions.
  `wednesday.mp4` then scheduled only about 19-21 fps and presented about
  18-20 fps. The machine reported AC power and the balanced plan, but the run
  followed prolonged native builds and UI capture. This was separate from the
  now-zero terminal context-drop counter;
- after an idle interval, a fresh process kept Debug closed for its first
  35 seconds. `legend.mkv` then produced consecutive 24.9-25.1 fps windows,
  zero steady coalescing, zero renderer-busy/retry/superseded/terminal results,
  2-9 successful handoffs with zero timeout, and warm draw maxima around
  36-43 ms. The same process replaced it with `wednesday.mp4`, which settled at
  23.8-24.1 fps with zero coalescing and the same zero busy/terminal counters;
  its warm draw maxima were about 35-40 ms. D3D11VA, RGB10/PQ, zero decoder
  copies, zero Player busy, and zero Present busy remained intact. This rejects
  a persistent renderer regression from proactive reservation and completes
  item 4 again; the stressed throughput state remains a useful environmental
  caution, not an accepted optimization target.

AMD repair and verification are complete: the loss is located in generic
retry/context handoff, the proactive bounded correction is validated, and the
cold source-rate baseline is restored. NVIDIA now has the required adapter,
driver, build, objective cadence, stage, zero-map, and lifecycle evidence. Do
not close the Windows cross-vendor task until Intel records the same evidence
and any remaining device or workload limits.

Completed NVIDIA checkpoint on 2026-08-05; items 3 and 5 now remain open only
for Intel:

- commit `dc23a099302f1bb19df8209259560c1aedd8f2e1`, shared Release
  QtAVCore built with the Visual Studio 2026 ClangCL/lld configuration and the
  Release WinUI 3 player. The system ran Windows 11 Enterprise build
  `26200.8246`, an Intel Core i5-14500 with 31.7 GiB RAM, balanced power plan,
  and no battery. The display-driving NVIDIA GeForce RTX 3050 reported
  `PCI\\VEN_10DE&DEV_2584&SUBSYS_184610DE`, driver `32.0.15.9186` (591.86),
  with an Intel UHD Graphics 770 also present but not exercised. The player was
  windowed with a 1708x814 composition surface on a 3840x2160/59 Hz HDR
  display; output reported RGB10/PQ, 240-nit SDR white, and a 1405-nit display
  peak. This refresh and display peak do not exactly match the earlier AMD
  system and are recorded as comparison limits.
- the initial WinUI/Core/backend rebuild completed. The initial shared Release
  CTest result was 34/36. One D3D11VA lifecycle run fast-failed
  in `ucrtbase.dll`, while five immediate isolated repetitions passed. The
  initial D3D11 composition access violation was not a valid backend result:
  the WinUI-target build had refreshed the August 5 shared DLLs but had left an
  August 4 test executable in place. Commit `dc23a09` enlarged the public,
  by-value `D3D11VideoOutputStatistics` result from 120 to 232 bytes. The stale
  caller supplied the old return buffer while the new DLL wrote the new layout,
  overwriting 112 bytes of caller stack; the later
  `boundSwapChain = swapChain` failure was a consequence, not the cause.
  Rebuilding `qtav_output_d3d11_test` removed the access violation. This is a
  build-consistency failure and also concrete evidence that an unrecompiled
  shared-library consumer is unsafe across this public ABI change; CTest must
  follow a complete build, and a released ABI would require versioning or an
  extensible statistics boundary.
- the rebuilt composition test exposed a separate timing-sensitive test
  assumption rather than a graphics failure. With a complete generated media
  file, the unchanged test passed 19/20 runs; its one failure asserted that no
  frame could be presented while the test held the immediate-context guard.
  The output deliberately releases that guard before statistics and Present,
  so an already-rendered frame can complete its Present/callback after the test
  samples the counter. Merely weakening that equality moved 3/50 failures to
  the corresponding recovery-counter assertion. A diagnostic-only experiment
  instead acquired the guard, allowed the pre-existing Present/callback to
  drain, then sampled the baseline; all strict busy, retry, supersession,
  terminal, and recovery assertions passed 50/50. The experiment was removed.
  The test fixture needs this synchronization correction before treating an
  isolated assertion abort as a backend or driver regression. The cached media
  fixture also referenced a removed FFmpeg 8.1.2 executable; the test media was
  regenerated successfully with the installed host FFmpeg 9.0.
- the composition test now makes that ordering deterministic. While it owns the
  context guard, it waits until the serial render worker reports the expected
  context-busy attempt; that attempt proves the earlier render/Present callback
  has drained before the test samples its presentation baseline. The strict
  busy, retry, supersession, terminal, and recovery assertions then passed 50
  consecutive direct runs. After updating the cached media-generator path, a
  clean full shared Release rebuild completed, all 36 CTest tests passed, and
  the composition test passed another 20 consecutive CTest runs. The Release
  WinUI player rebuilt separately with zero warnings and zero errors.
- `legend.mkv` retained 3840x2160 D3D11VA HEVC Main 10, BT.2020/PQ input and
  HDR RGB10/PQ output. After the cold window scheduled/rendered 24.6/24.4 fps,
  more than two minutes of settled windows normally scheduled and rendered
  24.8-25.1 fps with zero coalescing, Present busy, render skips, terminal
  drops, or decoder-surface copies. Warm draw maxima were normally about
  11-16 ms. One isolated settled window recorded a 90.2-ms render gap and a
  36.2-ms color-stage maximum without losing a counted frame. A seek to 22:48
  buffered for about 46 ms; its first window rendered 24.0 fps with four
  coalesced requests, and the next returned to 25.0 fps. That next window
  intercepted one context-busy attempt, used one retry and one bounded handoff
  timeout, and still kept `terminal=0`, directly exercising successful recovery
  from the transient contention fixed by the proactive handoff work.
- `wednesday.mp4` retained 3840x2160 D3D11VA HEVC Main 10 Dolby Vision Profile
  5 input and HDR RGB10/PQ output; the container track reports unknown static
  color fields as expected for this workload. Cold loading completed in about
  112 ms and the first frame was presented about 192 ms later. Over more than
  60 seconds, settled windows scheduled 23.8-24.2 fps and rendered 23.9-24.1
  fps with zero coalescing, Present busy, render skips, terminal drops, and
  decoder-surface copies. Warm draw maxima were normally about 13-16 ms. A seek
  to 29:39 buffered for about 14 ms; the seek window rendered 22.2 fps with one
  coalesced request and the expected roughly 460-ms discontinuity, then the
  next window returned to 24.0 fps with every loss/backpressure counter zero.
- the same first process replaced `legend.mkv` with `wednesday.mp4` and played
  the replacement for almost six minutes before closing while playing. Both
  that process and the dedicated Dolby Vision process exited normally, and no
  `QtAVWinUI3` Application Error event was recorded. Twelve-second process GPU
  samples averaged about 11.9/0.09/0.01% video-decode/3D/copy for
  `legend.mkv` and 11.3/0.13/0.00% for `wednesday.mp4`; working set remained
  about 287 MiB. D3D11VA, `decoder-copies=0`, and the absence of any reported
  software fallback or decoded-source map remained intact throughout.
- the generated `C:\\test\\qtav-h264-nv12-control-1080p.mp4` is a 120-second
  H.264 High/yuv420p limited-range BT.709 control at 1920x1080 and 30000/1001
  fps with 48-kHz stereo AAC-LC. The player selected D3D11VA/NV12 with HDR
  RGB10/PQ composition output. Loading completed in about 13 ms and the first
  frame was presented about 141 ms after open. More than 80 seconds of settled
  windows scheduled/rendered 29.9-30.1 fps with zero coalescing, Present busy,
  render skips, renderer busy, retry/superseded/terminal results, decoder
  copies, or greater-than-80-ms warm gaps; warm draw maxima were about 13-16
  ms. A twelve-second sample averaged about 8.07% video-decode, 0.09% 3D, and
  0.00% copy utilization with about 181 MiB working set.
- a seek to 00:59 buffered for about 19 ms. Its transition window had five
  coalesced requests and the expected roughly 344-ms discontinuity but zero
  terminal result or decoder copy; the next windows returned to 29.9-30.1 fps
  with all loss/backpressure counters zero. In the same process, the control
  reached end of media and was replaced by `legend.mkv`; the replacement loaded
  as 3840x2160 D3D11VA HEVC Main 10 BT.2020/PQ, presented HDR RGB10/PQ with zero
  decoder copies, and continued playing. Closing at about 00:50 left no
  `QtAVWinUI3` process and no Application Error or Windows Error Reporting event.
- the NVIDIA matrix is complete for the generated H.264/NV12 control,
  `legend.mkv`, and `wednesday.mp4`, including cold start, sustained playback,
  seek, media replacement, and close while playing. A separate 4K SDR control
  was not available and is recorded as an optional matrix limit rather than a
  failure. The user already compared the current NVIDIA output with MPC-BE and
  accepted that trusted-player check. NVIDIA regression is therefore closed;
  exact trusted-player settings and the remaining Intel HDR10 seek-scene result
  still keep items 3, 5, and 7 partially open.

Intel checkpoint on 2026-08-05; the system record is complete, but the matrix
is not yet accepted:

- commit `61afd1e4508e4817b9de4c8419df12880d19eb37`; a fresh
  `build/modern-shared-intel` tree used Visual Studio 2026, ClangCL 22.1.3,
  the repository `x64-windows-static-md` dependency package, and shared Release
  QtAVCore. The complete build succeeded, all 36 CTest tests passed, and the
  Release WinUI 3 player rebuilt with zero MSBuild warnings and errors;
- Lenovo 21HW with a Core i5-13500H, 31.9 GB system memory, and Intel Iris Xe
  Graphics (`PCI\\VEN_8086&DEV_A7A0&SUBSYS_3C4817AA`, driver
  `32.0.101.7088`); Windows 25H2 build `26200.8894`, balanced power plan, and
  AC power online. The window used a 1708x814 composition surface on the
  primary Philips `PHL0979` display at 3840x2160/60 Hz. Windows HDR was active
  and output reported RGB10/PQ, 240-nit SDR white, and a 1405-nit display peak;
- the generated 120-second 1920x1080 H.264/yuv420p BT.709 control at
  30000/1001 fps with 48-kHz stereo AAC retained D3D11VA/NV12. A roughly
  70-second run scheduled and rendered 29.8-30.1 fps with zero coalescing,
  Present busy, render skips, retry/superseded/terminal results, decoder
  copies, or warm greater-than-80-ms gaps. A seek to 00:59 buffered for about
  22 ms; the following settled windows returned to 30.0-30.1 fps with every
  loss/backpressure counter zero;
- `wednesday.mp4` retained 3840x2160 D3D11VA HEVC Main 10 Dolby Vision Profile
  5 input, 24000/1001 fps, 5.1 E-AC-3, and HDR RGB10/PQ output. Both a
  same-process replacement run longer than 80 seconds and a dedicated cold run
  longer than 75 seconds settled at 23.8-24.2 scheduled/rendered fps with zero
  coalescing, Present busy, render skips, retry/superseded/terminal results,
  decoder copies, or warm greater-than-80-ms gaps. Warm draw maxima were
  normally about 12-16 ms. A seek to 29:39 buffered for about 20 ms; after its
  expected discontinuity window, cadence returned to 23.9-24.1 fps with every
  loss/backpressure counter zero. Twelve-second process samples averaged 3.59%
  3D, 6.78% video decode, and 0.00% copy utilization;
- `legend.mkv` retained 3840x2160 D3D11VA HEVC Main 10/yuv420p10le limited-range
  BT.2020/PQ input at 25 fps with 5.1 E-AC-3, HDR RGB10/PQ output, and zero
  decoder-surface copies. Before seeking, consecutive settled windows ran at
  24.8-25.2 scheduled and 24.9-25.1 rendered fps with zero loss/backpressure
  counters and normally 13-21-ms warm draw maxima. Pause for five seconds held
  the media clock and resume advanced normally. The first seek to 22:48 then
  produced a persistent 24.0-24.8 rendered fps range despite 24.9-25.1 fps
  scheduling, with 48-60-ms draw maxima, recurring Present busy/coalescing, and
  98-175-ms render gaps. Closing Debug for 30 seconds did not remove it;
- a dedicated cold rerun reproduced the stage change at the same 22:48 scene.
  It usually retained 24.9-25.2 rendered fps, but draw remained roughly
  49-52 ms with recurring Present busy and 84-131-ms render gaps, then two
  later windows fell to 24.8 and 24.5 fps with roughly 155-ms draw maxima and
  176-177-ms render gaps. In both runs Player scheduling remained at source
  cadence and `render-skipped`, `retry/superseded/terminal`, and
  `decoder-copies` remained zero. A twelve-second sample during the reproduced
  state averaged only 4.66% 3D, 7.37% video decode, and 0.00% copy utilization,
  so the evidence does not indicate decoder or GPU saturation;
- media replacement, Debug open/close, pause/resume, seek, and close while
  playing were exercised. Delivered close requests exited in about 226-386 ms
  without a `QtAVWinUI3` Application Error or Windows Error Reporting event.
  No separate 4K SDR control or Intel trusted-player comparison was completed.
  Keep the Intel task open for a controlled `legend.mkv` 22:48 diagnostic A/B;
  do not treat zero terminal drops alone as completion while repeatable warm
  render gaps and occasional cadence loss remain.

Intel clean-load follow-up on 2026-08-05 with the additional supplied
`suzume.mkv` does not satisfy the no-reproduction closure condition:

- the prior run overlapped other compilation and the user observed CPU thermal
  throttling, so its severity is treated as potentially load-amplified rather
  than a clean performance baseline. The follow-up reused the same commit and
  Release WinUI binary with no `cl`, `clang-cl`, `link`, `msbuild`, `cmake`,
  `ninja`, `gradle`, or other build process active. Before playback, five-second
  samples averaged 13.82% CPU utility, 96.54% processor performance, 2243 MHz
  frequency, and 0.20 processor-queue depth;
- `suzume.mkv` is 3840x1608 HEVC Main 10/yuv420p10le limited-range BT.2020/PQ
  at 24 fps with 5.1 E-AC-3. It retained D3D11VA, HDR RGB10/PQ output, zero
  decoder-surface copies, and zero render-skipped or terminal results;
- the cold start and more than 110 seconds before seeking settled at
  23.8-24.2 scheduled/rendered fps with zero coalescing, Present busy,
  retry/superseded/terminal results, decoder copies, or warm greater-than-80-ms
  gaps. Warm draw maxima were normally 12-15 ms. Playback samples averaged
  15.18% CPU utility, 112.88% processor performance, 2343 MHz frequency with a
  2600-MHz sample maximum, 0.08 processor-queue depth, 3.81% GPU 3D, 5.01%
  video decode, and 0.00% copy utilization. Pause for five seconds held the
  media clock and resume advanced normally;
- after a seek to 1:00:00, most windows still rendered 23.8-24.1 fps, but draw
  repeatedly rose to roughly 41-48 ms with 84-95-ms render gaps and intermittent
  Present busy; one window rendered 23.3 fps. Closing Debug did not remove the
  behavior: a later window rendered 23.4 fps with a 161-ms draw and 212-ms
  render gap. During that Debug-hidden interval, samples averaged only 9.86%
  CPU utility, 114.32% processor performance, 2445 MHz frequency with a
  2600-MHz maximum, zero processor-queue depth, 4.75% GPU 3D, 6.13% video
  decode, and 0.00% copy utilization;
- a second seek to 1:40:00 produced a 22.6-fps transition window with a
  70.8-ms draw and 197-ms render gap. Subsequent windows returned to
  23.9-24.0 fps but retained intermittent roughly 41-43-ms draw maxima,
  82-102-ms render gaps, and occasional Present busy. Player scheduling stayed
  at source cadence and all retry/superseded/terminal and decoder-copy counters
  remained zero;
- closing while playing exited in about 231 ms without a `QtAVWinUI3`
  Application Error or Windows Error Reporting event. Temperature telemetry
  was not available, but the processor performance/frequency, queue, and GPU
  samples show no concurrent load collapse during the reproduced windows.
  Thermal throttling may have amplified the earlier `legend.mkv` result, but
  it does not explain away this clean-load, Debug-independent recurrence.
  Keep the Intel issue open and include both `legend.mkv` 22:48 and
  `suzume.mkv` 1:00:00/1:40:00 in the controlled diagnostic A/B.

Intel D3D11 pass-submission follow-up on 2026-08-05 narrows the issue but does
not yet justify a production fix:

- renderer/output statistics now split CPU wall time across completion-query
  retirement/acquisition, render-target clear, `pl_render_image()`, completion
  `End()`, and retained-resource insertion. The libplacebo render-info callback
  also records pass count, pass-graph changes, asynchronous rolling GPU time,
  callback arrival, and time after the last pass callback. The diagnostics do
  not add a GPU finish, decoded-surface copy, CPU map, or blocking wait;
- before seeking, `suzume.mkv` again held 23.8-24.2 fps with one libplacebo
  raster pass, warm `pl_render_image()` time normally 11-16 ms, and rolling GPU
  pass time normally about 1.3-2.1 ms. The cold first pass took roughly
  154-176 ms of CPU wall time while its GPU sample remained about 1.5-1.9 ms;
- after a single pointer seek to about 1:00:00, repeated slow windows measured
  roughly 40-49 ms inside `pl_render_image()`. The pass count stayed one, the
  pass graph did not change after the seek transition, and rolling GPU time
  stayed about 1.7-1.9 ms. A second instrumented run placed the successful-pass
  callback at the end of the entire 40-45-ms interval and measured effectively
  zero time after the callback. This localizes the CPU stall before the
  successful pass callback, inside libplacebo's D3D11 pass execution/timer
  query path, rather than QtAV scheduling, post-pass cleanup, or genuinely long
  shader execution;
- the temporary `SdrOnly` A/B reported BGRA8/80-nit SDR and still reproduced
  post-seek pass CPU maxima of roughly 42-86 ms while rolling GPU time remained
  about 1.4-1.9 ms. Occasional `ClearRenderTargetView()` maxima of 20-34 ms
  appeared in the same later interval. Native RGB10/PQ was restored after the
  experiment. RGB10, PQ encoding, HDR metadata, and the HDR output shader are
  therefore not necessary conditions for the regression;
- libplacebo 7.351.0 executes this raster pass through a dynamic vertex/index
  stream-buffer upload (`Map` with `WRITE_NO_OVERWRITE` or `WRITE_DISCARD`),
  immediate-context bindings, `Draw`, resource unbinding, GPU timer `End`, and
  a non-blocking timer-query poll before the callback. Current QtAV-level
  instrumentation cannot distinguish which one of those internal D3D11 calls
  is blocking. A WPR `GPU` profile could provide that split, but starting it in
  the current unelevated session failed with `0xc5585011` (system-performance
  profiling policy); no trace was produced. The next evidence step is an
  elevated GPUView/WPA trace or a temporary instrumented libplacebo build,
  followed by the remaining output-size, hardware-decode, and swap-chain-buffer
  A/Bs;
- the progress slider handles pointer press, release, and capture loss;
  release/capture loss clears scrubbing and commits exactly one seek, while
  passive pointer hover has no seek handler. In the reproduction, the cursor
  remained at the released slider coordinate for about 45 seconds and the log
  contained exactly one seek. Moving it into the video area did not remove the
  repeated stalls in the earlier run. A stuck pointer release would instead
  leave the progress UI in scrubbing mode and omit the committed seek. Mouse
  hover is therefore excluded as the cause of the post-seek D3D11 stalls.

The prior Android task is retained below as completed history, not as the
active next step.

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

The existing Android playback-stutter correction remains complete. Preserve
this OpenGL reopen/long-form run as the Android regression gate. Intel validates
the remaining Windows correction on the transferred administrator-capable
machine, while local work proceeds from the completed render-attempt and OHOS
target/toolchain gates into the HAP/XComponent integration slice.

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
4. [x] Keep hardware decode, D3D11 texture interop, libplacebo rendering, and
   final presentation as separate responsibilities and targets where the
   existing module boundaries require them.
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

Historical implementation slice (completed, then superseded):

1. [x] Add the common Windows D3D11 device-access target and shared recursive
   context guard.
2. [x] Add the opaque supplied-hardware-device token and private FFmpeg bridge
   in core.
3. [x] Prove device identity, COM lifetime, locking, and install/export
   behavior with deterministic tests before opening the native decoder.

Completed Windows D3D11 device-access checkpoint:

- `QtAV::PlatformWindows` verifies that a selected context is the chosen
  device's immediate context, retains both COM interfaces, and exports
  `D3D11DeviceAccess`, a move-only recursive `D3D11ContextGuard`, bounded timed
  acquisition, and move-only reservation priority for FFmpeg-aware context
  owners;
- the D3D11 renderer accepts shared device access while preserving its
  borrowed device/context convenience constructor, and it holds the common
  guard for immediate-context rendering calls;
- deterministic WARP tests cover null, foreign, and deferred-context
  rejection, retained COM lifetime, same-thread recursion, cross-thread
  exclusion, guard lifetime, reservation fairness, zero/bounded timed waits,
  and renderer participation in the shared lock;
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

Historical D3D11 Video Processor interop checkpoint (superseded):

This checkpoint records the former RGB-intermediate implementation. The
current Windows renderer instead retains raw NV12/P010 decoder slices and lets
libplacebo own plane sampling, Dolby Vision/HDR processing, and final output.

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

1. [~] External Windows machine: complete the Intel administrator trace,
   evidence-backed correction, and same-commit matrix described in `Next task`;
   AMD repair and NVIDIA verification are complete.
2. [~] This machine: the portable render-result contract, target/toolchain
   gate, HAP/XComponent Vulkan and OpenGL ES adapters, and shared-selector
   fallback and OHAudio output are complete; continue with OHCodec direct-
   surface hardware decode without waiting for the external Intel trace.

Platform implementation order after the contracts are stable:

1. Windows reference path.
2. Android production path with connected-device validation.
3. Android playback performance/regression work.
4. OHOS production path with connected-device validation (active local slice).

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
  redraw-coalescing render thread, reason-aware `renderVideoDetailed()`, bounded
  latest-frame retry/context handoff, `Present()`, resize, D3D11VA/raw-plane
  libplacebo setup, per-frame composition-monitor/Advanced Color tracking,
  FP16 scRGB HDR presentation, and teardown, retaining the borrowed-target
  renderer as the advanced external-context path.

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

Status: complete; Milestone 6 is also complete. AD-007 is accepted across
NVIDIA, Intel, and AMD; the AMD integrated-GPU cadence correction and NVIDIA
same-commit matrix are fully validated. The Intel post-seek performance matrix
remains incomplete on the external Windows track, while Milestone 7 is active
locally.

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

Status: complete. Its connected-device results remain the Android regression
baseline. The Intel matrix continues on the external administrator-capable
Windows machine; the portable Android/OHOS result contract and OHAudio slice
are complete and local work proceeds into OHCodec hardware decode.

## Active milestone 7 — OHOS production path

This is now the active local development milestone. The transferred Intel
Windows performance issue remains open in parallel and must not be described as
fixed or closed. Target clarification, the OHOS arm64/API 23 dependency and
toolchain validation, portable render-result contract, Vulkan/OpenGL ES
selector path, and OHAudio sink are complete and connected-device validated.
The next local slice is OHCodec hardware decode with direct
`OHNativeWindow` presentation first.

Target clarification gate:

- [x] The initial connected target is a commercial HUAWEI Mate 60 Pro
  (`ALN-AL80`) running HarmonyOS 6.1.0.135 / OpenHarmony 6.1.1.120, API 24,
  arm64-v8a. The build baseline uses DevEco Studio 6.1, OpenHarmony SDK
  6.1.1.125/API 24, and an arm64/API 23 minimum target. DevEco debug signing is
  configured; HDC 3.2 installs and launches a signed HAP. The device exposes
  H.264/HEVC OHCodec decoders, OHAudio construction, `OH_NativeBuffer`, and
  `VK_OHOS_external_memory`. A generic OpenHarmony distribution remains a
  secondary compatibility target without a recorded device validation.

### Toolchain and application shell

- [x] The reproducible OHOS arm64/API 23 FFmpeg 8.1.2 dependency cross-build
  under `../ffmpeg/` supports both macOS and 64-bit Windows hosts. The Windows
  entry handles the DevEco `Program Files` path through a stable junction,
  enables `--enable-ohcodec`, and verifies the installed H.264/HEVC OHCodec
  decoder symbols.
- [x] The Windows-hosted QtAVCore OHOS script configures the repository vcpkg
  and OHOS chainload toolchains, then builds and installs Release static and
  shared SDKs. The shared build applies `-Wl,-Bsymbolic`; FFmpeg pkg-config
  metadata carries libsmb2, OpenSSL, dav1d, and OHCodec system libraries into
  the first clean configure.
- [~] Add QtAVCore OHOS CI execution and production HAP playback/device
  validation. The existing dependency CI remains on its macOS self-hosted
  runner. Local signed-HAP XComponent Vulkan/OpenGL ES presentation and
  one-way selector fallback plus OHAudio output are now validated; CI execution
  and OHCodec playback remain pending.

Windows toolchain validation on 2026-08-05 rebuilt the complete target
dependency closure locally from source in 17 minutes and passed
`verify-install.cmake`. A clean shared QtAVCore build linked 26/26 steps and a
clean static build linked 19/19 steps; both installed successfully. A separate
external project then consumed all installed QtAVCore targets with
`find_package(QtAVCore)` and produced an AArch64 shared object. The installed
core ELF records `libnative_media_vdec`, `libnative_media_venc`,
`libnative_media_codecbase`, `libnative_media_core`, and `libnative_window` as
runtime dependencies and carries the `SYMBOLIC` flag. A fresh Windows x64
ClangCL shared build also passed after the cross-target pkg-config discovery
change.

- [~] Add the `modern/platform/ohos/` root; small shared helpers are still
  pending, and media, graphics, and audio implementations remain in their
  responsibility-specific backend targets.
- [x] Minimal HAP/native application shell using ArkUI/XComponent only at the
  integration boundary.
- [x] Connected-device deployment, signing, logging, generated-media playback,
  and automated result collection through thin OHOS-specific adapters to the
  shared mobile test scenarios.

### Vulkan and OpenGL ES rendering

- [x] Reuse the Android-proven platform-neutral Vulkan renderer engine,
  shaders, color conversion, geometry, synchronization rules, golden vectors,
  and renderer contract tests.
- [x] Add the OHOS `OHNativeWindow`/XComponent Vulkan surface and swapchain
  adapter as a separate target or platform helper.
- [x] Reuse OpenGL ES renderer internals where compatible, with a separate OHOS
  EGL/window adapter and explicit capability checks.
- [x] Reuse the Android-proven renderer selector and one-way Vulkan-to-OpenGL
  ES policy while keeping OHOS window, EGL, and error classification in its
  own adapter.
- [~] Validate software YUV/NV12/P010/RGB upload, viewport, aspect ratio,
  rotation, resize, redraw, surface loss/recreation, SDR, and supported HDR
  output behavior on a real device. The shared renderer retains its existing
  deterministic coverage; OHOS device validation currently proves generated
  YUV420 software decode, fit scaling, redraw, SDR Vulkan and OpenGL ES
  presentation, native-window recreation in both adapters, forced initial
  OpenGL ES selection, and fatal one-way Vulkan-to-OpenGL ES fallback on one
  media open. The remaining upload families, rotation, surface-loss injection,
  and native HDR behavior remain pending.

### Audio and hardware decode

- [x] Add an OHOS OHAudio sink target with negotiated PCM, bounded callback-safe
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
