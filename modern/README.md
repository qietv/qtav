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
- optional Windows D3D11 renderer for borrowed devices, immediate contexts,
  and current render-target views;
- optional Metal renderer for borrowed Apple devices, command queues, and
  current render targets;
- optional libswresample converter for negotiated interleaved PCM output;
- optional RIFF/WAVE PCM diagnostic file sink;
- optional macOS CoreAudio device sink with native playback timing;
- optional VideoToolbox hardware decoding with reference-counted
  `CVPixelBuffer` frames and explicit software fallback;
- optional CVMetalTextureCache interop for zero-copy limited/full-range
  VideoToolbox-frame rendering through Metal;
- standalone CMake package and headless integration tests.

The core does not open a platform audio device by default. Applications can
keep consuming decoded frames through `onAudioFrame()` and can optionally bind
an `AudioSink`; the macOS CoreAudio implementation and GPU render APIs remain
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
- `HardwareDecodeConfig` selects an optional hardware device for video decode;
  `QtAV::HWVideoToolbox` supplies the Apple configuration helper and produces
  `HardwareFrame` values backed by retained `CVPixelBuffer` storage;
- `QtAV::RenderCPU` converts and scales decoded software frames into packed
  RGB/BGR/RGBA/BGRA/ARGB or Gray8 buffers;
- `QtAV::RenderD3D11` uploads software YUV420/422/444, NV12/NV21, P010,
  RGB/BGR/RGBA/BGRA/ARGB, or Gray8 frames and renders through an
  application-owned D3D11 device, immediate context, and current render-target
  view;
- `QtAV::RenderMetal` uploads software YUV420/422/444, NV12/NV21, P010,
  RGB/BGR/RGBA/BGRA/ARGB, or Gray8 frames and renders into the application's
  current Metal texture or drawable, accepts an optional hardware-frame
  interop provider, and applies structured range/matrix/transfer/primaries
  metadata;
- `QtAV::InteropCVMetal` imports supported VideoToolbox `CVPixelBuffer` planes
  as retained Metal textures without mapping or copying through CPU memory,
  preserving whether the native pixel buffer is limited or full range;
- no Windows audio-device or hardware-decoder backend, and no Linux or Android
  native backend, has been implemented yet.

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
`QTAV_RENDER_D3D11=AUTO` builds the native software-frame renderer on Windows,
`QTAV_RENDER_METAL=AUTO` builds the native renderer on Apple hosts with Metal,
`QTAV_AUDIO_RESAMPLE=AUTO` builds the PCM converter when libswresample is
available, `QTAV_AUDIO_FILE=AUTO` builds the dependency-free diagnostic sink,
`QTAV_AUDIO_COREAUDIO=AUTO` builds the macOS device sink when AudioToolbox
and CoreAudio are available, and `QTAV_HW_VIDEOTOOLBOX=AUTO` builds the Apple
hardware-decode selection and native-frame access target when VideoToolbox and
CoreVideo are available. `QTAV_INTEROP_CVMETAL=AUTO` builds the Apple
VideoToolbox/Metal interop target when the Metal renderer and CoreVideo are
available. Other backend implementations are not present yet, so their `AUTO`
behavior is to remain disabled and explicitly requesting one with `ON` is an
error.

Run the headless example:

```sh
build/modern/examples/qtav_core_console /path/to/media.mp4
```

Visual Studio multi-config builds place executables and project DLLs together
under `build/modern/bin/<Config>`, for example
`build/modern/bin/Release/qtav_core_console.exe`.

On macOS, when `QtAV::AudioCoreAudio` and `QtAV::AudioResample` are available,
the console example sends decoded audio to the default output device. Other
hosts retain callback-only audio inspection.

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

### D3D11 renderer

On Windows, link `QtAV::RenderD3D11`, include
`<qtav/d3d11_video_renderer.h>`, and pass a borrowed device, its immediate
context, and a callback that returns the current application-owned render
target:

```cpp
auto renderer = std::make_shared<qtav::D3D11VideoRenderer>(
    qtav::BorrowedD3D11Device(device),
    qtav::BorrowedD3D11DeviceContext(immediateContext),
    [&] {
        return qtav::D3D11RenderTarget { currentRenderTargetView };
    });

qtav::VideoRenderConfig config;
config.surfaceSize = { width, height };
renderer->open(config);
player.setVideoRenderAPI(renderer);
```

The device, immediate context, callback, and returned
`ID3D11RenderTargetView` remain application-owned. The callback runs
synchronously inside `render()` and is queried for every frame, so swap-chain
resize or other surface recreation can replace the view before calling
`configure()` with the new size. The renderer serializes its own use of the
borrowed immediate context, but it does not preserve D3D11 pipeline state;
applications sharing that context must restore their state after
`renderVideo()`.

The software path uploads YUV420P, YUV422P, YUV444P, NV12, NV21, P010,
RGB24, BGR24, RGBA, BGRA, ARGB, and Gray8 frames. It renders to single-sample
BGRA8 or RGBA8 UNORM 2D targets, applies limited/full-range BT.601, BT.709, or
BT.2020 YUV conversion, and supports custom viewports, Fit/Fill/Stretch, all
right-angle rotations, resize, surface recreation, and surface/device-loss
events. HDR transfer and D3D11VA zero-copy interop remain later Windows work.

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
target range. For an EDR/HDR application surface, use an `RGBA16Float` texture
and `MetalOutputColorSpace::ExtendedLinearSRGB`; shader output is then linear
BT.709/sRGB-primary light where `1.0` equals
`MetalRenderTarget::referenceWhiteNits`, and HDR highlights may exceed `1.0`.
The application remains responsible for configuring its `CAMetalLayer`,
display color space, and EDR headroom consistently with that target.

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

`QtAV::RenderD3D11` implements the Windows software-frame renderer. Its
backend-specific public header exposes strong non-owning wrappers for
`ID3D11Device` and `ID3D11DeviceContext`; no Windows SDK type reaches the core
headers. It compiles shaders for the borrowed device, uploads software-frame
planes into per-frame shader resources, and obtains the current borrowed
render target immediately before drawing.

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
events.

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
status, next task, and backend implementation order.
