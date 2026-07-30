# QtAV to QtAVCore migration

QtAVCore is an incremental replacement, not a compatibility wrapper. Legacy
QtAV remains buildable while callers move to the new API one integration at a
time.

QtAVCore requires FFmpeg 8.0 or newer. Compatibility code for FFmpeg 5–7 is
intentionally not carried in `modern/`; this does not change the dependency
range of the legacy root QtAV implementation.

## API mapping

| Legacy QtAV | QtAVCore |
| --- | --- |
| `QtAV::AVPlayer` | `qtav::Player` |
| `setFile(QString)` | `setMedia(std::string)` |
| `play()`, `pause()`, `stop()` | `setState(qtav::State)` |
| Qt signals | `std::function` callbacks |
| `QVariant` properties | string properties |
| `VideoRenderer::receive()` | `onVideoFrame()` |
| Qt paint/update events | `setRenderCallback()` |
| renderer paint method | `renderVideo()` and optional `VideoRenderAPI` |
| `AudioOutput` | `onAudioFrame()` and optional `setAudioSink()` |
| `QThread` playback workers | an internal standard C++ worker |
| `QString`, `QList`, `QImage` frame API | STL values and reference-counted frame views |

The rendering contract follows the same application-owned pattern used by
mdk-sdk:

1. decoding makes a frame current;
2. `setRenderCallback()` asks the application to schedule a redraw;
3. the application calls `renderVideo()` on its native render thread;
4. the configured renderer consumes the reference-counted frame.

## Implemented

- asynchronous load and playback state;
- prepare, pause/resume, seek, stop, playback rate;
- A-B range and finite/infinite looping;
- FFmpeg protocol and demux support;
- best-stream audio/video selection;
- FFmpeg send/receive software decoding;
- decoded video and audio frame callbacks;
- structured video color-space and HDR10 static metadata on `VideoFrame`;
- compile-time video-render, audio-sink, and hardware-frame interop contracts;
- optional audio-sink playback output with device-master clock fallback;
- optional libswresample conversion to negotiated interleaved PCM;
- optional `WavAudioSink` diagnostic output through `QtAV::AudioFile`;
- optional macOS `CoreAudioAudioSink` device output through
  `QtAV::AudioCoreAudio`;
- optional Windows `WasapiAudioSink` shared-mode device output through
  `QtAV::AudioWASAPI`;
- optional D3D11VA hardware decode through `QtAV::HWD3D11VA`, using the
  application-selected `D3D11DeviceAccess` and retained decoder texture-array
  slices;
- optional VideoToolbox hardware decode through `QtAV::HWVideoToolbox`, with
  reference-counted `CVPixelBuffer` frames and explicit software fallback;
- optional `QtAV::InteropCVMetal` import of limited/full-range VideoToolbox
  NV12/P010 pixel-buffer planes into Metal textures without a CPU map or copy;
- multiple video renderer instances keyed by an application opaque pointer;
- libswscale CPU rendering into application-owned packed image buffers;
- D3D11 rendering of decoded software frames into an application-provided
  current render-target view;
- shared retained D3D11 device/immediate-context access and recursive
  synchronization through `QtAV::PlatformWindows`;
- Metal rendering of decoded software frames into an application-provided
  current texture or drawable;
- media and track information;
- interruptible FFmpeg I/O when media changes or playback stops;
- standalone static/shared CMake builds and installable package metadata.

