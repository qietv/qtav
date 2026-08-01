# QtAVCore

QtAVCore is the Qt-free evolution path for QtAV. It keeps FFmpeg as the media
engine, replaces Qt types and the Qt event model with standard C++17, and uses
high-level platform outputs for ordinary presentation while retaining an
application-owned rendering callback, inspired by the public shape of
`mdk-sdk`, for custom graphics integration.

It is not source- or binary-compatible with `mdk-sdk`, and no `mdk-sdk` source
code is used.

## Current scope

- no Qt headers, libraries, meta-object compiler, or event loop;
- asynchronous `qtav::Player` state machine;
- FFmpeg 8+ send/receive decoding API;
- local files and FFmpeg-supported network protocols, with bounded HTTP(S)
  read timeouts and reconnect defaults that applications can override through
  `avformat.*` properties;
- audio and video frame callbacks with reference-counted frame lifetime;
- structured video range, primaries, transfer, matrix, chroma-location, HDR10
  mastering-display, and content-light metadata;
- `prepare`, `seek`, pause/resume/stop, playback rate, A-B range, and loop;
- media/track information and `avformat.*` property forwarding;
- decoder-driven `setRenderCallback()` plus render-thread `renderVideo()`;
- compile-time `VideoRenderAPI`, `AudioSink`, and hardware-frame interop
  contracts;
- optional libswscale CPU renderer for application-owned image buffers;
- optional Windows D3D11 renderer for a retained application-selected device
  and immediate context plus borrowed current render-target/swap-chain views,
  including Windows Advanced Color SDR, FP16 scRGB, and RGB10 HDR10 output;
- optional high-level Windows D3D11 composition output that owns the device,
  HDR-aware FP16 scRGB or SDR swap chain, render target, display tracking,
  redraw-coalescing thread, D3D11VA/interop wiring, `renderVideo()`,
  `Present()`, resize, and teardown;
- optional Metal renderer for borrowed Apple devices, command queues, and
  current render targets, including complete macOS/iOS extended-linear
  BT.2020 EDR layer presentation;
- optional libswresample converter for negotiated interleaved PCM output;
- optional RIFF/WAVE PCM diagnostic file sink;
- optional macOS CoreAudio device sink with native playback timing;
- optional Windows WASAPI shared-mode device sink with native playback timing;
- optional Android AAudio device sink with callback-safe bounded buffering,
  device timing, latency reporting, and disconnect recovery;
- optional D3D11VA hardware decoding on an application-selected retained
  D3D11 device, with reference-counted decoder texture-array slices and
  explicit software fallback;
- optional VideoToolbox hardware decoding with reference-counted
  `CVPixelBuffer` frames and explicit software fallback;
- optional Android MediaCodec H.264/HEVC hardware decoding into an
  application-supplied, versioned `ANativeWindow`, with explicit present,
  monotonic-time present, drop, stale-generation rejection, and software
  fallback policy;
- optional Android MediaCodec-to-Vulkan zero-CPU-copy texture interop through
  a private GPU-sampled `AImageReader`, retained `AHardwareBuffer` external
  memory, native YCbCr sampling, and acquire/release fence bridging;
- optional Android MediaCodec-to-OpenGL ES zero-CPU-copy texture interop
  through a detached `SurfaceTexture`, timestamp-correlated
  `updateTexImage()`, and `GL_TEXTURE_EXTERNAL_OES` sampling;
- optional CVMetalTextureCache interop for zero-copy limited/full-range
  VideoToolbox-frame rendering through Metal;
- optional platform-neutral Vulkan software-frame renderer using borrowed
  application-selected device/queue and current-image resources, with SDR,
  HDR10/PQ, HDR10/HLG, and extended-linear output contracts;
- optional Android Vulkan surface adapter that retains the current
  `ANativeWindow` generation, selects a supported SDR/native-HDR swapchain,
  publishes its output color space, and owns surface/swapchain synchronization;
- optional platform-neutral OpenGL ES 3.x software-frame renderer plus an
  Android EGL adapter that owns its display, context, window surface, and
  surface generation, selects native 10-bit BT.2020/PQ or BT.2020/HLG when
  available, and preserves an explicit RGBA8/sRGB SDR fallback;
- optional platform-neutral mobile renderer selector that performs
  Vulkan-preferred startup, bounded same-API recovery, fatal one-way fallback
  to OpenGL ES, and an explicit no-renderer state;
- explicit cross-API hardware-frame fallback decisions that rebind subsequent
  decoder output to compatible OpenGL ES native interop or select direct
  surface, software decode, or no video without retrying or mapping a frame
  produced for the retired Vulkan surface;
- an accepted Android/OHOS mobile rendering policy that prefers Vulkan and
  uses a separate OpenGL ES/EGL backend after Vulkan is unavailable or fails
  fatally, while keeping recoverable surface recreation within the active API;
- a reproducible macOS-to-Android arm64 build and connected-device
  NativeActivity harness for QtAVCore plus pinned FFmpeg 8.1.2 software
  decoding, Vulkan presentation, OpenGL ES/EGL native-HDR plus SDR fallback,
  AAudio output, MediaCodec H.264/HEVC direct-surface validation, and
  H.264/HEVC private-AImageReader Vulkan plus SurfaceTexture external-OES
  texture paths;
- standalone CMake package and headless integration tests.

The core does not open a platform audio device by default. Applications can
keep consuming decoded frames through `onAudioFrame()` and can optionally bind
an `AudioSink`; the macOS CoreAudio, Windows WASAPI, and Android AAudio
implementations remain separate backend targets so the core acquires no Qt or
platform dependency.

Current backend integration boundary:

- `VideoRenderAPI` is connected to `Player` and supports multiple renderer
  instances keyed by application-owned opaque pointers;
- `AudioSink` is connected through `Player::setAudioSink()`, follows playback
  lifecycle changes, and supplies the playback master when its device clock is
  supported and valid; decoded PCM crosses a bounded queue to a dedicated
  audio-output worker; queued audio is drained at completed playback segments,
  including loop boundaries, and before final close; seek/underrun buffering
  freezes `position()` until the device clock re-anchors (or callback-only
  output is actually delivered);
- `AudioFrameConverter` is connected through
  `Player::setAudioFrameConverter()` when a sink negotiates different PCM;
  `QtAV::AudioResample` supplies the portable libswresample implementation;
- `QtAV::AudioFile` writes negotiated interleaved PCM to a standard RIFF/WAVE
  file for diagnostics without becoming a playback clock;
- `QtAV::AudioCoreAudio` negotiates Float32 mono/stereo PCM against the macOS
  output device, owns its AudioQueue buffers, and supplies a device-backed
  playback clock and latency;
- `QtAV::AudioWASAPI` negotiates shared-mode Float32 mono/stereo PCM against a
  Windows render endpoint, owns an event-driven queue on a dedicated COM
  thread, and supplies an `IAudioClock`-backed playback clock and latency;
- `QtAV::AudioAAudio` negotiates Float32 mono/stereo PCM against the current
  Android output route, feeds AAudio's real-time callback from a bounded
  lock-free queue, maps AAudio presentation timestamps to the media timeline,
  and rebuilds a disconnected default-route stream on a non-callback thread;
- `HardwareDecodeConfig` selects an optional hardware device for video decode;
  its optional reference-counted `HardwareDecodeDevice` lets an in-tree
  backend supply a pre-created native device without exposing FFmpeg or
  platform SDK types;
  `QtAV::HWVideoToolbox` supplies the Apple configuration helper and produces
  `HardwareFrame` values backed by retained `CVPixelBuffer` storage;
  `QtAV::HWMediaCodec` explicitly selects FFmpeg's MediaCodec wrapper decoder,
  binds it to a versioned application `ANativeWindow`, and turns each decoded
  output into a single-decision direct-surface present/drop token;
- `QtAV::InteropMediaCodecVulkan` owns a private GPU-sampled `AImageReader`,
  supplies its surface to `QtAV::HWMediaCodec`, correlates codec and image
  timestamps, imports retained `AHardwareBuffer` images and fences into the
  application-owned Vulkan device, and returns a release sync fd after GPU
  submission without a decoded-pixel map, software transfer, staging copy, or
  renderer upload;
- `QtAV::InteropMediaCodecOpenGL` owns a detached Android `SurfaceTexture`,
  supplies its producer window to `QtAV::HWMediaCodec`, releases each output
  once, correlates the single current image by timestamp and surface
  generation, and samples its transform through `GL_TEXTURE_EXTERNAL_OES`
  without a decoded-pixel map, transfer, staging copy, or renderer upload;
- `QtAV::RenderCPU` converts and scales decoded software frames into packed
  RGB/BGR/RGBA/BGRA/ARGB or Gray8 buffers;
- `QtAV::RenderD3D11` uploads software YUV420/422/444, NV12/NV21, P010,
  RGB/BGR/RGBA/BGRA/ARGB, or Gray8 frames and renders through an
  application-selected D3D11 device and immediate context plus the current
  application-owned render-target view;
- `QtAV::PlatformWindows` retains an application-selected D3D11 device and
  its verified immediate context behind a shared recursive context guard;
- `QtAV::HWD3D11VA` creates FFmpeg's hardware device on that selected D3D11
  device, shares the same context lock, and exposes retained NV12/P010 decoder
  texture-array slices through a Windows-only strong frame view;
- `QtAV::InteropD3D11` consumes same-device D3D11VA NV12/P010 texture-array
  slices through the D3D11 Video Processor and returns shader-readable SDR
  BGRA8, FP16 scRGB, or RGB10/PQ textures without mapping decoded pixels
  through CPU memory;
- `QtAV::OutputD3D11` combines the Windows device, renderer, decoder, and
  interop targets into a composition-surface output for ordinary applications;
  the application supplies a swap-chain binding callback, surface size, and
  normally its hosting HWND, then the output owns display/HDR tracking,
  render scheduling, and presentation;
- `QtAV::RenderMetal` uploads software YUV420/422/444, NV12/NV21, P010,
  RGB/BGR/RGBA/BGRA/ARGB, or Gray8 frames and renders into the application's
  current Metal texture or drawable, accepts an optional hardware-frame
  interop provider, and applies structured range/matrix/transfer/primaries
  metadata;
- `QtAV::InteropCVMetal` imports supported VideoToolbox `CVPixelBuffer` planes
  as retained Metal textures without mapping or copying through CPU memory,
  preserving whether the native pixel buffer is limited or full range;
- `QtAV::RenderVulkan` packs supported software planes into a Vulkan storage
  buffer and draws through an application-supplied current image, applying
  structured color metadata and the common viewport/aspect/rotation contract;
  the target's `VkColorSpaceKHR` selects deterministic HDR-to-SDR output or
  native HDR10/PQ, HDR10/HLG, or extended-linear encoding;
- `QtAV::RenderVulkanAndroid` owns Android surface, swapchain, image-view, and
  acquire/present resources for a retained active `ANativeWindow`, prefers
  native HDR format/color-space pairs when available, and submits HDR10 static
  metadata when the application enabled `VK_EXT_hdr_metadata`; the application
  owns the Vulkan instance, device, queue, and NativeActivity;
