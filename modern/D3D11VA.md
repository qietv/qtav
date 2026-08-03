# D3D11VA, raw-plane interop, and libplacebo design

Status: implemented on Windows for FFmpeg 8 `AV_PIX_FMT_D3D11`. The legacy
`AV_PIX_FMT_D3D11VA_VLD` API is out of scope.

This document defines the ownership, color, threading, fallback, and validation
contracts shared by `QtAV::HWD3D11VA`, `QtAV::InteropD3D11`,
`QtAV::RenderD3D11`, and `QtAV::OutputD3D11`.

## Invariants

- Decode, interop, and rendering use one application-selected D3D11 device and
  its immediate context.
- Windows builds use only the QtAVCore D3D11 GPU renderer. The Vulkan and
  OpenGL renderer targets are not built on Windows.
- Core public headers expose neither Windows SDK nor FFmpeg types.
- A copied frame retains its FFmpeg frame, decoder array slice, frames context,
  hardware device, COM resources, and synchronization lifetime.
- Zero-copy means no decoded-source CPU map, transfer, staging copy, upload, or
  cross-device copy. libplacebo samples the decoder's native NV12/P010 planes.
- libplacebo is the sole semantic color authority. QtAVCore has no Windows
  shader or Video Processor path for YCbCr conversion, Dolby Vision reshaping,
  PQ/HLG conversion, tone mapping, gamut mapping, or output encoding.
- A Dolby Vision RPU belongs to one exact `VideoFrame`; it is never reused by
  timestamp approximation or applied after ordinary YCbCr conversion.
- Real-time render/context contention is retryable and non-blocking. There is
  no per-frame completion query, `Flush()`, or `pl_gpu_finish()`.

## Target and dependency boundaries

```text
qtav_core
  decoder selection, FFmpeg AVFrame lifetime, structured color/RPU metadata

qtav_platform_windows
  retained D3D11 device/immediate context and shared recursive lock

qtav_hw_d3d11va
  FFmpeg D3D11VA device creation on the selected device
  shader-readable decoder-surface request
  retained texture-array slice accessor

qtav_interop_d3d11
  same-device/raw-resource validation
  exact decoder-frame retention
  no GPU conversion work

qtav_render_d3d11
  libplacebo D3D11 device import
  software AVFrame mapping and hardware luma/chroma plane wrapping
  Dolby Vision/color/tone/gamut/scaling/output pipeline
  viewport, rotation, current target, and Advanced Color discovery

qtav_output_d3d11
  owned device, swap chain, target, render scheduling, and presentation
```

The decoder never depends on the renderer. The renderer consumes a
decoder-independent `D3D11HardwareFrameInterop` contract. The optional interop
target adapts the retained D3D11VA frame without merging decode, render, or
presentation responsibilities.

The repository FFmpeg package supplies libplacebo 7.351.0 with
`PL_HAVE_D3D11=1`, built-in Dolby Vision mapping, glslang, and the complete
static SPIRV-Cross closure. QtAVCore consumes that package with clang-cl/lld-link
on Windows; a system or independently downloaded FFmpeg is not a fallback.

## Selected device and decoder resources

`D3D11DeviceAccess` verifies that a borrowed context is the selected device's
immediate context, retains both COM interfaces, and owns the recursive lock
shared with FFmpeg.

`d3d11vaHardwareDecodeConfig()` allocates an FFmpeg D3D11VA device context,
installs the retained device/context and lock callbacks, requests
`D3D11_BIND_SHADER_RESOURCE`, initializes the context, and returns an opaque
core `HardwareDecodeDevice`. FFmpeg combines that bind flag with its decoder
requirements when allocating the fixed NV12/P010 texture array.

The bounded `extraHardwareFrames` setting is copied to
`AVCodecContext::extra_hw_frames`. Retaining more decoder frames than the
configured allowance applies backpressure; it does not grow an alternate pool
or copy the decoded pixels.

