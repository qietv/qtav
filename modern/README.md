# QtAVCore

QtAVCore is the Qt-free evolution path for QtAV. It keeps FFmpeg as the media
engine, replaces Qt types and the Qt event model with standard C++17, and uses
an application-owned rendering callback inspired by the public shape of
`mdk-sdk`.

It is not source- or binary-compatible with `mdk-sdk`, and no `mdk-sdk` source
code is used.

## Current scope

- no Qt headers, libraries, meta-object compiler, or event loop;
- asynchronous `qtav::Player` state machine;
- FFmpeg 8+ send/receive decoding API;
- local files and FFmpeg-supported network protocols;
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
  and immediate context plus borrowed current render-target views;
- optional Metal renderer for borrowed Apple devices, command queues, and
  current render targets, including complete macOS/iOS extended-linear
  BT.2020 EDR layer presentation;
- optional libswresample converter for negotiated interleaved PCM output;
- optional RIFF/WAVE PCM diagnostic file sink;
- optional macOS CoreAudio device sink with native playback timing;
- optional Windows WASAPI shared-mode device sink with native playback timing;
- optional D3D11VA hardware decoding on an application-selected retained
  D3D11 device, with reference-counted decoder texture-array slices and
  explicit software fallback;
- optional VideoToolbox hardware decoding with reference-counted
  `CVPixelBuffer` frames and explicit software fallback;
- optional CVMetalTextureCache interop for zero-copy limited/full-range
  VideoToolbox-frame rendering through Metal;
- optional platform-neutral Vulkan software-frame renderer using borrowed
  application-selected device/queue and current-image resources, with SDR,
  HDR10/PQ, HDR10/HLG, and extended-linear output contracts;
- optional Android Vulkan surface adapter that retains the current
  `ANativeWindow` generation, selects a supported SDR/native-HDR swapchain,
  publishes its output color space, and owns surface/swapchain synchronization;
- an accepted Android/OHOS mobile rendering policy that prefers Vulkan and
  uses a separate OpenGL ES/EGL backend after Vulkan is unavailable or fails
  fatally, while keeping recoverable surface recreation within the active API;
- a reproducible macOS-to-Android arm64 build and connected-device
  NativeActivity harness for QtAVCore plus pinned FFmpeg 8.1.2 software
  decoding and Vulkan presentation;
- standalone CMake package and headless integration tests.

The core does not open a platform audio device by default. Applications can
keep consuming decoded frames through `onAudioFrame()` and can optionally bind
an `AudioSink`; the macOS CoreAudio and Windows WASAPI implementations remain
separate backend targets so the core acquires no Qt or platform dependency.

Current backend integration boundary:

- `VideoRenderAPI` is connected to `Player` and supports multiple renderer
  instances keyed by application-owned opaque pointers;
- `AudioSink` is connected through `Player::setAudioSink()`, follows playback
  lifecycle changes, and supplies the playback master when its device clock is
  supported and valid; queued audio is drained before close at natural end;
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
- `HardwareDecodeConfig` selects an optional hardware device for video decode;
  its optional reference-counted `HardwareDecodeDevice` lets an in-tree
  backend supply a pre-created native device without exposing FFmpeg or
  platform SDK types;
  `QtAV::HWVideoToolbox` supplies the Apple configuration helper and produces
  `HardwareFrame` values backed by retained `CVPixelBuffer` storage;
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
  BGRA8 textures without mapping decoded pixels through CPU memory;
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
- no Linux native backend, Android audio sink, or Android hardware decoder has
  been implemented yet.

Mobile renderer selection remains in the application or thin platform layer
that owns the native window and graphics devices. A new renderer session
prefers Vulkan and selects the planned OpenGL ES 3.x/EGL backend when Vulkan
is unavailable, lacks required capabilities, or cannot create its initial
surface generation. Recoverable Vulkan surface/swapchain events recreate
Vulkan in place; device loss, unrecoverable submission/presentation failure,
or repeated recreation failure causes a one-way switch to OpenGL ES without
reopening the media. If both APIs fail, video presentation reports an error
while playback, audio, and decoded-frame callbacks remain available. Decoder,
direct-surface, interop, and renderer fallback policies remain independent.
The accepted design is specified in [`MOBILE.md`](MOBILE.md); the OpenGL ES
backend and selector are not implemented yet and do not require SDL3.