- `QtAV::RenderOpenGL` uploads the same software YUV420/422/444, NV12/NV21,
  P010, RGB/BGR/RGBA/BGRA/ARGB, and Gray8 families into OpenGL ES textures,
  applies the structured color path plus the common viewport/aspect/rotation
  contract, and writes explicit SDR sRGB, HDR10/PQ, or HDR10/HLG encoding into
  a caller-supplied current framebuffer;
- `QtAV::RenderOpenGLAndroid` retains the current `ANativeWindow` generation
  and owns its EGL display, OpenGL ES 3.x context, window surface, and swap,
  prefers an exact RGB10_A2 BT.2020/PQ surface, considers BT.2020/HLG, and
  falls back explicitly to RGBA8/sRGB while keeping Android and EGL types
  outside core public headers;
- `QtAV::RenderMobile` owns no graphics or platform resources. Applications
  supply Vulkan and OpenGL ES renderer factories for the current native-window
  generation; the selector keeps one stable `VideoRenderAPI` attached to
  `Player` across same-API recreation and one-way fallback. A synchronous
  hardware-frame fallback callback selects OpenGL ES native interop,
  direct-surface presentation, software decode, or no video for subsequent
  output; cross-API fallback never retries or maps the retired native frame;
- no Linux native backend or OHOS hardware decoder has been implemented yet;
  Android is the completed reference for the mobile renderer/hardware-interop
  fallback policy.

Mobile renderer creation remains in the application or thin platform layer
that owns the native window and graphics devices, while
`MobileVideoRendererSelector` implements the accepted shared selection policy.
A new session probes Vulkan first and selects OpenGL ES when Vulkan is
unavailable or cannot open its initial surface. `SurfaceLost` causes bounded
same-API recreation; a fatal error or exhausted Vulkan recovery retires Vulkan
for that session and retries the retained frame through OpenGL ES without
reopening media. `suspendSurface()` and `recreateSurface()` preserve the
selected API across an application-led native-window replacement. If both
APIs fail, video presentation reports unavailable while playback, audio, and
decoded-frame callbacks remain usable. Decoder, direct-surface, interop, and
renderer fallback policies remain independent. When the current frame is
hardware-backed, `setHardwareFrameFallbackCallback()` runs synchronously
after the OpenGL ES candidate is prepared and before any cross-API retry. The
application reconfigures subsequent decoder output and returns the chosen
route. Late frames from the retired native surface are discarded without
mapping; no callback or a `None` decision reports presentation unavailable.
The accepted design is
specified in [`MOBILE.md`](MOBILE.md).

The Android Vulkan and OpenGL ES zero-CPU-copy paths are separate backends.
Vulkan consumes private GPU-sampled
`AImageReader` images by importing retained `AHardwareBuffer` allocations,
native YCbCr/external formats, and acquire/release fences. OpenGL ES consumes
a detached MediaCodec `SurfaceTexture` through
`GL_TEXTURE_EXTERNAL_OES`, with explicit timestamp/generation correlation,
single-current-image lifetime, surface recreation, and seek/flush coverage.
P010/HDR external-OES sampling remains disabled unless the application
explicitly confirms device/codec/driver color control; private
`AImageReader` plus `AHardwareBuffer`/`EGLImage` remains a future optional
alternative. On OHOS, the confirmed GLES path uses
`OH_NativeImage` plus an external-OES texture. OHOS Vulkan additionally needs
a retained `OH_AVBuffer`/`OH_NativeBuffer` bridge because the current FFmpeg 8
OHCodec buffer branch performs `OH_AVBuffer_GetAddr()` plus
`av_image_copy2()` and therefore cannot satisfy this contract as-is. A
zero-CPU-copy claim requires zero decoded-pixel map, software transfer, CPU
staging, and re-upload calls plus verified native-buffer lifetime and fence
ordering. Unsupported imports are reported rather than silently mapped, and a
Vulkan-to-OpenGL ES renderer switch does not itself authorize a CPU copy.

## Build

Requirements:

- CMake 3.20 or newer;
- a C++17 compiler;
- FFmpeg 8.0 or newer development libraries;
- `pkg-config` is recommended but not required.

```sh
cmake -S modern -B build/modern -DQTAV_CORE_BUILD_TESTS=ON
cmake --build build/modern
ctest --test-dir build/modern --output-on-failure
```

Backend switches are cache strings with `AUTO`, `ON`, and `OFF` values. `AUTO`
enables a backend when its implementation and host requirements are available,
`OFF` always disables it, and `ON` requires it or stops configuration with a
clear error. Current switches are:

- render: `QTAV_RENDER_CPU`, `QTAV_RENDER_MOBILE`, `QTAV_RENDER_OPENGL`,
  `QTAV_RENDER_VULKAN`, `QTAV_RENDER_D3D11`, and `QTAV_RENDER_METAL`;
- audio: `QTAV_AUDIO_WASAPI`, `QTAV_AUDIO_COREAUDIO`, `QTAV_AUDIO_ALSA`,
  `QTAV_AUDIO_PULSEAUDIO`, `QTAV_AUDIO_AAUDIO`, `QTAV_AUDIO_RESAMPLE`, and
  `QTAV_AUDIO_FILE`;
- hardware decode: `QTAV_HW_D3D11VA`, `QTAV_HW_VIDEOTOOLBOX`,
  `QTAV_HW_VAAPI`, and `QTAV_HW_MEDIACODEC`;
- interop: `QTAV_INTEROP_D3D11`, `QTAV_INTEROP_CVMETAL`,
  `QTAV_INTEROP_MEDIACODEC_VULKAN`, and `QTAV_INTEROP_VAAPI`.
- output: `QTAV_OUTPUT_D3D11`.

`QTAV_RENDER_CPU=AUTO` builds the CPU renderer when libswscale is available,
`QTAV_RENDER_MOBILE=AUTO` builds the dependency-free mobile renderer selector,
`QTAV_RENDER_OPENGL=AUTO` builds the OpenGL ES renderer when GLES 3 headers
and libraries are available and adds the Android EGL adapter on Android,
`QTAV_RENDER_VULKAN=AUTO` builds the Vulkan renderer when a Vulkan loader and
`glslc` are available (the Android harness requires it explicitly),
`QTAV_RENDER_D3D11=AUTO` builds the native software-frame renderer on Windows,
`QTAV_RENDER_METAL=AUTO` builds the native renderer on Apple hosts with Metal,
`QTAV_AUDIO_RESAMPLE=AUTO` builds the PCM converter when libswresample is
available, `QTAV_AUDIO_FILE=AUTO` builds the dependency-free diagnostic sink,
`QTAV_AUDIO_WASAPI=AUTO` builds the shared-mode device sink on Windows,
`QTAV_AUDIO_COREAUDIO=AUTO` builds the macOS device sink when AudioToolbox
and CoreAudio are available, and `QTAV_HW_VIDEOTOOLBOX=AUTO` builds the Apple
hardware-decode selection and native-frame access target when VideoToolbox and
CoreVideo are available. `QTAV_INTEROP_CVMETAL=AUTO` builds the Apple
VideoToolbox/Metal interop target when the Metal renderer and CoreVideo are
available. `QTAV_HW_D3D11VA=AUTO` builds the Windows hardware-decode
selection and native-frame access target. `QTAV_INTEROP_D3D11=AUTO` builds the
Windows Video Processor adapter when the D3D11 renderer and D3D11VA decoder
targets are available. `QTAV_OUTPUT_D3D11=AUTO` builds the high-level Windows
composition output when the D3D11 renderer, D3D11VA decoder, and interop
targets are available. `QTAV_AUDIO_AAUDIO=AUTO` builds the AAudio sink on
Android API 26 or newer; the current Android harness targets API 28 and does
not require OpenSL ES fallback. `QTAV_HW_MEDIACODEC=AUTO` builds the Android
MediaCodec direct-surface backend when the NDK Media APIs and FFmpeg's
MediaCodec hardware context are available.
`QTAV_INTEROP_MEDIACODEC_VULKAN=AUTO` builds the private-AImageReader Vulkan
interop on Android API 26 or newer when the MediaCodec and Vulkan targets are
both available.
`QTAV_INTEROP_MEDIACODEC_OPENGL=AUTO` builds the SurfaceTexture external-OES
interop on Android API 28 or newer when the MediaCodec and OpenGL ES targets
are both available.
Backend implementations not otherwise described remain disabled under
`AUTO`, and explicitly requesting one with `ON` is an error.

Run the headless example:

```sh
build/modern/examples/qtav_core_console /path/to/media.mp4
```

Visual Studio multi-config builds place executables and project DLLs together
under `build/modern/bin/<Config>`, for example
`build/modern/bin/Release/qtav_core_console.exe`.

On macOS, when `QtAV::AudioCoreAudio` and `QtAV::AudioResample` are available,
the console example sends decoded audio to the default output device. On
Windows it does the same through `QtAV::AudioWASAPI` and, when the D3D11
targets are available, exercises D3D11VA plus `QtAV::InteropD3D11` into an
offscreen D3D11 render target. Other hosts retain callback-only audio
inspection.

Windows CTest runs the example against generated H.264/AAC media with
`QTAV_CORE_REQUIRE_NATIVE_WINDOWS_AV=1`. In that strict mode the example
requires hardware video frames, successful D3D11 rendering, decoded audio, and
a usable WASAPI endpoint. A session without an active render endpoint returns
CTest skip code 77, so unavailable device validation is not reported as a
pass. The strict test has also been exercised with an active endpoint, where
H.264/AAC playback passed and produced audible output.

### Android arm64 foundation harness

The initial Android production-path harness is under
`examples/android/`. It uses SDK CMake/Ninja and NDK r28c to cross-build a
checksum-pinned minimal FFmpeg 8.1.2 configuration and QtAVCore for
`arm64-v8a`, then uses AAPT2, zipalign, and apksigner to package a
NativeActivity without Qt or a Gradle dependency:

```sh
modern/examples/android/build-android.sh
modern/examples/android/run-connected-device.sh
```

Generated inputs and outputs remain under `build/android/`. The connected
device script requires exactly one authorized device, records ABI/API and
Vulkan facts, installs once, launches the generated-media playback test, and
collects its pass/fail log. If installation or replacement fails because the
device may be waiting for user authorization, stop and approve the prompt
manually before retrying. The Vulkan-enabled Android 16/arm64 device run now
decodes and presents 180 MPEG-4 video frames through an Adreno 830 Vulkan
1.3.284 native HDR10/PQ swapchain, decodes 282 PCM audio frames, submits
`VK_EXT_hdr_metadata`, and recreates the HDR surface/swapchain across a
background/foreground transition.