The `VideoRenderAPI` and `AudioSink` contracts are connected to `Player`.
The default decode path remains software-only. Applications can pass the
backend-provided VideoToolbox `HardwareDecodeConfig` before opening media to
select FFmpeg's VideoToolbox hardware path. Hardware video frames attach a
reference-counted `HardwareFrame`; the Apple-specific accessor returns its
borrowed `CVPixelBufferRef`, while the generic contract can perform an
explicit read mapping to CPU memory. Device creation and pixel-format
negotiation failures either report `decoder.hardware.fallback` and continue
in software or report `decoder.hardware.error`, according to the selected
fallback policy.
`HardwareDecodeConfig` can also carry a copied `HardwareDecodeDevice` token
created by an in-tree backend. The token exposes only a generic device type
and opaque native identity in the installed core API while privately retaining
the backend's FFmpeg hardware-device context. This is the common bridge for
decoding on an application-selected native device; changing the token while
media is open causes the same asynchronous decoder reopen as changing the
requested hardware type.
Applications that link `QtAV::InteropCVMetal` can bind a
`CVMetalFrameInterop` to `MetalVideoRenderer`. Supported limited- and
full-range bi-planar NV12 and P010 `CVPixelBuffer` planes are exposed to the
render command as retained Metal textures, without calling
`HardwareFrame::map()` or staging through CPU memory. Imported frame resources
remain alive until asynchronous Metal execution completes.
`VideoFrame::colorSpaceInfo()` replaces string parsing for range, primaries,
transfer, matrix, and chroma location. HDR10 mastering-display and content
light side data are copied into toolkit-independent value types. Metal uses
that metadata for limited/full-range YUV conversion, BT.601/BT.709/BT.2020
matrix selection, PQ/HLG handling, and source-primary conversion. An
`RGBA16Float` target may request `ExtendedLinearSRGB` output so EDR highlights
remain above reference white; configuring the application-owned Metal layer
and display remains the caller's responsibility.
`AudioSink` can use an injected `AudioFrameConverter` when decoded and device
PCM formats differ. Applications link `QtAV::AudioResample` and pass a
`SwresampleAudioConverter` through `Player::setAudioFrameConverter()`.
`AudioSink::drain()` is called at natural end after the converter is drained
and before the sink is closed; the default implementation is a no-op for
existing synchronous or non-queuing sinks. The CPU renderer currently supports
full-surface `Stretch` rendering with no rotation. The Apple-only
Objective-C++ Metal renderer supports Fit, Fill, Stretch, custom viewports,
resize, and all right-angle rotations for software YUV, NV12/NV21, P010, and
RGB-family frames. Its strongly typed device and command queue are borrowed,
and the application supplies the current texture or drawable for each render
call.

On macOS, `CoreAudioAudioSink` follows the default output device or accepts an
explicit backend-specific `CoreAudioDevice`. It negotiates Float32
mono/stereo PCM at the device's nominal sample rate, so decoded formats
normally use the injected `SwresampleAudioConverter`. The sink copies PCM into
its native AudioQueue pool, implements pause/flush/natural-end drain, and
provides the device-master clock and latency consumed by `Player`.

On Windows, `WasapiAudioSink` follows the default multimedia render endpoint
or accepts an explicit owning `WasapiEndpointId`. It negotiates interleaved
Float32 mono/stereo PCM at the endpoint mix rate, so decoded formats normally
use `SwresampleAudioConverter`. A dedicated multimedia-class thread owns COM,
the event-driven WASAPI client, and copied PCM queue. The sink implements
pause/flush/natural-end drain and exposes a cached `IAudioClock`-based
device-master position plus engine and stream latency.

On Windows, `D3D11DeviceAccess` verifies and retains an application-selected
`ID3D11Device` and its immediate context. `D3D11VideoRenderer` accepts that
shared access (or creates one from its compatibility constructor), borrows the
render-target view returned by an application callback, and holds the shared
recursive context guard while rendering. Applications issuing concurrent
calls on the same immediate context must acquire
`D3D11DeviceAccess::contextGuard()` or provide equivalent external
serialization. The renderer uploads software RGB, YUV, NV12/NV21, P010, and
gray frames, performs SDR YUV conversion in a pixel shader, and supports
resize, custom viewports, aspect handling, right-angle rotation, and
render-target recreation. Windows SDK types remain in platform/backend
headers and never enter a core public header.