The same design plans separate zero-CPU-copy native-buffer interop for Vulkan
and OpenGL ES after direct-surface hardware presentation is stable. On
Android, Vulkan consumes a private GPU-sampled `AImageReader` image by
importing its retained `AHardwareBuffer` and acquire/release fences; OpenGL ES
primarily consumes MediaCodec `SurfaceTexture` output through
`GL_TEXTURE_EXTERNAL_OES`, with `AHardwareBuffer`/`EGLImage` as a
capability-gated alternative. On OHOS, the confirmed GLES path uses
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

- render: `QTAV_RENDER_CPU`, `QTAV_RENDER_OPENGL`, `QTAV_RENDER_VULKAN`,
  `QTAV_RENDER_D3D11`, and `QTAV_RENDER_METAL`;
- audio: `QTAV_AUDIO_WASAPI`, `QTAV_AUDIO_COREAUDIO`, `QTAV_AUDIO_ALSA`,
  `QTAV_AUDIO_PULSEAUDIO`, `QTAV_AUDIO_AAUDIO`, `QTAV_AUDIO_RESAMPLE`, and
  `QTAV_AUDIO_FILE`;
- hardware decode: `QTAV_HW_D3D11VA`, `QTAV_HW_VIDEOTOOLBOX`,
  `QTAV_HW_VAAPI`, and `QTAV_HW_MEDIACODEC`;
- interop: `QTAV_INTEROP_D3D11`, `QTAV_INTEROP_CVMETAL`, and
  `QTAV_INTEROP_VAAPI`.

`QTAV_RENDER_CPU=AUTO` builds the CPU renderer when libswscale is available,
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
targets are available. Backend implementations not otherwise described remain
disabled under `AUTO`, and explicitly requesting one with `ON` is an error.

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

This is a toolchain, packaging, software-decode, and Vulkan-rendering
checkpoint. The renderer uses a bounded three-frame resource ring and retains
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
recreation. It does not yet provide the required OpenGL ES/EGL fallback and
selector, an AAudio sink, or a MediaCodec backend. Their accepted shared
Android/OHOS
responsibility and lifecycle design is documented in [`MOBILE.md`](MOBILE.md).

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

Callbacks are invoked from the playback worker unless explicitly documented by
the integration layer. `renderVideo()` runs on the caller's thread, so an
OpenGL, Vulkan, Metal, or D3D integration can keep ownership of its native
context and surface.

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
surface/swapchain and resumes presentation without reopening media. The Vulkan
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

### D3D11 renderer

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
        return qtav::D3D11RenderTarget { currentRenderTargetView };
    });