This is a toolchain, packaging, software-decode, Vulkan-rendering, and
OpenGL ES presentation checkpoint. The Vulkan renderer uses a bounded
three-frame resource ring and retains
each source frame until its fence completes. Platform-neutral offscreen
readback goldens run in the Android harness and cover YUV color conversion,
limited/full range, BT.601/BT.709 matrices, viewport, rotation, target
recreation, ring reuse, and P010/BT.2020 PQ/HLG input with mastering-display,
MaxCLL, and default-luminance selection. The same vectors verify both SDR
BGRA8 luminance compression and 10-bit native HDR10/PQ plus HDR10/HLG target
encoding, HLG-to-PQ conversion, and FP16 extended-linear/BT.2020-linear output
above reference white. The real-device harness requires an HDR swapchain,
records its format/color space, requires the Android compositor to report an
active HDR layer, presents a synthetic P010/BT.2020/PQ frame with mastering
and MaxCLL metadata, and verifies that the swapchain survives surface
recreation. The same device run now compiles the OpenGL ES 3 shaders, validates
offscreen uploads for YUV420/422/444, NV12/NV21, P010, RGB/BGR,
RGBA/BGRA/ARGB, and Gray8, checks viewport, rotation, and target-generation
replacement, and presents a P010/PQ-to-SDR frame through the real Android EGL
window adapter after the Vulkan lifecycle completes. It then requires an
RGB10_A2 BT.2020/PQ-or-HLG EGL surface, renders the same P010/PQ source without
SDR tone mapping, and independently verifies that Android reports the EGL
surface as an active HDR layer. The harness keeps one
`MobileVideoRendererSelector` attached to the player, recreates Vulkan across
the activity background/foreground window replacement, and separately proves
forced Vulkan-unavailable startup through both the explicit SDR and required
HDR OpenGL ES policies. The same run converts decoded PCM through
`QtAV::AudioResample`, presents it through `QtAV::AudioAAudio`, and validates
a monotonic device clock plus non-negative queued/device latency across the
pause/resume transition. It then explicitly selects FFmpeg's H.264 and HEVC
MediaCodec wrapper decoders, presents or drops their direct-surface outputs,
seeks, replaces media, stops explicitly, rejects an output token against the
old surface generation, and reopens against a replacement `ANativeWindow`
without mapping decoded pixels. A final texture-interoperable MediaCodec phase
runs through the private `AImageReader` target for both H.264 and HEVC:
the device imports external-format `AHardwareBuffer` images, samples them
through Vulkan YCbCr conversion, returns release sync fds, and requires all
decoded-source CPU-map, software-transfer, staging-copy, and renderer-upload
counters to stay zero. Separate H.264 and HEVC phases then use detached
`SurfaceTexture` producers, correlate MediaCodec output timestamps, sample
the current images as `GL_TEXTURE_EXTERNAL_OES`, cover seek/flush plus EGL
window suspension/recreation, and keep the same decoded-source counters at
zero. A connected fallback phase keeps the same H.264 media session, injects
a fatal Vulkan renderer error after 30 successful native-buffer
presentations, creates the compatible SurfaceTexture/OpenGL ES candidate, and
rebinds MediaCodec from AImageReader generation 5 to SurfaceTexture generation
6 through the explicit policy callback. The recorded run continued with 32
Vulkan-generation and 180 OpenGL ES-generation decoded frames, 30 Vulkan
imports/release fences, 179 external-OES images, and zero decoded-source
map/transfer/staging/upload calls. The
accepted shared Android/OHOS responsibility and lifecycle design is documented
in [`MOBILE.md`](MOBILE.md).

### Android user player demo

The manual final-test and user-facing player is under
[`examples/android_player/`](examples/android_player/). It is separate from
the automated NativeActivity harness and provides a standard Android UI with
an upper `SurfaceView`, current time/seek/duration controls, local document
selection, direct FFmpeg HTTP/HTTPS URL opening, play/pause/stop, and live
Vulkan, HDR, ZeroCopy, hardware-decode, and Vulkan validation-layer switches.
ZeroCopy and hardware decode occupy their own second option row so the full
controls remain touchable on a portrait phone.

The option matrix exercises software decode through Vulkan or OpenGL ES,
MediaCodec direct-Surface output, MediaCodec/AImageReader Vulkan ZeroCopy, and
MediaCodec/SurfaceTexture OpenGL ES ZeroCopy. Changing an option rebuilds the
affected native pipeline at the current position. Remote URLs go directly to
QtAVCore/FFmpeg networking; the example cross-builds OpenSSL, explicitly
verifies HTTPS peers and host names against an app-private PEM bundle assembled
from Android's public system trust store, and does not download through Java
or the Android networking stack. This is remote-file playback, not adaptive
streaming.

Hardware decode with direct-Surface presentation is the example's smooth
playback default; ZeroCopy is an explicit interop/application-processing
option. The core paces MediaCodec packets by DTS before decode, independently
of audio decode and device submission, and retains only a small bounded window
of decoded Surface outputs for the presentation worker. This avoids both
demux starvation and exhaustion of the codec output pool. The dedicated
Vulkan render thread reserves a
bounded slot before releasing each ZeroCopy output and waits only for the
private AImageReader ownership transfer, preventing producer bursts from
silently coalescing images without waiting for the render deadline. Its
diagnostics separate core queue/late drops, application render drops,
frames-in-flight, AHardwareBuffer cache reuse, and interop queue/acquire/import
progress. SurfaceTexture drops only an ambiguous zero-PTS first output;
AImageReader accepts zero as a valid timestamp.

Build and sign the arm64 debug APK without installing it:

```sh
modern/examples/android_player/build-android-player.sh
```

The output is `build/android-player/qtav-core-player.apk`. The connected-device
launcher has an explicit pre-install confirmation gate because modern Android
releases may require manual approval on the physical device:

```sh
modern/examples/android_player/run-connected-device.sh
QTAV_ANDROID_INSTALL_CONFIRMED=1 \
  modern/examples/android_player/run-connected-device.sh
```

The first command stops before `adb install`; run the second only after the
device is unlocked and ready for the manual prompt. See the example's
[`README.md`](examples/android_player/README.md) for codec scope, exact option
semantics, validation-layer requirements, and manual test guidance.

## API shape

```cpp
#include <qtav/player.h>
#include <qtav/swresample_audio_converter.h>

qtav::Player player;
auto converter = std::make_shared<qtav::SwresampleAudioConverter>();

player
    .setAudioFrameConverter(converter)
    .setAudioSink(audioSink)
    .onVideoFrame([](const qtav::VideoFrame& frame, int track) {
        // Inspect, filter, or forward the decoded frame.
    })
    .setVideoRenderer([](const qtav::VideoFrame& frame, void* surface) {
        // Upload/draw on the application's render thread.
    })
    .setRenderCallback([&](void*) {
        schedule_on_render_thread([&] { player.renderVideo(); });
    });

player.setMedia("movie.mkv");
player.setState(qtav::State::Playing);
```

To capture decoded audio as inspectable PCM, link `QtAV::AudioFile` and
`QtAV::AudioResample`:

```cpp
#include <qtav/swresample_audio_converter.h>
#include <qtav/wav_audio_sink.h>

auto sink = std::make_shared<qtav::WavAudioSink>(
    qtav::WavAudioSinkConfig {
        "capture.wav",
        48'000,
        2,
        qtav::SampleFormat::S16,
        "stereo",
    });

player
    .setAudioFrameConverter(
        std::make_shared<qtav::SwresampleAudioConverter>())
    .setAudioSink(sink);
```

### CoreAudio device sink

Link `QtAV::AudioCoreAudio` and `QtAV::AudioResample`, then bind the macOS
device sink:

```cpp
#include <qtav/coreaudio_audio_sink.h>
#include <qtav/swresample_audio_converter.h>

player
    .setAudioFrameConverter(
        std::make_shared<qtav::SwresampleAudioConverter>())
    .setAudioSink(
        std::make_shared<qtav::CoreAudioAudioSink>());
```

The default configuration follows the current default output device. A
backend-specific `CoreAudioDevice` may select an explicit `AudioDeviceID`.
The initial implementation negotiates the device's nominal sample rate and
one or two interleaved Float32 channels; `SwresampleAudioConverter` converts
decoded planar PCM, sample rate, or channel layout as needed. AudioQueue owns
the native playback schedule while queued data is copied into backend-owned
buffers. `clock()` maps AudioQueue sample time to the media timeline and
reports queued plus device latency.

### WASAPI device sink

Link `QtAV::AudioWASAPI` and `QtAV::AudioResample`, then bind the Windows
shared-mode device sink:

```cpp
#include <qtav/swresample_audio_converter.h>
#include <qtav/wasapi_audio_sink.h>

player
    .setAudioFrameConverter(
        std::make_shared<qtav::SwresampleAudioConverter>())
    .setAudioSink(
        std::make_shared<qtav::WasapiAudioSink>());
```

The default configuration resolves the current default multimedia render
endpoint whenever the sink opens. `WasapiEndpointId` can select an explicit
endpoint without passing an apartment-bound COM interface across threads.
The initial implementation negotiates one or two interleaved Float32 channels
at the endpoint mix rate and relies on `SwresampleAudioConverter` for decoded
format conversion. A dedicated multimedia-class thread owns COM, the
event-driven `IAudioClient`, copied PCM queue, pause/flush/drain lifecycle, and
the `IAudioClock` media-timeline anchor. A device-clock underrun temporarily
returns an invalid clock so `Player` can use its monotonic fallback until the
next buffer establishes a new anchor.

### AAudio device sink

Link `QtAV::AudioAAudio` and `QtAV::AudioResample` in an Android application
whose minimum API is 26 or newer:

```cpp
#include <qtav/aaudio_audio_sink.h>
#include <qtav/swresample_audio_converter.h>

player
    .setAudioFrameConverter(
        std::make_shared<qtav::SwresampleAudioConverter>())
    .setAudioSink(
        std::make_shared<qtav::AAudioAudioSink>());
```

The default configuration follows the current Android output route, requests
low-latency shared mode, and negotiates mono/stereo interleaved Float32 PCM.
Decoded PCM is copied on the audio-output worker into a fixed-capacity
single-producer/single-consumer queue; AAudio's real-time data callback only
performs lock-free queue reads, bounded memory copies, silence fill, and atomic
state updates. It never allocates, locks, sleeps, or opens/closes a stream.

`clock()` maps `AAudioStream_getTimestamp(CLOCK_MONOTONIC)` frame positions to
the media timestamp of the corresponding queued frame. Reported latency
combines callback/device pipeline frames with backend-queued PCM. Pause,
flush, drain, underrun re-anchoring, and stream teardown follow the generic
`AudioSink` lifecycle. If Android disconnects the stream during a default
route change, a separate backend worker closes and reopens it with the same
negotiated PCM format; successful recovery emits an underrun event so
`Player` buffers until a new device-clock anchor is available. A failed
restart emits `DeviceLost`.

`AAudioStreamInfo` exposes the current device identifier, burst/capacity,
xrun count, transparent route-change count, and disconnect-restart count for
diagnostics. The backend requires no OpenSL ES fallback at the current API 28
baseline.

### MediaCodec direct-surface hardware decode

Link `QtAV::HWMediaCodec` in an Android application. Create a new
`MediaCodecSurface` token for every application-published `ANativeWindow`
generation, then select the explicit FFmpeg wrapper decoder before playing:

```cpp
#include <qtav/mediacodec_hardware_decoder.h>

qtav::MediaCodecSurface surface(nativeWindow);
qtav::MediaCodecHardwareDecodeOptions options;
options.allowSoftwareFallback = false;

player
    .setHardwareDecodeConfig(
        qtav::mediaCodecHardwareDecodeConfig(surface, options))
    .setVideoFrameScheduler(
        [&surface](
            const qtav::VideoFrame& frame,
            int,
            std::int64_t monotonicNanoseconds) {
            auto output = qtav::mediaCodecFrame(frame, surface);
            return output
                && output.presentAt(monotonicNanoseconds);
        });
player.setMedia(path);
player.setState(qtav::State::Playing);
```

The helper selects FFmpeg's `*_mediacodec` wrapper by wrapper identity instead
of relying on the default software decoder lookup. `MediaCodecFrame` is a
move-only, single-decision token. `present()` releases immediately to the
configured surface, `presentAt()` uses a `CLOCK_MONOTONIC` nanosecond
timestamp, and `drop()` releases without display. Destroying an undecided last
frame copy lets FFmpeg drop it. Hardware-frame storage retains the decoder
context until all copied outputs are released, including across seek, stop,
media replacement, and asynchronous decoder reopen.

`setVideoFrameScheduler()` runs on the video-decode worker once a decoded frame
is inside its bounded lead window and supplies the target monotonic presentation
time. Returning `true` means the application consumed that frame and suppresses
the later `onVideoFrame()`/renderer path. This is the preferred direct-Surface
integration because the codec owns only a small output pool.

On surface loss, pause playback, publish an empty hardware-decode
configuration, and release the old token only after application-held frame
copies are no longer being used. Create a new `MediaCodecSurface` for the new
window, publish its configuration, and resume. The generation and native
window identity are copied into each output; `mediaCodecFrame()` rejects a
stale or foreign token. The direct-surface path does not expose a
shader-readable texture and does not imply Vulkan/OpenGL ES interop.

### MediaCodec Vulkan zero-CPU-copy texture interop

Link `QtAV::HWMediaCodec`, `QtAV::RenderVulkanAndroid`, and
`QtAV::InteropMediaCodecVulkan`. Before creating the application-owned
logical device, enable
`VK_ANDROID_external_memory_android_hardware_buffer`,
`VK_KHR_external_semaphore_fd`, `VK_EXT_queue_family_foreign`, and the
sampler-YCbCr-conversion feature. Construct the interop, give its private
surface to MediaCodec, and attach the same interop object to the Vulkan
renderer:

```cpp
#include <qtav/android_vulkan_video_renderer.h>
#include <qtav/mediacodec_vulkan_interop.h>

qtav::MediaCodecVulkanInteropConfig interopConfig;
interopConfig.maximumImages = 5;
interopConfig.androidHardwareBufferExternalMemoryEnabled = true;
interopConfig.externalSemaphoreFdEnabled = true;
interopConfig.samplerYcbcrConversionEnabled = true;
interopConfig.foreignQueueFamilyEnabled = true;

auto interop = std::make_shared<qtav::MediaCodecVulkanInterop>(
    borrowedVulkanDevice,
    interopConfig);
auto renderer = std::make_shared<qtav::AndroidVulkanVideoRenderer>(
    borrowedAndroidVulkanContext,
    qtav::VulkanOutputPreference::PreferHdr);
renderer->setHardwareFrameInterop(interop);
renderer->setWindow(nativeWindow);

player
    .setVideoRenderAPI(renderer)
    .setVideoFrameScheduler(
        [interop](
            const qtav::VideoFrame& frame,
            int,
            std::int64_t monotonicNanoseconds) {
            std::string detail;
            if (!interop->queueFrame(frame, detail)) {
                return false;
            }
            retain_and_schedule_render(
                frame,
                monotonicNanoseconds);
            return true;
        })
    .setHardwareDecodeConfig(
        qtav::mediaCodecHardwareDecodeConfig(interop->surface()))
    .setState(qtav::State::Playing);
```

The interop creates an `AIMAGE_FORMAT_PRIVATE` reader with
`AHARDWAREBUFFER_USAGE_GPU_SAMPLED_IMAGE`. Releasing a MediaCodec token to that
surface produces an asynchronous `AImage`; codec and image timestamps are
matched before its retained `AHardwareBuffer` is imported. Timestamp zero is
valid and is correlated normally; only negative image timestamps are rejected.
The config's optional `width` and `height` are only the reader's default
dimensions; when omitted, the interop uses `1x1`. MediaCodec overrides that
default with its decoded output size, and every import uses the actual
`AImage` dimensions and crop, so applications do not need a separate media
probe before creating the zero-copy path.
`queueFrame()` performs a bounded wait for the matching asynchronous image to
be acquired, serializing producer ownership transfer without waiting for the
playback deadline or Vulkan submission. Applications should reserve their own
bounded render slot before calling it, retain the exact `VideoFrame`, and
render at the scheduler-provided deadline.
Vulkan uses the
driver-reported format or external format and suggested YCbCr conversion. The
renderer also applies the `AImage` crop rectangle, so codec-aligned native
allocations may be larger than the visible decoded frame.

When an acquire sync fd is present it is imported into a temporary Vulkan
semaphore. Submission waits on that semaphore and transfers ownership from
the foreign queue family; completion signals an exportable semaphore whose
sync fd is returned with `AImage_deleteAsync()`. The imported object retains
the image and decoder output through the submission fence. Vulkan image,
memory, view, and YCbCr-conversion resources are cached by retained
`AHardwareBuffer` identity and retired by the AImageReader buffer-removed
callback; per-frame acquire/release semaphores remain synchronized with the
individual image. Call `flush()` before seek, decoder/media replacement, or
explicit stop to retire queued images and timestamp associations that have
not entered a submission.

`statistics()` exposes queue depth, import/fence counts, last native/Vulkan
format, decoded-source map/transfer/staging/upload counters, and persistent
hardware-buffer import/cache-hit/removal/high-water counters. Unsupported or
protected images fail explicitly. The implementation never calls
`AHardwareBuffer_lock*()` and has no implicit software-frame fallback; decoder
fallback and renderer/API fallback remain application policies.

### MediaCodec OpenGL ES zero-CPU-copy texture interop

Link `QtAV::HWMediaCodec`, `QtAV::RenderOpenGLAndroid`, and
`QtAV::InteropMediaCodecOpenGL`. Supply the NativeActivity's `JavaVM`, create
the interop before opening the renderer, bind MediaCodec to the interop's
private producer surface, and route renderer `RedrawRequested` events back to
the application's normal `renderVideo()` scheduling:

```cpp
#include <qtav/android_opengl_video_renderer.h>
#include <qtav/mediacodec_opengl_interop.h>

qtav::MediaCodecOpenGLInteropConfig interopConfig;
interopConfig.javaVM = nativeActivity->vm;

auto interop = std::make_shared<qtav::MediaCodecOpenGLInterop>(
    interopConfig);
auto renderer = std::make_shared<qtav::AndroidOpenGLVideoRenderer>(
    qtav::OpenGLOutputPreference::SdrOnly);
renderer->setHardwareFrameInterop(interop);
renderer->setWindow(nativeWindow);
qtav::VideoRenderConfig renderConfig;
renderConfig.surfaceSize = renderer->surfaceSize();
renderer->open(renderConfig);

player
    .setVideoRenderAPI(renderer)
    .setHardwareDecodeConfig(
        qtav::mediaCodecHardwareDecodeConfig(interop->surface()))
    .setState(qtav::State::Playing);
```

The interop constructs `android.graphics.SurfaceTexture` in detached mode and
retains its producer `ANativeWindow`. On the renderer thread it attaches the
consumer to the current OpenGL ES context, releases each MediaCodec output
exactly once, calls `ASurfaceTexture_updateTexImage()`, and accepts only the
timestamp-correlated surface generation. The renderer samples the returned
external texture with its SurfaceTexture transform, submits the draw, and
calls `glFlush()` before a later update advances the single current image.
A configured `width` and `height` only set the SurfaceTexture's default buffer
size; omitted values use `1x1`, and MediaCodec video output overrides that
default with the decoded size.
A short native retry worker only emits redraw requests; it never calls GL or
SurfaceTexture APIs.

Call `flush()` before seek, loop, decoder/media replacement, explicit stop, or
an interop-policy switch. Close the renderer while its context can still be
made current so the SurfaceTexture detaches cleanly. Same-context Android
window loss/recreation does not replace the decoder surface or map the native
frame.

The default contract accepts SDR 8-bit external-OES sampling. P010 or HDR
input is rejected unless
`MediaCodecOpenGLInteropConfig::hdrExternalOesSamplingEnabled` is explicitly
set after independent device/codec/driver color validation. There is no
implicit hardware-frame mapping or software upload. `statistics()` reports
codec decisions, latches, GL texture attach/detach/update counts, redraws,
pending depth, timestamps, and the zero CPU-map/transfer/staging/upload
counters.

### VideoToolbox hardware decode

Link `QtAV::HWVideoToolbox` and select it before opening media:

```cpp
#include <qtav/player.h>
#include <qtav/videotoolbox_hardware_decoder.h>

player
    .setHardwareDecodeConfig(
        qtav::videoToolboxHardwareDecodeConfig())
    .onVideoFrame([](const qtav::VideoFrame& frame, int) {
        if (!frame.hasHardwareFrame()) {
            return; // Explicit software fallback may be active.
        }
        const auto hardware = frame.hardwareFrame();
        CVPixelBufferRef pixelBuffer =
            qtav::videoToolboxPixelBuffer(hardware);
        // pixelBuffer is borrowed and remains valid while hardware is alive.
    });
```

The default backend configuration allows software fallback when the codec has
no VideoToolbox configuration, device creation fails, or hardware pixel-format
negotiation cannot complete. Set
`VideoToolboxHardwareDecodeConfig::allowSoftwareFallback` to `false` when the
hardware path is mandatory. A hardware `VideoFrame` reports its underlying
software pixel format but exposes no software plane pointers; use its
`HardwareFrame`, retain the returned `CVPixelBuffer` if needed independently,
or call `HardwareFrame::map()` for an explicit CPU copy.

To render supported VideoToolbox frames without that CPU copy, also link
`QtAV::InteropCVMetal` and attach its importer to `MetalVideoRenderer`:

```objective-c++
#include <qtav/cvmetal_frame_interop.h>
#include <qtav/metal_video_renderer.h>

auto interop = std::make_shared<qtav::CVMetalFrameInterop>(
    qtav::BorrowedMetalDevice(device));
auto renderer = std::make_shared<qtav::MetalVideoRenderer>(
    qtav::BorrowedMetalDevice(device),
    qtav::BorrowedMetalCommandQueue(commandQueue),
    currentTargetCallback,
    interop);

player
    .setHardwareDecodeConfig(
        qtav::videoToolboxHardwareDecodeConfig())
    .setVideoRenderAPI(renderer);
```

