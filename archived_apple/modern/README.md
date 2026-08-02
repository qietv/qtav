# Archived macOS/iOS QtAVCore integration

This document describes the final Apple backend snapshot before it was
removed from active QtAVCore development on 2026-08-02. It is historical and
unmaintained. The examples below are not compiled by the active source tree.

## Historical CMake targets

- `QtAV::RenderMetal` (`QTAV_RENDER_METAL`)
- `QtAV::AudioCoreAudio` (`QTAV_AUDIO_COREAUDIO`, macOS only)
- `QtAV::HWVideoToolbox` (`QTAV_HW_VIDEOTOOLBOX`)
- `QtAV::InteropCVMetal` (`QTAV_INTEROP_CVMETAL`)

Each former option accepted `AUTO`, `ON`, or `OFF`. The active CMake project no
longer defines these options or exports these targets.

## CoreAudio device output

The former macOS sink was linked with `QtAV::AudioCoreAudio` and
`QtAV::AudioResample`:

```cpp
#include <qtav/coreaudio_audio_sink.h>
#include <qtav/swresample_audio_converter.h>

player
    .setAudioFrameConverter(
        std::make_shared<qtav::SwresampleAudioConverter>())
    .setAudioSink(
        std::make_shared<qtav::CoreAudioAudioSink>());
```

It followed the default output device or an explicit `CoreAudioDevice`,
negotiated interleaved Float32 PCM, copied accepted data into a bounded
AudioQueue pool, and supplied a device-backed clock and latency.

## VideoToolbox and CVMetal

The former decoder selected VideoToolbox before media open:

```cpp
player.setHardwareDecodeConfig(
    qtav::videoToolboxHardwareDecodeConfig());
```

Decoded `HardwareFrame` values retained their `CVPixelBuffer`. The
backend-specific accessor returned a borrowed pixel-buffer reference that
remained valid while the copied hardware frame lived. Software fallback was
explicitly configurable.

For zero-CPU-copy presentation, applications could bind the former CVMetal
interop to the Metal renderer:

```objective-c++
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

The importer accepted limited/full-range NV12 and P010 pixel buffers, created
luma/chroma plane views through `CVMetalTextureCache`, and retained all native
resources through Metal command completion.

## Metal rendering and EDR

The renderer accepted a borrowed device, command queue, and callback returning
the current application-owned texture, drawable, or layer. Its software path
covered YUV420/422/444, NV12/NV21, P010, RGB/BGR/RGBA/BGRA/ARGB, and Gray8,
with Fit/Fill/Stretch, custom viewports, rotation, resize, and redraw.

The final EDR path accepted an application-owned `CAMetalLayer` and active
display, configured RGBA16Float extended-linear BT.2020 output, installed
HDR10/HLG metadata before acquiring the drawable, and exposed system,
display-adaptive, and no-tone-mapping policies. It queried live display
headroom and preserved HDR values above reference white where supported.

## Threading and lifetime

`renderVideo()` remained a synchronous application render-thread call. The
application owned the Metal device, queue, target, layer, and display objects.
Imported hardware resources and copied frames were reference-counted; native
views were valid only for the lifetime documented by their owning wrapper.

These contracts may diverge from later active QtAVCore APIs. Any revival must
be treated as a new port, not as an assumption that the archived code still
builds.