qtav::VideoRenderConfig config;
config.surfaceSize = { width, height };
renderer->open(config);
player.setVideoRenderAPI(renderer);
```

`D3D11DeviceAccess::create()` rejects null, foreign-device, and deferred
contexts, retains the selected device and verified immediate context, and
provides the recursive lock shared by the renderer and future D3D11VA/interop
backends. The older renderer constructor taking the two borrowed wrappers
remains a convenience path and creates the same retained access internally.
The callback and returned `ID3D11RenderTargetView` remain application-owned.
The callback runs synchronously inside `render()` and is queried for every
frame, so swap-chain resize or other surface recreation can replace the view
before calling `configure()` with the new size.

The renderer holds `D3D11DeviceAccess::contextGuard()` while issuing immediate
context calls. Applications using that immediate context from another thread
must acquire the same guard (or provide equivalent external serialization):

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
BGRA8 or RGBA8 UNORM 2D targets, applies limited/full-range BT.601, BT.709, or
BT.2020 YUV conversion, and supports custom viewports, Fit/Fill/Stretch, all
right-angle rotations, resize, surface recreation, and surface/device-loss
events.

`QtAV::RenderD3D11` also defines the decoder-independent
`D3D11HardwareFrameInterop` and `D3D11TextureFrame` contracts implemented by
`QtAV::InteropD3D11`. The interop binds to the same retained
`D3D11DeviceAccess`, performs no CPU map during `importFrame()`, and returns
borrowed texture/SRV pointers whose COM resources remain valid while the
returned texture-frame object is alive. Bind it with
`setHardwareFrameInterop()`; the renderer advertises D3D11 hardware-frame
support only when both objects use the same `D3D11DeviceAccess`. Imported
BGRA8 textures are sampled directly by the final viewport/aspect/rotation
pass.

Hardware-frame import and decoder fallback are independent policies. The
renderer does not map a hardware frame by default. Applications may explicitly
enable `setAllowSoftwareMappingFallback(true)` to call
`HardwareFrame::map(Read)` and use the existing software upload path when
interop is unavailable or import fails. A successful mapped fallback emits an
error/detail event containing `software-mapping fallback` so the CPU transfer
is observable; a disabled or failed mapping makes `render()` fail. HDR
presentation remains later Windows work.

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
current NV12/P010 size, creates retained input/output views and a
shader-readable BGRA8 intermediate for each import, and submits
`VideoProcessorBlt()` on the selected immediate/video context. The renderer
passes structured frame color metadata to the interop; Direct3D 11.1 color
spaces are used when available, with a BT.601/BT.709 legacy path otherwise.
The intermediate and final target are SDR. A driver-reported HDR-to-SDR
conversion may be used, but this is not HDR presentation or a product
tone-mapping guarantee.

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
`drain()` waits until accepted buffers have been presented and is called at
natural end before `close()`; its default implementation is a no-op for
synchronous or non-queuing sinks. The `audioBufferView()` helper creates a view
when no conversion is required. `Player` opens an injected
`AudioFrameConverter` when the negotiated format differs, resets it on
flush/seek, drains the converter and sink at natural end, and closes them.
Without an injected converter, a different device format reports
`audio.sink.format` while `onAudioFrame()` continues normally.

`QtAV::AudioResample` implements `SwresampleAudioConverter`. It converts sample
format, sample rate, and channel layout to interleaved U8, S16, S32, float, or
double PCM. Conversion result memory belongs to the converter until its next
operation; `Player` writes it to the sink synchronously. Converter lifecycle
and conversion calls run on the playback worker, serialized with sink calls
and without the player mutex held.

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
falls back to its monotonic software clock. Sink lifecycle and write calls run
on the playback worker without the player mutex held. Natural-end `drain()`
also runs on the playback worker and may block until the backend queue is
presented. `clock()` can also be queried by the thread calling
`Player::position()`.

`HardwareDecodeConfig` is copied by `Player` and applied the next time the
video decoder opens. Changing it while media is loaded interrupts the current
open/read operation and asynchronously reopens the media. The generic core
maps `HardwareDeviceType` to FFmpeg's internal hardware-device selection,
checks the codec's advertised hardware pixel format, and keeps the platform
types private. An unknown device type selects the ordinary software path.

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
Windows D3D11 tests use the WARP device for deterministic offscreen rendering
and cover RGB, YUV420P, and NV12 upload, viewport, aspect ratio, rotation,
resize, target recreation, foreign-device rejection, and missing-surface
events. D3D11VA tests cover selected-device creation, shared locking, bounded
extra frames, native texture/slice validation, and invalid handles; an H.264
hardware integration test covers mapping, pause/resume, seek, media
replacement, stop, target recreation, and retained source/import access after
player shutdown, with an explicit software fallback result when the adapter
has no matching decoder profile. A capability-gated D3D12-generated HEVC
Main10 test verifies P010 D3D11VA decode, Video Processor conversion, zero CPU
mapping, and pixel readback. WASAPI device and strict native H.264/AAC example
tests pass with an active render endpoint and are explicitly skipped when a
Windows session exposes no endpoint.

## Architecture

```text
Player facade
  ├─ async control/state machine
  ├─ FFmpeg demux + decoder contexts
  ├─ audio-device master clock with monotonic fallback
  ├─ reference-counted AudioFrame/VideoFrame
  ├─ application render scheduling
  │    └─ keyed VideoRenderAPI instances on native render threads
  └─ optional backend contracts
       ├─ libswscale CPU image-buffer renderer
       ├─ D3D11 software-frame renderer with borrowed Windows resources
       ├─ libswresample interleaved PCM converter
       ├─ RIFF/WAVE diagnostic PCM file sink
       ├─ CoreAudio device sink with AudioQueue clocking
       ├─ WASAPI shared-mode device sink with IAudioClock clocking
       ├─ D3D11VA decoder producing retained texture-array slices
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