`CVMetalFrameInterop` accepts limited- and full-range bi-planar NV12 and P010
`CVPixelBuffer` values. It creates luma and chroma texture views through
`CVMetalTextureCache`; the renderer retains the imported frame until its Metal
command buffer completes. The associated `VideoFrame` carries range,
primaries, transfer, matrix, chroma location, HDR10 mastering-display, and
content-light metadata without exposing FFmpeg or CoreVideo types.

Multiple `VideoRenderAPI` instances can be associated with one player by using
an application-owned opaque key:

```cpp
player
    .setVideoRenderAPI(mainRenderer, mainSurfaceKey)
    .setVideoRenderAPI(previewRenderer, previewSurfaceKey)
    .setRenderCallback([&](void* key) {
        schedule_on_render_thread([&, key] { player.renderVideo(key); });
    });
```

Passing an empty `std::shared_ptr` removes the renderer for that key. The
existing `setVideoRenderer()` callback remains available and is used when no
`VideoRenderAPI` is registered for the requested key.

State/status callbacks are normally invoked from the playback worker. The
playback worker only demuxes selected packets; independent audio- and
video-decode workers prevent codec work or output backpressure on one stream
from starving packet delivery to the other. Decoded audio/video frame
notifications and `setRenderCallback()` run on a separate presentation
worker, so a slow application redraw path cannot stall demux, decode, or
device audio submission. The video presentation queue is bounded;
when the application falls behind, it preserves the imminent queued frame and
discards an incoming farther-future frame instead of accumulating unbounded
latency or repeatedly replacing the next presentable frame. `renderVideo()`
runs on the caller's thread, so an OpenGL, Vulkan, Metal, or D3D integration
can keep ownership of its native context and surface. It returns the rendered
timestamp in seconds, or a negative value when no frame is ready or a
real-time render attempt is temporarily declined; the render scheduler should
retry on a later redraw rather than block its native/UI thread. A/V startup
and playing seeks use a bounded video preroll before releasing device audio,
avoiding an audio-first clock sprint while the first video frames are still
being decoded.

`setVideoFrameScheduler()` is the earlier, opt-in hardware-presentation hook.
It runs on the video-decode worker inside the bounded video decode window and
receives a steady-clock target in nanoseconds. Returning `true` makes the
application responsible for that exact frame and suppresses normal video
delivery; returning `false` keeps the presentation-worker path. Use
`playbackStatistics()` to distinguish decoded/delivered frames, bounded queue
overflow drops, late drops, the video-queue high-water mark, and presentation
queue starvation count/maximum duration.

An accepted seek while playing changes the media status to `Buffering` and
holds `position()` at the requested position until output really resumes. A
clock-capable audio sink must publish a valid post-flush device-clock sample;
callback-only playback resumes when the first item from the new presentation
generation is delivered. Audio-device underruns return to `Buffering` and
freeze the fallback clock until the device clock re-anchors. Queue invalidation
in the public `seek()` call does not wait for the presentation or audio workers
to release their queue locks. HTTP(S) inputs additionally use a 15-second read
timeout and bounded FFmpeg reconnect defaults, all overridable through
`avformat.*`; this is not yet a general adaptive/live packet-buffer policy.

### CPU image-buffer renderer

Link `QtAV::RenderCPU`, include `<qtav/cpu_video_renderer.h>`, and provide a
writable buffer owned by the application:

```cpp
std::vector<std::uint8_t> pixels(width * height * 4);
auto renderer = std::make_shared<qtav::CpuVideoRenderer>();

renderer->setTarget({
    pixels.data(),
    width,
    height,
    width * 4,
    qtav::PixelFormat::BGRA,
});

qtav::VideoRenderConfig config;
config.surfaceSize = { width, height };
config.aspectRatio = qtav::VideoAspectRatioMode::Stretch;
renderer->open(config);
player.setVideoRenderAPI(renderer);
```

The buffer remains application-owned and must stay valid until rendering has
finished or another target is set. The initial reference backend accepts a
positive stride, a full-surface borrowed target, `Stretch`, and `Rotate0`.
Packed RGB24, BGR24, RGBA, BGRA, ARGB, and Gray8 are supported as destination
formats. `setTarget()` and `render()` are synchronized internally, but the
application remains responsible for coordinating its own reads of the pixel
memory with rendering.

### Vulkan renderer and Android surface adapter

Link `QtAV::RenderVulkan` and construct the engine with an
application-selected physical device, logical device, graphics queue, and
queue-family index. The application supplies the current image/view, extent,
format, `VkColorSpaceKHR`, wait/signal semaphores, final layout, and surface
generation through `VulkanCurrentTargetCallback`. These Vulkan objects remain
borrowed and must survive the submission fence. Supported targets are
RGBA8/BGRA8 with sRGB/BT.709 nonlinear output, 10-bit packed RGB with
HDR10/PQ or HDR10/HLG output, and RGBA16F with extended-sRGB-linear or
BT.2020-linear output. PQ/HLG sources preserve BT.2020 and native HDR
luminance on HDR targets; SDR targets retain the documented BT.709/sRGB tone
mapping. Linear HDR targets use `1.0` as the renderer's 100-nit reference
white and preserve brighter values above `1.0`.

The renderer can also accept an optional `VulkanHardwareFrameInterop`.
`prepareFrame()` starts or polls asynchronous producer release before a
render target is acquired, while `importFrame()` returns a retained sampled
image/view, immutable YCbCr sampler, acquire/release semaphores, native image
layouts, foreign queue-family identity, and normalized source crop. The
renderer retains that `VulkanTextureFrame` until its submission fence
completes. Platform interop remains a separate target; `QtAV::RenderVulkan`
does not depend on MediaCodec or Android.

On Android, `QtAV::RenderVulkanAndroid` implements that target protocol and
the `VideoRenderAPI` facade together. The application creates a Vulkan
instance/device/graphics-present queue, then publishes the current
`ANativeWindow`:

```cpp
auto renderer =
    std::make_shared<qtav::AndroidVulkanVideoRenderer>(
        qtav::BorrowedAndroidVulkanContext {
            instance,
            { physicalDevice, device, queue, queueFamilyIndex },
            hdrMetadataExtensionWasEnabled,
        },
        qtav::VulkanOutputPreference::PreferHdr);
renderer->setWindow(nativeWindow);

qtav::VideoRenderConfig config;
config.surfaceSize = renderer->surfaceSize();
renderer->open(config);
player.setVideoRenderAPI(renderer);
```

The adapter acquires its own window reference and owns the associated
`VkSurfaceKHR`, swapchain, image views, and per-frame acquire/present
semaphores. Passing `nullptr` to `setWindow()` invalidates that generation.
Publishing a new window while the renderer remains open rebuilds the
surface/swapchain and resumes presentation without reopening media. Publishing
the same window again after its buffer geometry changes refreshes the
swapchain extent and Fit/Fill viewport in place. The Vulkan
instance, device, and queue remain application-owned and must outlive the
renderer. `PreferHdr` chooses HDR10/PQ first, then native HLG or
extended-linear output, and falls back to SDR; `RequireHdr` fails explicitly
when no implemented HDR pair is exposed, while `SdrOnly` preserves an
application-selected SDR policy. The application must enable
`VK_EXT_swapchain_colorspace` while creating its instance to expose extended
surface color spaces. If it also enables `VK_EXT_hdr_metadata` on the logical
device and reports that fact in `BorrowedAndroidVulkanContext`, the adapter
submits frame-derived mastering-display and content-light metadata before
presentation. `surfaceFormat()` and `hdrOutputActive()` expose the selected
contract for diagnostics and tests.

### OpenGL ES renderer and Android EGL adapter

Link `QtAV::RenderOpenGLAndroid` for the Android OpenGL ES fallback. The
adapter owns its EGL display, OpenGL ES 3.x context, window surface, and swap
operations while retaining only the active `ANativeWindow` generation:

```cpp
#include <qtav/android_opengl_video_renderer.h>

auto renderer =
    std::make_shared<qtav::AndroidOpenGLVideoRenderer>(
        qtav::OpenGLOutputPreference::PreferHdr);
renderer->setWindow(nativeWindow);

qtav::VideoRenderConfig config;
config.surfaceSize = renderer->surfaceSize();
config.aspectRatio = qtav::VideoAspectRatioMode::Fit;
renderer->open(config);
player.setVideoRenderAPI(renderer);

if (renderer->hdrOutputActive()) {
    recordHdrSurface(
        renderer->outputColorSpace(),
        renderer->colorComponentBits());
}
```

`PreferHdr` tries an exact RGB10_A2 BT.2020/PQ EGLConfig and colorspace first,
then BT.2020/HLG when the display exposes it, and finally RGBA8/sRGB.
`RequireHdr` rejects an SDR-only display, while `SdrOnly` always selects the
RGBA8 path and preserves deterministic HDR-to-SDR tone mapping.
`outputColorSpace()`, `hdrOutputActive()`, and `colorComponentBits()` expose
the selected contract. The adapter verifies the EGL surface color space and
the `ANativeWindow` dataspace used by Android composition.

Passing `nullptr` to `setWindow()` invalidates the active EGL surface; a later
window recreates it and reconfigures the still-open renderer generation.
Publishing the same window after an in-place buffer-geometry change recreates
the EGL surface and refreshes the renderer target size.
`render()` makes the owned context current for the call, presents with
`eglSwapBuffers()`, and then releases it from the calling thread. The adapter
does not create or select Vulkan and is not itself the Vulkan-to-OpenGL ES
policy layer.

`QtAV::RenderOpenGL` is the reusable engine for applications or future
platform adapters that already own an OpenGL ES 3.x context. Its
`OpenGLCurrentTargetCallback` returns the current framebuffer number, size,
generation, and `OpenGLOutputColorSpace`; framebuffer zero is the default
framebuffer. The context must be current for `open()`, `render()`, and
`close()`. The engine accepts
YUV420/422/444, NV12/NV21, little-endian P010, RGB/BGR,
RGBA/BGRA/ARGB, and Gray8 software frames. It applies structured
range/matrix/transfer/primaries metadata, Fit/Fill/Stretch, custom viewports,
and all right-angle rotations. The current baseline writes SDR sRGB-coded
output with PQ/HLG tone mapping for `SdrSrgb`, or preserves HDR luminance and
converts primaries/transfer explicitly for BT.2020 `HDR10PQ` and `HDR10HLG`
targets. Hardware-frame interop remains separate work.

### Mobile renderer selector

Link `QtAV::RenderMobile` and provide factories that create a fully prepared
platform adapter for the current native-window generation. The Android
application continues to own its activity, window reference, and borrowed
Vulkan device:

```cpp
#include <qtav/mobile_video_renderer.h>

qtav::MobileRendererSelectorConfig selectorConfig;
selectorConfig.vulkan = [&] {
    return createPreparedAndroidVulkanRenderer(currentWindow);
};
selectorConfig.openGLES = [&] {
    return createPreparedAndroidOpenGLRenderer(currentWindow);
};

auto selector =
    std::make_shared<qtav::MobileVideoRendererSelector>(
        std::move(selectorConfig));
selector->setSelectionCallback([](const auto& event) {
    recordRendererSelection(
        qtav::mobileRenderAPIName(event.selectedAPI),
        event.detail);
});
selector->setHardwareFrameFallbackCallback(
    [&](const qtav::MobileHardwareFrameFallbackEvent& event) {
        if (event.sourceDevice
                == qtav::HardwareDeviceType::MediaCodec
            && preparedOpenGLInterop) {
            player.setHardwareDecodeConfig(
                qtav::mediaCodecHardwareDecodeConfig(
                    preparedOpenGLInterop->surface()));
            return qtav::MobileHardwareFrameFallbackDecision {
                qtav::MobileHardwareFrameFallbackRoute::
                    OpenGLESInterop,
                "MediaCodec rebound to the SurfaceTexture producer",
            };
        }
        return qtav::MobileHardwareFrameFallbackDecision {
            qtav::MobileHardwareFrameFallbackRoute::NoVideo,
            "No compatible native interop was available",
        };
    });

selector->open(config);
player.setVideoRenderAPI(selector);
```

Each successful factory result contains an already window-bound
`VideoRenderAPI`; an unavailable result carries its diagnostic reason. The
selector prefers Vulkan on every new `open()` session. A candidate
`SurfaceLost` event triggers at most `maximumRecoveryAttempts` complete
same-API recreations. A candidate `Error` is fatal: Vulkan switches one-way to
OpenGL ES, while fatal OpenGL ES failure reports video unavailable. The
current frame is retained and retried after successful recovery or fallback,
and the selector object remains attached to `Player`, so media is not reopened.
The exception is a cross-API hardware frame: it belongs to the retired native
surface and is discarded rather than retried. The hardware fallback callback
runs synchronously inside `renderVideo()` and must choose
`OpenGLESInterop`, `DirectSurface`, `SoftwareDecode`, or `NoVideo`; returning
`None` or omitting the callback is an explicit unavailable result. It may
publish a new `HardwareDecodeConfig`, but must not destroy the selector.
OpenGL ES interop also requires the prepared candidate to advertise the
source hardware device. Software decode keeps the OpenGL ES renderer for later
software frames, while direct-surface and no-video routes retire it.

A renderer may return `false` without emitting `SurfaceLost` or `Error` while
asynchronous native interop is pending. The selector treats that as retryable
and does not change graphics APIs. Late frames from the retired native surface
and hardware frames arriving while a software-decode transition completes are
discarded without calling `HardwareFrame::map()`.

Call `suspendSurface()` before releasing a platform window, publish the new
window to the factory's application state, and call `recreateSurface()`.
Selection notifications distinguish initial selection, recovery, fallback,
and no-renderer results. `selectedAPI()`, `usingFallback()`,
`presentationAvailable()`, `hardwareFrameFallbackRoute()`, and `lastError()`
provide synchronous diagnostics.

### D3D11 composition output

For ordinary Windows presentation, link `QtAV::OutputD3D11` and provide the
native composition control's swap-chain binding operation. The output owns
the D3D11 device, HDR-aware flip-model swap chain, target, display tracking,
render scheduling thread, `renderVideo()`, `Present()`, D3D11VA
configuration, Video Processor interop, resize, and teardown:

```cpp
#include <qtav/d3d11_video_output.h>
#include <qtav/player.h>

qtav::Player player;
qtav::D3D11VideoOutput output;
qtav::D3D11CompositionSurface surface;
surface.size = { width, height };
surface.compositionScaleX = compositionScaleX;
surface.compositionScaleY = compositionScaleY;
surface.window = hwnd;
surface.bindSwapChain = [panelNative](IDXGISwapChain1* swapChain) {
    return panelNative->SetSwapChain(swapChain);
};

if (!output.open(std::move(surface)) || !output.attach(player)) {
    throw std::runtime_error(output.lastError());
}

player.setMedia(url).setState(qtav::State::Playing);

// On native surface size/scale changes:
output.resize({ newWidth, newHeight }, newScaleX, newScaleY);

// Before destroying either the Player or native surface:
output.detach();
output.close();
```

The default `D3D11OutputPreference::PreferHdr` creates an FP16 scRGB
composition swap chain when `surface.window` or `surface.currentMonitor` can
identify the active display. Before every presentation, the renderer resolves
that monitor to `IDXGIOutput6`, observes the current Windows HDR setting,
system SDR reference white, and panel luminance, and configures
`DXGI_COLOR_SPACE_RGB_FULL_G10_NONE_P709`. PQ/HLG material therefore remains
HDR through the final DWM layer while Advanced Color is active; on an SDR
display or with Windows HDR disabled, the same path tone maps into the SDR
range. Moving the window between displays takes effect on the next frame.

`D3D11OutputPreference::RequireHdr` refuses to present while the current
display is not in Advanced Color HDR mode.
`D3D11OutputPreference::SdrOnly` creates a BGRA8 SDR swap chain. `PreferHdr`
also falls back to BGRA8 when no HWND or monitor provider is supplied, keeping
non-windowed composition hosts usable without claiming HDR. `colorInfo()`
reports the selected format/color space, active display, system SDR white,
luminance, and whether the presented layer is currently HDR.

`attach()` exclusively owns the player's default render slot and render
callback until `detach()`. The player and native surface must therefore
outlive the attachment. Output event and frame-presented callbacks may run on
the output render thread and must not destroy or detach the output from inside
the callback. Toolkit-specific UI changes should be posted to that toolkit's
dispatcher.

The current WinUI 3 example uses only its HWND, this surface binding, and
`attach()`/`resize()`; it no longer implements a graphics device, swap chain,
render thread, HDR policy, or presentation loop in application code.

The composition output caps DXGI frame latency at one, uses non-blocking
`Present()` and a bounded waitable-object wait on its private render thread,
and retries when the compositor is busy. It never waits for presentation
capacity on the UI thread. `takeStatistics()` returns and resets render
requests/passes, presented frames, coalesced requests, busy presents, skipped
renders, long gaps, render/present maxima, and the renderer's color, interop,
buffer-update, and draw-stage maxima.

### D3D11 renderer (advanced external-context path)

On Windows, link `QtAV::RenderD3D11`, include
`<qtav/d3d11_video_renderer.h>`, create shared access to the selected device
and its immediate context, and pass a callback that returns the current
application-owned render target:

```cpp
auto deviceAccess = qtav::D3D11DeviceAccess::create(
    qtav::BorrowedD3D11Device(device),
    qtav::BorrowedD3D11DeviceContext(immediateContext));

auto renderer = std::make_shared<qtav::D3D11VideoRenderer>(
    deviceAccess,
    [&] {
        return qtav::D3D11RenderTarget {
            currentRenderTargetView,
            currentSwapChain3,
        };
    });

qtav::VideoRenderConfig config;
config.surfaceSize = { width, height };
renderer->open(config);
player.setVideoRenderAPI(renderer);
```

`D3D11DeviceAccess::create()` rejects null, foreign-device, and deferred
contexts, retains the selected device and verified immediate context, and
provides the recursive lock shared by the renderer and D3D11VA/interop
backends. `contextGuard()` waits for ownership; `tryContextGuard()` is its
non-blocking real-time form and returns a false guard when the context is
busy. The older renderer constructor taking the two borrowed wrappers remains
a convenience path and creates the same retained access internally.
The callback and returned `ID3D11RenderTargetView`/`IDXGISwapChain3` remain
application-owned. The swap chain is optional for offscreen rendering, but it
is required for native Advanced Color presentation. Composition swap chains
also return the hosting window's current `HMONITOR` in
`D3D11RenderTarget::monitor` because `GetContainingOutput()` is unsupported
for that swap-chain kind. The callback runs synchronously inside `render()`
and is queried for every frame, so swap-chain resize, display moves, or other
surface recreation can replace the current values before calling
`configure()` with the new size.

The renderer uses `tryContextGuard()` and a non-blocking render lock while
issuing immediate-context calls. If either is busy, `render()` returns false
without emitting a backend error; `Player::renderVideo()` therefore returns a
negative value and the scheduler retries. Applications using that immediate
context from another thread must acquire `contextGuard()` (or provide
equivalent external serialization):

```cpp
{
    auto guard = deviceAccess->contextGuard();
    immediateContext->CopyResource(destination, source);
}
```

The renderer does not preserve D3D11 pipeline state; applications sharing that
context must restore their state after `renderVideo()`.

The software path uploads YUV420P, YUV422P, YUV444P, NV12, NV21, P010,
RGB24, BGR24, RGBA, BGRA, ARGB, and Gray8 frames. It renders to single-sample
BGRA8/RGBA8 UNORM, `R16G16B16A16_FLOAT`, or
`R10G10B10A2_UNORM` 2D targets. The shader applies limited/full-range
BT.601/BT.709/BT.2020 YUV conversion, PQ or HLG EOTF, linear-light
BT.2020/Display-P3 to BT.709 conversion, display-aware luminance mapping, and
the output transfer. It also supports custom viewports, Fit/Fill/Stretch, all
right-angle rotations, resize, surface recreation, and surface/device-loss
events.

For general-purpose Windows presentation, create a flip-model
`R16G16B16A16_FLOAT` swap chain. The renderer sets its color space to
`DXGI_COLOR_SPACE_RGB_FULL_G10_NONE_P709`; scRGB `1.0` represents 80 nits and
HDR highlights may exceed `1.0`. An `R10G10B10A2_UNORM` flip-model swap chain
is also accepted: while Windows HDR is active on the current display, the
renderer selects `DXGI_COLOR_SPACE_RGB_FULL_G2084_NONE_P2020` and writes
BT.2020/PQ HDR10 values; while HDR is inactive, it switches the same target to
SDR G22/P709 instead. Eight-bit targets remain SDR.

When a swap chain is supplied, every `render()` obtains its containing output
and `IDXGIOutput6::GetDesc1()` data. This makes Windows HDR setting changes and
moving the window between displays take effect on the next frame. The renderer
also reads `DISPLAYCONFIG_SDR_WHITE_LEVEL`; SDR material composed into an HDR
surface is scaled to the user's current Windows SDR reference-white setting.
HDR material is mapped against the current display maximum luminance. On an
SDR display, FP16 output is mapped into `[0, 1]` before DWM composition rather
than relying on numeric clipping. `advancedColorInfo()` exposes the most
recent monitor, active display/swap-chain color spaces, bit depth, SDR white,
whether that white value came from the system query, and luminance limits for
diagnostics. Explicit swap-chain HDR metadata is not set; the renderer maps
pixels into the reported display range.

`QtAV::RenderD3D11` also defines the decoder-independent
`D3D11HardwareFrameInterop` and `D3D11TextureFrame` contracts implemented by
`QtAV::InteropD3D11`. The interop binds to the same retained
`D3D11DeviceAccess`, performs no CPU map during `importFrame()`, and returns
borrowed texture/SRV pointers whose COM resources remain valid while the
returned texture-frame object is alive. Bind it with
`setHardwareFrameInterop()`; the renderer advertises D3D11 hardware-frame
support only when both objects use the same `D3D11DeviceAccess`. Imported
textures report their DXGI format and color space so the final
viewport/aspect/rotation/color pass can preserve or convert SDR, linear scRGB,
and RGB10/PQ data correctly.

