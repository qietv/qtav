# Archived QtAVCore Apple backends

The former QtAVCore macOS and iOS implementation was archived on 2026-08-02.
It is preserved for historical reference only and is no longer built, tested,
packaged, installed, supported, or maintained.

Active QtAVCore development under `modern/` supports Windows, Android, and
OHOS targets only. Do not add these directories back to the active CMake graph
unless Apple platform support is explicitly revived as a new maintained
project decision.

## Archived source

- `modern/backends/render/metal/`: software-frame Metal rendering, borrowed
  native resources, HDR/EDR output, and display-headroom handling;
- `modern/backends/audio/coreaudio/`: CoreAudio/AudioQueue device output;
- `modern/backends/hwaccel/videotoolbox/`: VideoToolbox hardware decode and
  retained `CVPixelBuffer` access;
- `modern/backends/interop/cvmetal/`: zero-copy CVPixelBuffer-to-Metal plane
  import;
- `modern/platform/apple/`: the former Apple platform-helper root;
- `modern/tests/`: the former CoreAudio, Metal, VideoToolbox, CVMetal, and EDR
  tests;
- `modern/examples/coreaudio_console_integration.cpp`: the removed console
  example wiring.

The archived CMake files retain their historical relative paths and target
definitions, but this directory is intentionally not a standalone build. The
old targets were `QtAV::RenderMetal`, `QtAV::AudioCoreAudio`,
`QtAV::HWVideoToolbox`, and `QtAV::InteropCVMetal`.

Historical API usage is recorded in [`modern/README.md`](modern/README.md),
the former migration guidance in
[`modern/MIGRATION.md`](modern/MIGRATION.md), and the completed milestone in
[`modern/PLAN.md`](modern/PLAN.md).

## Historical behavior

The Metal renderer accepted borrowed `MTLDevice` and command-queue objects and
an application callback returning the current texture, drawable, or
`CAMetalLayer`. Its software path covered planar YUV, NV12/NV21, P010,
RGB-family, and Gray8 frames. It supported Fit/Fill/Stretch, viewports,
right-angle rotation, SDR output, extended-linear BT.2020 EDR presentation,
HDR10/HLG metadata, and live display-headroom adaptation.

The VideoToolbox backend selected FFmpeg's hardware path, exposed retained
`CVPixelBuffer` hardware frames, and allowed explicit software fallback. The
CVMetal interop imported limited/full-range NV12 and P010 planes through
`CVMetalTextureCache` and retained them through asynchronous Metal execution
without a decoded-source CPU copy.

The CoreAudio sink followed the default output device or an explicit
`AudioDeviceID`, negotiated interleaved Float32 PCM, used a bounded AudioQueue
pool, and reported playback clock and latency through the generic `AudioSink`
contract.

## Last recorded validation

Before archival, static and shared native builds plus ASan/UBSan passed 29/29
applicable CTest tests on the recorded macOS development host. The Metal
renderer also passed an iOS 16 arm64 Objective-C++ syntax build. Install and
external CMake consumption of all four Apple targets had passed. These are
historical results only; they must not be represented as current support.

## Revival requirements

A future revival must restore explicit public hardware-device identifiers and
FFmpeg mappings, reconnect the backend and test CMake targets, provide current
SDK/toolchain coverage, revalidate lifecycle and HDR behavior on physical
macOS/iOS devices, and update the active support policy. The archived code
must not be assumed compatible with later QtAVCore API changes.