`d3d11vaHardwareDecodeConfig()` creates FFmpeg's D3D11VA device on the same
retained device access, installs callbacks for the shared recursive lock, and
requests a bounded number of extra decoder surfaces. `D3D11VAFrame` retains a
decoded NV12/P010 texture-array slice and validates its native resource before
returning borrowed D3D11 pointers. The core `NativeHandle` now carries an
optional subresource index, while Windows SDK and FFmpeg declarations remain
outside installed core headers. Explicit CPU mapping, software fallback, seek,
media replacement, stop, and retained lifetime after player shutdown are
implemented. `QtAV::RenderD3D11` now exposes decoder-independent
`D3D11HardwareFrameInterop` and retained `D3D11TextureFrame` interfaces for
the next adapter layer; imported texture and shader-view pointers remain valid
while the texture-frame object lives. Renderer consumption, zero-copy
decoder-texture conversion, software-map fallback, and HDR output remain
separate follow-up work under the accepted contract in
[D3D11VA.md](D3D11VA.md).

For offline PCM inspection, `WavAudioSink` negotiates an interleaved output
format and writes a standard RIFF/WAVE file. It does not expose a device clock
or pace playback. Decoded planar audio therefore normally uses
`SwresampleAudioConverter` before reaching the file sink.

## Deliberately deferred

- remaining platform audio device implementations (ALSA/PulseAudio, AAudio);
- OpenGL and Vulkan renderer implementations;
- remaining hardware decoders and D3D11/non-Apple GPU zero-copy interop;
- subtitle decoding and libass rendering;
- active track switching after load;
- buffering policy for live/network streams;
- audio time-stretch without pitch change;
- compressed Dolby passthrough, Atmos object rendering, and Dolby Vision.

The current audio callback exposes the decoder's native sample format and
reference-counted planes. A platform audio sink should convert/resample only
when its device format requires it.

### Dolby formats

The core is codec-agnostic and uses the decoder registered by FFmpeg. AC-3,
E-AC-3, and TrueHD software decoding have been exercised through the audio
frame callback. This is PCM decode support only; IEC 61937/HDMI passthrough,
Atmos object rendering, Dolby Vision processing, licensing, and certification
are separate backend/product work.

## Threading rules

- control methods are thread-safe;
- state, status, audio, and video callbacks run on the playback worker;
- media events normally run on the playback worker; forwarded audio-sink
  events run on the backend's event thread;
- callbacks may request another player state, but must not destroy the player;
- `renderVideo()` runs synchronously on its caller and should be called from the
  thread that owns the native graphics context;
- D3D11 renderer, decoder, interop, and application calls sharing one
  immediate context must serialize through the same
  `D3D11DeviceAccess::contextGuard()` or equivalent external locking;
- `VideoRenderAPI::render()` runs synchronously inside `renderVideo()` and
  backend event callbacks may request another player state;
- audio-sink and video-render backend event callbacks run on the thread chosen
  by the backend and may request another player state;
- audio-sink lifecycle, write, and natural-end drain calls run on the playback
  worker without the player mutex held; `drain()` may block until queued audio
  is presented, while `clock()` may also run on a `position()` caller thread;
- audio-converter lifecycle and conversion calls run on the playback worker,
  serialized with sink calls and without the player mutex held;
- changing `HardwareDecodeConfig` while media is open interrupts and
  asynchronously reopens the decoder; hardware frame callbacks still run on
  the playback worker;
- replacing its supplied `HardwareDecodeDevice` token also reopens the
  decoder; a token/type mismatch follows the selected software-fallback
  policy before decoder open;
- frame data remains valid for as long as the copied `AudioFrame` or
  `VideoFrame` object is alive.
- an opaque hardware native handle remains valid while its `HardwareFrame`
  lives, and a CPU mapping remains valid while its mapping object lives.

## Recommended next implementation order

1. Use the CPU swscale renderer and image-buffer target as the reference
   software path.
2. Use the interleaved PCM libswresample backend when a sink negotiates a
   different format.
3. Complete the Apple production path by adding CoreAudio, then VideoToolbox
   and CVPixelBuffer/Metal interop.
4. Add hardware decode and zero-copy frame handles for that path.
5. Implement the accepted Windows D3D11VA device/frame contract, then add
   zero-CPU-copy texture rendering through D3D11 interop.
6. Add subtitle and multi-track switching.
7. Add live-stream buffering and recovery policies.

Each backend should remain optional so the core library never acquires a GUI
toolkit dependency.