The imported wrapper is retained only through submission of its Video
Processor conversion and final draw. Decode, interop, and rendering use the
same serialized immediate context, so a later decode submission that reuses a
surface is ordered after the preceding GPU reads. D3D11 keeps resources
referenced by queued commands alive; the renderer deliberately does not add a
per-frame event query or CPU wait, which would retain decoder surfaces and
introduce driver stalls after seek.

Hardware-frame import and decoder fallback are independent policies. The
renderer does not map a hardware frame by default. Applications may explicitly
enable `setAllowSoftwareMappingFallback(true)` to call
`HardwareFrame::map(Read)` and use the existing software upload path when
interop is unavailable or import fails. A successful mapped fallback emits an
error/detail event containing `software-mapping fallback` so the CPU transfer
is observable; a disabled or failed mapping makes `render()` fail.

### D3D11VA hardware decode

Link `QtAV::HWD3D11VA` and pass the same retained device access used by the
renderer before opening media:

```cpp
#include <qtav/d3d11va_hardware_decoder.h>

auto deviceAccess = qtav::D3D11DeviceAccess::create(
    qtav::BorrowedD3D11Device(device),
    qtav::BorrowedD3D11DeviceContext(immediateContext));

player
    .setHardwareDecodeConfig(
        qtav::d3d11vaHardwareDecodeConfig(deviceAccess))
    .onVideoFrame([](const qtav::VideoFrame& frame, int) {
        const auto native =
            qtav::d3d11vaFrame(frame.hardwareFrame());
        if (!native) {
            return; // Explicit software fallback may be active.
        }
        ID3D11Texture2D* texture = native.texture();
        const UINT slice = native.arraySlice();
        // Both remain valid while native or its source frame is alive.
    });
```

The helper creates and initializes FFmpeg's D3D11VA device context on the
selected device, retains its verified immediate context, and installs lock
callbacks backed by the same recursive guard used by the renderer. Its default
pool allowance is four extra hardware frames and is bounded to 64. If selected
device initialization, codec capability, pixel-format negotiation, or decoder
open fails, the default policy reports `decoder.hardware.fallback` and
continues in software; disabling fallback makes the failure terminal.

Decoded D3D11 frames expose NV12 or P010 `ID3D11Texture2D` array slices through
`D3D11VAFrame`. A copied view retains the underlying FFmpeg frame, pool,
hardware device, COM resources, and lock state. `HardwareFrame::map()` remains
the explicit CPU-copy path and can be used after seek, media replacement,
stop, or player shutdown while the copied frame is alive. Feeding decoder
slices to the renderer without a CPU copy uses the separate
`QtAV::InteropD3D11` target:

```cpp
#include <qtav/d3d11_frame_interop.h>

auto interop = std::make_shared<qtav::D3D11FrameInterop>(
    deviceAccess);
renderer->setHardwareFrameInterop(interop);
player
    .setHardwareDecodeConfig(
        qtav::d3d11vaHardwareDecodeConfig(deviceAccess))
    .setVideoRenderAPI(renderer);
```

`D3D11FrameInterop` validates the decoder texture, array slice, source format,
device health, and exact COM device identity before entering the shared
context guard. It caches the Video Processor enumerator/processor for the
current NV12/P010 size and keeps a bounded pool of up to three
shader-readable output textures and their output views. Sequential imports
reuse a compatible free output; overlapping imported-frame lifetimes receive
independent pooled or transient outputs, so the retained-frame contract is
preserved without allocating a full-resolution GPU texture every frame. Each
import retains its source and input view and submits `VideoProcessorBlt()` on
the selected immediate/video context. The renderer passes structured frame
color metadata to the interop. SDR input uses BGRA8 G22/P709 and retains the
legacy BT.601/BT.709 fallback. PQ/BT.2020 input prefers RGB10/PQ P2020 and
falls back to FP16 linear scRGB when the driver reports that conversion;
HLG/BT.2020 prefers FP16 scRGB and can fall back to RGB10/PQ. The final
renderer then performs display-specific tone and gamut mapping without a CPU
map.

### Metal renderer

`QtAV::RenderMetal` is an Objective-C++ API. Include
`<qtav/metal_video_renderer.h>` from a `.mm` file and pass strongly typed
borrowed resources:

```objective-c++
auto renderer = std::make_shared<qtav::MetalVideoRenderer>(
    qtav::BorrowedMetalDevice(device),
    qtav::BorrowedMetalCommandQueue(commandQueue),
    [&] {
        qtav::MetalRenderTarget target;
        target.drawable = currentDrawable;
        return target;
    });

qtav::VideoRenderConfig config;
config.surfaceSize = {
    static_cast<int>(currentDrawable.texture.width),
    static_cast<int>(currentDrawable.texture.height),
};
renderer->open(config);
player.setVideoRenderAPI(renderer);
```

The application owns the device, queue, callback, texture, and drawable.
Device and queue must outlive the renderer; objects returned by the target
callback must remain valid for that `render()` call. The renderer owns its
shader and pipeline resources, copies software frame planes into a Metal
buffer or binds textures supplied by an optional `MetalHardwareFrameInterop`,
commits the command buffer, and presents a returned drawable. Set
`MetalRenderTarget::waitUntilCompleted` only for deterministic offscreen
readback or diagnostics.

The SDR software path supports YUV420P, YUV422P, YUV444P, NV12, NV21,
little-endian P010, RGB24, BGR24, RGBA, BGRA, ARGB, and Gray8. It supports
custom viewports, Fit/Fill/Stretch, all right-angle rotations, resize, and
BGRA/RGBA 8-bit or RGBA16-float render targets. With
`QtAV::InteropCVMetal`, the hardware path supports limited- and full-range
NV12 and P010 VideoToolbox frames without a CPU copy.

`MetalRenderTarget::outputColorSpace` defaults to `SDR`. In this mode the
renderer preserves ordinary SDR presentation and maps PQ/HLG input into the
target range. For complete Apple EDR presentation, return the application-owned
`CAMetalLayer` and its active display instead of obtaining the drawable first:

```objective-c++
qtav::MetalRenderTarget target;
target.layer = metalLayer;
#if TARGET_OS_OSX
target.display = view.window.screen;
#else
target.display = view.window.windowScene.screen;
#endif
target.outputColorSpace =
    qtav::MetalOutputColorSpace::ExtendedLinearBT2020;
target.edrToneMapping = qtav::MetalEDRToneMapping::System;
```

The renderer configures that layer before calling `nextDrawable`: device and
drawable size, `MTLPixelFormatRGBA16Float`,
`kCGColorSpaceExtendedLinearITUR_2020`,
`wantsExtendedDynamicRangeContent = YES`, and frame-derived HDR10 or HLG
`CAEDRMetadata`. PQ/HLG input is decoded into display-referred linear light
where `1.0` equals `MetalRenderTarget::referenceWhiteNits`; BT.2020 input
remains in BT.2020 primaries instead of being reduced to BT.709. This preserves
the source HDR luminance and gamut representation through the render target;
it is a color-managed linear representation, not bit-identical compressed
HDR10 data.

`MetalEDRToneMapping::System` lets Core Animation adapt the layer using
`CAEDRMetadata`. `DisplayAdaptive` instead samples
`NSScreen.maximumExtendedDynamicRangeColorComponentValue` on macOS or
`UIScreen.currentEDRHeadroom` on iOS for every render call, clears layer EDR
metadata to avoid double tone mapping, and compresses HDR into the live
display headroom. It preserves values through SDR white when headroom is
greater than `1.0`; at `1.0` it uses an HDR-to-SDR shoulder instead of clipping
all highlights. `None` preserves unmodified extended-linear values for
reference displays and offscreen processing.
`MetalRenderTarget::currentEDRHeadroom` can provide a positive explicit
override for deterministic tests; zero uses the live display query.
`metalCurrentEDRHeadroom()` and `metalPotentialEDRHeadroom()` expose the same
screen queries to Objective-C++ applications.

`ExtendedLinearSRGB` remains available for applications that deliberately use
BT.709/sRGB primaries, but it does not preserve the complete BT.2020 gamut.
For onscreen EDR, returning `layer` is required so metadata is installed before
the drawable is acquired. Deterministic GPU readback verifies HDR pixels above
`1.0`, BT.2020 preservation, and adaptive 2x/4x headroom. A separate macOS
display test presents through a real EDR-capable screen and reports a CTest
skip when no screen has live EDR headroom.

`VideoFrame::colorSpaceInfo()` returns structured range, primaries, transfer,
matrix, and chroma location. `masteringDisplayMetadata()` and
`contentLightMetadata()` return copied HDR10 static metadata, so their values
remain valid with any copied frame after decoder progress or player shutdown.
The existing `colorSpace()` string remains available for diagnostics.

## Backend contracts

`VideoRenderAPI` defines surface size, viewport, supported aspect modes,
rotation, native resource ownership, capability reporting, redraw/surface
events, and explicit open/configure/render/close lifecycle. Generic headers
contain no graphics API types; typed native constructors and handle helpers
belong in a backend's public header.

`AudioSink` separates the decoded input format passed to `open()` from the
negotiated device format returned by it. It defines close, pause, flush, write,
drain, event, latency, and device-clock operations. `write()` consumes a
synchronous non-owning `AudioBufferView` in the negotiated device format.
`drain()` waits until accepted buffers have been presented and is called after
each completed playback segment, including a loop boundary, and before
`close()` at final natural end; its default implementation is a no-op for
synchronous or non-queuing sinks. The `audioBufferView()` helper creates a
view when no conversion is required. `Player` opens an injected
`AudioFrameConverter` when the negotiated format differs, resets it on
flush/seek, drains the converter and sink at each completed segment, and closes
them at final natural end. Without an injected converter, a different device
format reports
`audio.sink.format` while `onAudioFrame()` continues normally.

`QtAV::AudioResample` implements `SwresampleAudioConverter`. It converts sample
format, sample rate, and channel layout to interleaved U8, S16, S32, float, or
double PCM. Conversion result memory belongs to the converter until its next
operation; `Player` writes it to the sink synchronously from its audio-output
worker. Converter and sink calls are serialized without the player mutex held;
control-driven lifecycle calls run on the playback worker, while conversion
and ordinary writes run on the audio-output worker.

`QtAV::AudioFile` implements `WavAudioSink`. It negotiates interleaved U8, S16,
S32, float, or double PCM, writes a little-endian RIFF/WAVE stream, and
finalizes its size fields on close. Its configured zero sample-rate or channel
count inherits the decoded value. It has no device clock and does not pace
playback; applications normally inject `QtAV::AudioResample` when decoded PCM
is planar or otherwise differs from the requested file format. Seek flushes
the stream but preserves already captured samples, so subsequent playback is
appended to the same diagnostic timeline.