For a D3D11 hardware frame, the generic `NativeHandle` contains:

- `value`: `ID3D11Texture2D*` from `AVFrame::data[0]`;
- `subresource`: the array slice encoded by `AVFrame::data[1]`.

`D3D11VAFrame` is valid only when the texture, slice, device identity, decoded
size, and NV12/P010 software format are consistent. The borrowed native
pointers remain valid while a copy of the strong frame view or source
`HardwareFrame` is alive.

Seek, decoder flush, replacement, and stop release player-owned references but
do not invalidate application copies. An old decoder pool survives until its
last retained frame is released.

## Raw-plane interop and Dolby ordering

`D3D11FrameInterop` validates:

1. D3D11 hardware-frame and same-device identity;
2. `D3D11_USAGE_DEFAULT`, one mip level, and one sample;
3. NV12 or P010 DXGI format and a valid array slice;
4. `D3D11_BIND_SHADER_RESOURCE`.

It returns a retained wrapper around the original decoder texture and slice.
It creates no Video Processor, RGB intermediate, shader resource view, pool,
or GPU command. Its color-space accessor is diagnostic only; semantic metadata
comes from the source `VideoFrame`.

The renderer wraps plane-specific views through `pl_d3d11_wrap()`:

```text
NV12: R8 luma + R8G8 chroma
P010: R16 luma + R16G16 chroma, bit encoding {16, 10, 6}
```

It then applies the source range, matrix, primaries, transfer, chroma location,
mastering display, and content-light metadata. If the frame has
`AV_FRAME_DATA_DOVI_METADATA`, the renderer maps that exact RPU to libplacebo
while the raw plane representation is still active. Only then may
`pl_render_image()` perform the Dolby reshape and ordinary color pipeline.

This ordering is mandatory for Profile 5, whose base layer is not a
conventional HDR10-compatible image. An import that reports an RGB
intermediate or cannot expose raw shader-readable planes is rejected for Dolby
Vision.

Software frames use libplacebo's FFmpeg mapping bridge with Dolby mapping
enabled. An explicit hardware-to-software mapping fallback exists but remains
disabled by default and is reported when used.

## Output color and presentation

The current D3D11 target selects the libplacebo destination contract:

- BGRA8: full-range sRGB/BT.709 SDR;
- FP16: full-range linear BT.709 scRGB;
- RGB10: full-range BT.2020/PQ HDR10.

When a swap chain is supplied, `IDXGIOutput6`, the current monitor, Windows SDR
reference white, display luminance, `CheckColorSpaceSupport()`, and
`SetColorSpace1()` refine the target metadata. These native calls choose and
describe the destination; they do not implement tone mapping.

libplacebo owns YCbCr conversion, Dolby Vision, transfer functions, gamut
mapping, tone mapping, scaling, and final encoding. Native D3D11 code owns only
device/resource validation, target clearing, swap-chain configuration, and
presentation.

## Threading, lifetime, and performance

FFmpeg decode callbacks and QtAVCore D3D11 rendering share the recursive
`D3D11DeviceAccess` lock. `D3D11VideoRenderer::render()` uses a try-lock for
both renderer state and immediate-context ownership; contention declines the
render so the output can retry instead of blocking its render/UI thread.

The imported frame and libplacebo plane wrappers remain alive until
`pl_render_image()` has submitted its commands. They are then released without
waiting for GPU completion. Because decode and render submissions use the same
serialized immediate context, later decoder-surface reuse is ordered after the
earlier reads and D3D11 retains queued resource references.

Per-frame `pl_gpu_finish()`, D3D11 event queries, and synchronous flushes are
forbidden. `pl_gpu_finish()` is used only during renderer teardown before its
reusable libplacebo resources are destroyed. The high-level output separately
uses a frame-latency-one flip-model swap chain, redraw coalescing, non-blocking
`Present()`, and bounded compositor backpressure as specified by AD-005.