`QtAV::AudioCoreAudio` implements `CoreAudioAudioSink` on macOS. Its
backend-specific public header exposes the strong, non-owning
`CoreAudioDevice` wrapper, while no Apple SDK type reaches the core headers.
The sink follows the default output device unless an explicit device is
selected, negotiates interleaved Float32 PCM at the device's nominal rate,
copies accepted buffers into a bounded AudioQueue pool, implements
pause/flush/drain, and reports a media-timeline device clock plus hardware and
queued latency.

`QtAV::AudioWASAPI` implements `WasapiAudioSink` on Windows. Its
backend-specific public header uses an owning `WasapiEndpointId` value rather
than exposing `IMMDevice` through the generic contract. The sink follows the
default multimedia render endpoint unless an explicit endpoint is selected,
negotiates shared-mode interleaved Float32 PCM at the engine mix rate, copies
accepted buffers into a bounded backend queue, and implements event-driven
pause/flush/drain. COM and native WASAPI interfaces stay on a dedicated
multimedia-class thread; callers can query a cached media-timeline
`IAudioClock` position and combined engine/stream latency from any thread.

`QtAV::AudioAAudio` implements `AAudioAudioSink` on Android API 26 or newer.
Its backend-specific header represents an optional device selection as an
owning integer `AAudioDeviceId`, so no AAudio declaration enters the generic
contract or core headers. It negotiates interleaved Float32 mono/stereo PCM,
copies accepted buffers into a preallocated SPSC ring, and feeds the native
data callback without allocation or locks. AAudio presentation timestamps
supply the media-timeline clock; latency includes queued PCM and frames already
submitted to the native stream. Pause, flush, drain, xrun detection, transparent
route observation, and disconnect-triggered stream reconstruction remain
inside the Android target.

`QtAV::PlatformWindows` owns the Windows-only `D3D11DeviceAccess` helper and
strong non-owning wrappers for `ID3D11Device` and `ID3D11DeviceContext`; no
Windows SDK type reaches the core headers. `QtAV::RenderD3D11` retains that
shared access, compiles shaders for its device, uploads software-frame planes
into per-frame shader resources, and obtains the current borrowed render
target immediately before drawing.

`QtAV::HWD3D11VA` also retains the shared device access while its FFmpeg
hardware-device token, decoder pools, or copied frames remain alive. The
backend-specific `D3D11VAFrame` validates the decoded texture format, array
slice, dimensions, and source device before exposing borrowed native pointers.
Its decoder and mapping calls use FFmpeg lock callbacks connected to the same
recursive context lock as rendering.

`AudioSinkClock` fields are measured in milliseconds on the media timeline.
`positionMilliseconds` is the sample position currently presented by the
device, while `latencyMilliseconds` is informational and must not already be
folded into that position. A sink that advertises a device clock becomes the
playback master whenever `clock()` returns a valid value; otherwise the player
falls back to its monotonic software clock. Ordinary sink writes and their
clock samples run on the dedicated audio-output worker. When a playing seek or
underrun recovery is waiting for the first valid post-flush timestamp, that
same worker polls the sink until it becomes valid; presentation never calls the
backend. A blocking backend write is therefore never allowed to stall video
scheduling. Repeated device samples are monotonically extrapolated, bounded by
submitted audio, and published as a generation-checked cache, so
`Player::position()` never waits for a sink write or calls into a platform
backend. Sink lifecycle and segment-end `drain()` run on the playback worker,
serialized with writes and without the player mutex held; `drain()` may block
until the backend queue is presented. Shutdown synchronizes the quitting
predicate with each worker condition-variable mutex before notification so
worker joins cannot lose the wake-up. On initial playback, a clock-capable sink
whose first sample is invalid falls back after the first delivered buffer.

`HardwareDecodeConfig` is copied by `Player` and applied the next time the
video decoder opens. Changing it while media is loaded interrupts the current
open/read operation and asynchronously reopens the media. The generic core
maps `HardwareDeviceType` to FFmpeg's internal hardware-device selection,
checks the codec's advertised hardware pixel format, and keeps the platform
types private. A backend can additionally request a registered FFmpeg decoder
by wrapper identity and attach an application surface generation; both values
participate in asynchronous configuration replacement. An unknown device type
selects the ordinary software path.

An in-tree hardware backend may attach a `HardwareDecodeDevice` to the config.
The public token reports only its generic device type and opaque native
identity; it is a cheap reference-counted value and keeps the backend-created
FFmpeg device context alive. Its FFmpeg bridge is a private, uninstalled core
header. `Player` takes its own device-context reference before decoder open and
rejects a token whose device type differs from
`HardwareDecodeConfig::deviceType`, using the configured software-fallback
policy. Replacing a supplied token while media is loaded also reopens the
decoder. If no token is supplied, core retains its existing FFmpeg-created
device behavior.

`HardwareFrame` is a cheap reference-counted view over backend-owned frame
storage. A native handle is an opaque integer tagged by its role and remains
valid while the frame is alive. CPU mapping returns a reference-counted
`HardwareFrameMapping`, whose destructor ends the mapping. Backend-specific
headers are responsible for converting opaque handles to strong D3D, Metal,
VAAPI, or Android types. `HardwareFrameInterop` describes source/target support
and imports frames without committing to a runtime plugin ABI.

These are compile-time C++ contracts for targets built with a compatible
toolchain. They are not a stable cross-compiler dynamic-plugin boundary.

The contract implementation is covered by mock backend tests. Deterministic
audio-clock tests cover pause, seek, media replacement, stop, and shutdown.
The test-only simulated sink adds configurable format negotiation, capacity,
latency, explicit or query-driven buffer consumption, underrun, flush, and
drain behavior without a wall clock. Its player tests cover resampling, A/V
device-master timing, seek, loop, media replacement, natural-end drain, and
monotonic fallback.
The playback test also verifies that two keyed `VideoRenderAPI` instances and
the legacy `setVideoRenderer()` callback can render the same decoded frame
without replacing one another. CPU-renderer tests decode a lossless RGB frame
and verify scaled BGRA, RGBA, and Gray8 output plus padded-stride safety.
Audio-resample tests convert deterministic 8 kHz mono PCM to 16 kHz stereo S16
and verify channel data, drain timing, sample counts, and seek reset behavior.
Audio-file tests verify RIFF/WAVE headers and little-endian samples, then run
the player and converter to produce an exact 64,000-byte 16 kHz stereo S16
payload from deterministic 8 kHz mono input.
AAudio's portable SPSC queue test covers capacity, wraparound, timestamp
continuity, rejection, and reset without an Android device. The connected
Android harness additionally verifies Float32 negotiation, real-time
presentation, monotonic device-clock samples, non-negative combined latency,
pause/resume, background/foreground recovery, drain, and clean close on an
AAudio-capable device.
The same connected harness verifies MediaCodec H.264/HEVC direct-surface
outputs with explicit present/drop, seek/flush, media replacement, stop,
surface-generation replacement, stale-token rejection, decoder/output
lifetime, and NativeActivity shutdown.
It then decodes H.264 and HEVC into separate private GPU-sampled
`AImageReader` surfaces, imports external-format `AHardwareBuffer` images into
Vulkan, samples aligned native allocations using their visible crop, and
returns one release sync fd per import. The device result requires bounded
pending images and zero decoded-source CPU map, software transfer, staging
copy, and renderer upload counters.
Windows D3D11 tests use the WARP device for deterministic offscreen rendering
and cover RGB, YUV420P, NV12, and synthetic P010 PQ/HLG input; SDR tone
mapping; FP16 scRGB and RGB10/PQ numeric output; viewport, aspect ratio,
rotation, resize, target recreation, foreign-device rejection, and
missing-surface events. Native HWND and composition flip-model tests exercise
`IDXGIOutput6`, `SetColorSpace1`, SDR-white lookup, the current Windows HDR
state, composition-monitor resolution, and display switching when two outputs
share the adapter. With Windows HDR enabled on a PHL 27B1U7903, the native
test verifies a 10-bit
G2084/P2020 output, system SDR white, nonzero panel luminance, and FP16 scRGB
highlights above `1.0`. D3D11VA tests cover selected-device creation, shared
locking, bounded
extra frames, native texture/slice validation, and invalid handles; an H.264
hardware integration test covers mapping, pause/resume, seek, media
replacement, stop, target recreation, and retained source/import access after
player shutdown, with an explicit software fallback result when the adapter
has no matching decoder profile. A capability-gated D3D12-generated HEVC
Main10 test uses PQ/BT.2020 metadata and verifies P010 D3D11VA decode,
HDR-preserving Video Processor conversion, FP16 scRGB output above `1.0`,
zero CPU mapping, and pixel readback. WASAPI device and strict native H.264/AAC
example tests pass with an active render endpoint and are explicitly skipped
when a Windows session exposes no endpoint.

## Architecture

```text
Player facade
  ├─ async control + FFmpeg demux worker
  ├─ bounded compressed-packet queues
  │    ├─ audio-decode worker
  │    └─ video-decode worker
  ├─ bounded decoded-audio queue
  │    └─ audio-output worker + device-clock snapshots
  ├─ bounded timestamp-ordered presentation queue
  │    └─ video/frame notification worker with late-frame dropping
  ├─ audio-device master clock with monotonic fallback
  ├─ reference-counted AudioFrame/VideoFrame
  ├─ application render scheduling
  │    └─ keyed VideoRenderAPI instances on native render threads
  └─ optional backend contracts
       ├─ libswscale CPU image-buffer renderer
       ├─ D3D11 software-frame renderer with borrowed Windows resources
       ├─ mobile Vulkan/OpenGL ES selection and bounded recovery policy
       ├─ libswresample interleaved PCM converter
       ├─ RIFF/WAVE diagnostic PCM file sink
       ├─ CoreAudio device sink with AudioQueue clocking
       ├─ WASAPI shared-mode device sink with IAudioClock clocking
       ├─ AAudio device sink with callback-safe SPSC buffering
       ├─ D3D11VA decoder producing retained texture-array slices
       ├─ MediaCodec decoder producing direct-surface present/drop tokens
       ├─ MediaCodec/AImageReader Vulkan hardware-buffer interop
       ├─ Metal software/hardware-frame renderer with borrowed native resources
       ├─ VideoToolbox decoder producing retained CVPixelBuffer frames
       ├─ CVMetalTextureCache zero-copy frame interop
       ├─ lifecycle-connected AudioSink
       └─ HardwareFrame + HardwareFrameInterop
```

The legacy QtAV sources remain unchanged while capabilities are migrated. New
platform backends should target this module rather than adding more Qt
dependencies to the legacy library.

See [MIGRATION.md](MIGRATION.md) for the QtAV API mapping, current limitations,
and threading contract. See [PLAN.md](PLAN.md) for the persistent milestone
status, next task, and backend implementation order. The accepted Windows
D3D11VA device, frame-lifetime, and zero-copy interop design is recorded in
[D3D11VA.md](D3D11VA.md).
The shared Android/OHOS mobile renderer, native lifecycle, hardware-output,
audio, and connected-device test boundaries are recorded in
[`MOBILE.md`](MOBILE.md).