## Fallback and failure policy

- `HardwareDecodeConfig::allowSoftwareFallback` controls fallback when D3D11VA
  device/codec initialization fails.
- `D3D11VideoRenderer::setAllowSoftwareMappingFallback()` controls the
  separate, disabled-by-default readback fallback when an individual hardware
  import is incompatible.
- A foreign device, invalid slice, non-shader-readable resource, wrong format,
  or removed device is rejected before semantic rendering.
- No implicit resource sharing, cross-device copy, Video Processor conversion,
  or alternate native shader is attempted.
- Device removal is terminal for that device generation. The application must
  create and bind a new device access, renderer, interop, and decode config.

Dolby Vision support here means FFmpeg-parsed Profile 5 metadata can drive
libplacebo rendering to an SDR/scRGB/HDR10 target. It does not claim Dolby
certification, enhancement-layer residual reconstruction, compressed
passthrough, display tunnelling, or licensed logo behavior.

## Validation matrix

Deterministic WARP tests cover:

- device/context identity and recursive/try-lock behavior;
- retained texture-array slice lifetime;
- NV12/P010 raw-import validation and foreign-device rejection;
- proof that import does not wait on or submit to the immediate context;
- software-frame color/geometry rendering through libplacebo D3D11;
- synthetic PQ, HLG, HDR-to-SDR, HDR10, scRGB, and Dolby Vision RPU rendering;
- retryable context contention, explicit software mapping fallback, target
  recreation, and device-removal classification.

Hardware-adapter integration covers:

- H.264/NV12 and HEVC Main10/P010 D3D11VA decode on the selected device;
- shader-resource bind flags on real decoder arrays;
- exact raw slice retention with zero decoded-source CPU mapping;
- libplacebo rendering to BGRA8 and FP16 targets with pixel readback;
- pause/resume, seek, media replacement, explicit stop, target recreation, and
  retained frame/import lifetime after player shutdown;
- the supplied Dolby Vision Profile 5 media through FFmpeg RPU parsing,
  D3D11VA raw-plane import, and bounded D3D11/libplacebo presentation.

The network-media check is intentionally bounded; it does not download the
8.6-GB file. From a Release build directory, run:

```powershell
./bin/Release/qtav_interop_d3d11_test.exe `
  "https://2dland.cn/test/Wednesday.S01E01.2022.NF.WEB-DL.2160p.HEVC.DV.DDP-Xiaomi.mp4"
```

The test stops after at least 48 rendered hardware frames and requires Dolby
Vision metadata on the rendered sequence. The final 2026-08-03 validation
rendered 49 D3D11VA/P010 frames, all 49 with FFmpeg-parsed Dolby Vision
metadata, with zero decoded-source CPU mapping.

Static/shared builds, install/export consumers, the all-backends-disabled
build, `git diff --check`, and the Qt-dependency scan remain release gates.

## Source and license boundary

Implementation uses QtAVCore code, FFmpeg 8 public APIs, libplacebo public APIs,
and Microsoft D3D11/DXGI APIs. No third-party player shader, lookup table, or
Dolby implementation is copied.

Primary references:

- [FFmpeg 8 hardware-frame parameter contract](https://ffmpeg.org/doxygen/8.0/group__lavc__decoding.html)
- [FFmpeg 8 D3D11VA hardware-context implementation](https://ffmpeg.org/doxygen/8.0/hwcontext__d3d11va_8c_source.html)
- [libplacebo D3D11 API](https://code.videolan.org/videolan/libplacebo/-/blob/v7.351.0/src/include/libplacebo/d3d11.h)
- [Microsoft D3D11 bind flags](https://learn.microsoft.com/en-us/windows/win32/api/d3d11/ne-d3d11-d3d11_bind_flag)
- [Microsoft DXGI NV12/P010 view formats](https://learn.microsoft.com/en-us/windows/win32/api/dxgiformat/ne-dxgiformat-dxgi_format)
