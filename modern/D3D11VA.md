# D3D11VA device, frame, and interop design

Status: implemented and verified on Windows; both `QtAV::HWD3D11VA` and
`QtAV::InteropD3D11` are complete for the documented SDR output path.

This document records the ownership, threading, fallback, and test contracts
implemented by `QtAV::HWD3D11VA` and `QtAV::InteropD3D11`. It is deliberately
limited to FFmpeg 8's modern `AV_PIX_FMT_D3D11` path. The legacy
`AV_PIX_FMT_D3D11VA_VLD` API is out of scope.

## Invariants

- The preferred path decodes and renders on one application-selected D3D11
  device.
- Core public headers continue to expose neither Windows SDK nor FFmpeg types.
- D3D11 types appear only in Windows platform/backend headers.
- A copied QtAVCore frame keeps its decoder surface, array slice, frame pool,
  hardware device context, synchronization state, and native device alive.
- The playback worker may submit decode or transfer work while the application
  calls `renderVideo()` on its render thread, but calls on the shared immediate
  and video contexts are serialized.
- "Zero-copy interop" means that no decoded pixels are mapped through CPU
  memory and no cross-device staging copy is made. A same-device D3D11 Video
  Processor pass is allowed.

The last distinction is necessary for the initial implementation. FFmpeg's
D3D11VA decoder uses one fixed-size `D3D11_BIND_DECODER` texture array, while
Direct3D 11 does not allow a shader-resource view to be created from a decoder
texture array. The decoder texture and slice will therefore be consumed
directly as a Video Processor input and converted to a same-device,
shader-readable intermediate texture. The existing renderer remains
responsible for the final viewport, aspect-ratio, rotation, and target pass.

## Target and dependency boundaries

The implementation will use these responsibilities:

```text
qtav_core
  generic hardware-device token, FFmpeg decoder selection, AVFrame lifetime

qtav_platform_windows
  retained D3D11 device/immediate-context access and shared recursive lock

qtav_hw_d3d11va
  FFmpeg D3D11VA device creation on the selected device
  D3D11VA configuration helper
  retained decoder-texture-and-array-slice accessor

qtav_interop_d3d11
  foreign-device validation
  D3D11 Video Processor import/conversion
  shader-readable same-device texture view

qtav_render_d3d11
  software upload
  interop interface consumption
  final shader, geometry, viewport, rotation, and current render target
```

`qtav_hw_d3d11va` must not depend on the renderer.
`qtav_render_d3d11` must not depend on a decoder implementation.
`qtav_interop_d3d11` is the optional adapter between the two. Video Processor
code initially remains an internal responsibility of the interop target; it
can become a separate target later if another backend needs the same service.

The common Windows target is required to avoid either a renderer-to-decoder
dependency or independent locks around the same immediate context.

## Selected device and core bridge

The Windows platform target will add a reference-counted
`D3D11DeviceAccess` value. Its factory receives a borrowed
`ID3D11Device` and `ID3D11DeviceContext`, verifies that the context is the
device's immediate context, retains both COM interfaces, and creates the
shared recursive lock.

The existing renderer constructor that accepts independent borrowed device and
context values remains the software-only convenience path. The hardware path
uses a new renderer overload taking the same `std::shared_ptr<D3D11DeviceAccess>`
that is passed to the D3D11VA configuration helper. Applications which issue
their own immediate-context calls concurrently can acquire the same public
RAII context guard; otherwise they retain their existing responsibility to
avoid racing QtAVCore.

Core will add a concrete, PIMPL-backed `HardwareDecodeDevice` token to
`HardwareDecodeConfig`. The public token reports only
`HardwareDeviceType` and generic native identity. An uninstalled private bridge
between core and in-tree hardware backends carries a referenced FFmpeg
`AVHWDeviceContext`; no FFmpeg declaration enters an installed header.

`d3d11vaHardwareDecodeConfig(deviceAccess, options)` will:

1. allocate an FFmpeg 8 D3D11VA device context;
2. install retained references to the selected D3D11 device and its immediate
   context;
3. install lock/unlock callbacks backed by the shared recursive lock;
4. initialize the FFmpeg device context and wrap it in the generic core token;
5. set the requested hardware type and software-fallback policy.

`Player` takes an `av_buffer_ref()` of that supplied device context before
`avcodec_open2()`. If no device token is supplied, the existing
FFmpeg-created-device behavior remains available for generic hardware
selection and for the VideoToolbox path. A supplied token whose type differs
from `HardwareDecodeConfig::deviceType` is rejected before decoder open.

The D3D11VA options include a bounded `extraHardwareFrames` value. It is copied
to `AVCodecContext::extra_hw_frames` before opening the codec so FFmpeg can size
its required fixed decoder array for the current codec, reference pictures,
frame threading, QtAVCore's current render frame, and a small number of
application-retained frames. The default will be four extra frames. Retaining
more frames than the configured allowance deliberately applies decoder
backpressure rather than silently copying frames or growing an incompatible
pool.

## Retained native-frame access

Core's generic `NativeHandle` gains a subresource/index field. For
`HardwareDeviceType::D3D11`, a texture handle contains:

- `value`: the decoded `ID3D11Texture2D*` from `AVFrame::data[0]`;
- `subresource`: the array slice encoded by `AVFrame::data[1]`.

The core value remains toolkit-independent. The D3D11VA backend exposes a
strong Windows-only value, tentatively named `D3D11VAFrame`, with:

- a retained `HardwareFrame`;
- a borrowed `ID3D11Texture2D*`;
- the `UINT` array slice;
- the source `ID3D11Device*` identity;
- decoded size and software format.

`d3d11vaFrame(const HardwareFrame&)` returns an invalid value unless the frame
is D3D11 hardware, the texture is present, the slice is within the texture
array, the texture's device matches the retained source-device identity, and
the software format is supported. The native pointers remain valid while a
copy of `D3D11VAFrame` or its source `HardwareFrame` is alive. Native code that
needs a longer independent lifetime must call `AddRef`.

The retained `HardwareFrame` continues to own a cloned FFmpeg `AVFrame`. Its
buffer reference owns the texture slice and `AVHWFramesContext`; the frames
context owns the hardware-device reference; the device reference owns the COM
interfaces and lock state. Consequently:

- seek and `avcodec_flush_buffers()` invalidate queued/current player frames,
  but not copies already held by the application;
- media replacement and stop release player references without invalidating
  retained application frames;
- old pools survive codec reconfiguration until their last frame is released;
- a retained frame can still be mapped or inspected after `Player` is
  destroyed, unless the native device itself has been removed.

## Context locking

FFmpeg 8 requires D3D11VA device lock callbacks to use a recursive lock. The
callbacks protect its immediate-context, video-context, and internal staging
texture operations, but they do not protect application or renderer calls.
QtAVCore therefore installs callbacks that use the same recursive mutex as
`D3D11DeviceAccess`.

The lock is acquired for:

- FFmpeg decode submission through its installed callbacks;
- `av_hwframe_transfer_data()` through those same callbacks;
- interop Video Processor input/output view creation and blits;
- renderer immediate-context map, state, draw, copy, and flush operations.

Device resource-creation methods are free-threaded, but implementations may
hold the guard across related creation and submission to keep failure and
device-removal handling atomic. Backend event callbacks are invoked only after
the guard is released.

QtAVCore does not enable or disable `ID3D11Multithread` protection behind the
application's back. The shared guard is the synchronization contract for
QtAVCore components. An application that accesses the same immediate context
from additional threads must use that guard or provide equivalent external
serialization.

## Interop and final rendering

`qtav_render_d3d11` defines decoder-independent
`D3D11HardwareFrameInterop` and retained `D3D11TextureFrame` interfaces,
parallel to the Metal renderer contracts. The renderer advertises D3D11
hardware-frame support only while a compatible interop object is installed.

`qtav_interop_d3d11` implements the interface as follows:

1. validate the D3D11VA retained frame and compare source and target COM device
   identity;
2. validate NV12 or P010 software format, coded size, array slice, device
   health, and Video Processor format support;
3. cache the enumerator and processor for the current size/format, then create
   retained input/output views and a same-device shader-readable RGB
   intermediate for the import;
4. configure source/destination rectangles and color space from structured
   frame metadata;
5. submit `VideoProcessorBlt()` under the shared context guard;
6. return a retained texture frame which keeps the source frame and all
   imported resources alive through the renderer submission.

The initial intermediate is SDR BGRA8. HDR transfer/output remains a later
extension and must not be silently described as HDR presentation. The renderer
then applies its existing geometry to the current application-owned render
target. Target/swap-chain recreation changes only the current-target callback
and renderer configuration; it does not rebuild the decoder device or pool.

Direct shader views over the decoder array are not an initial path. A future
implementation may add a proven vendor/runtime path, but it must be capability
checked and cannot replace the portable Video Processor contract.

## Fallback and failure policy

There are two distinct fallback decisions:

- `HardwareDecodeConfig::allowSoftwareFallback` controls reopening/continuing
  with FFmpeg software decoding when D3D11VA device creation, codec capability,
  pixel-format negotiation, or decoder initialization fails.
- `D3D11VideoRenderer` has a separate, disabled-by-default
  `allowSoftwareMappingFallback` option. When import is unsupported or the
  source belongs to another healthy device, the renderer may call
  `HardwareFrame::map(Read)` and feed the result to its existing software
  upload path.

No implicit cross-device GPU resource sharing or copy is attempted. A foreign
device is rejected before any context operation. CPU mapping is the only
initial cross-device fallback and is observable through a renderer error/detail
event; when disabled or unsuccessful, `render()` fails.

The state rules are:

- seek/loop flush: clear the player's current frame and interop caches which
  are tied to frame parameters; retained external frames remain valid;
- media replacement/stop: close the codec and release active pools after
  retained frames drain naturally;
- surface recreation: reacquire the current target on every render and retain
  the decoder/intermediate resources;
- decoder format/size change: create a new FFmpeg pool and rebuild interop
  Video Processor resources without mutating retained old-frame state;
- device removal: classify the decoder/interop/renderer failure consistently,
  stop submitting work, and report a terminal backend error. Automatic
  recreation is not attempted because both the decoder pool and render target
  belong to the removed device. The application creates a new
  `D3D11DeviceAccess`, renderer, interop, and decode config and rebinds them.

Software mapping may fail for a format for which FFmpeg does not support
`av_hwframe_transfer_data()` (notably an opaque 4:2:0 surface). That failure is
reported; it is never treated as a valid empty frame.

## Validation matrix

Deterministic Windows/WARP tests do not claim hardware-video support. They
cover contracts that WARP can exercise reliably:

- device/immediate-context identity validation and retained COM lifetime;
- shared recursive locking from playback-worker and render-thread simulations;
- D3D11 texture-array/slice extraction using a synthetic hardware-frame data
  object;
- invalid slice, wrong format, missing texture, and foreign-device rejection;
- interop capability reporting and renderer behavior with a mock imported
  texture frame;
- enabled/disabled software-map fallback and map failure;
- seek/media-replacement/stop/shutdown lifetime using mock hardware frames;
- surface recreation plus factored device-removal/error classification;
- no Windows or FFmpeg declarations in core installed headers.

Real-GPU integration tests use generated media and skip with an explicit
reason when the adapter or codec profile is unavailable. They cover:

- H.264 D3D11VA decode on the application-selected device;
- NV12 texture-array and slice access, bounded pool use, and CPU mapping;
- same-device D3D11VA-to-Video-Processor-to-render-target presentation with no
  call to the CPU mapping path;
- pixel readback within tolerance, seek, pause/resume, media replacement,
  stop, and retained-frame use after player shutdown;
- P010/HEVC when a checked adapter profile supports it;
- foreign-device rejection and the explicit CPU fallback path.

Static and shared builds, install/export consumption of
`QtAV::PlatformWindows`, `QtAV::HWD3D11VA`, `QtAV::RenderD3D11`, and
`QtAV::InteropD3D11`, the all-backends-disabled build, `git diff --check`, and
the Qt-dependency scan remain release gates.

## Implementation order

1. [x] Add the common Windows device-access target and the opaque
   supplied-device bridge in core.
2. [x] Add `qtav_hw_d3d11va`, decoder configuration, retained texture/slice
   access, mapping, and lifecycle tests.
3. [x] Add decoder-independent D3D11 renderer interop interfaces.
4. [x] Add renderer capability reporting, texture-frame consumption, and
   explicit software-map fallback with mock WARP tests.
5. [x] Add the Video Processor implementation in `qtav_interop_d3d11`.
6. [x] Add WARP contract tests, native zero-copy tests, console-example
   wiring, install/export validation, and final public documentation.

Completed Video Processor checkpoint:

- `QtAV::InteropD3D11` is an optional Windows target that depends on the
  decoder and renderer contracts without merging their responsibilities;
- `D3D11FrameInterop` rejects invalid slices, unsupported NV12/P010 resources,
  removed or foreign devices before context work, then serializes Video
  Processor operations through the shared recursive guard;
- the interop caches the enumerator/processor for the active size and format,
  creates retained per-import views and an SDR BGRA8 shader resource, and
  keeps the decoder frame alive through final renderer submission;
- the renderer supplies structured color metadata through a backward-
  compatible color-aware import overload; Direct3D 11.1 color spaces cover
  range, BT.601/709/2020, PQ, HLG, and chroma siting when the driver reports
  the conversion, with a legacy SDR BT.601/709 fallback;
- WARP verifies texture/slice extraction, retained lifetime, recursive locking,
  and safe unavailable behavior on systems without software Video Processor
  support;
- hardware-adapter tests prove generated H.264/NV12 and HEVC Main10/P010
  D3D11VA frames reach the render target with correct pixel readback and no CPU
  mapping; Main10 media generation and decode are capability-gated;
- the H.264 zero-copy test covers pause/resume, seek, media replacement,
  explicit stop, target recreation, and retained source/import use after
  `Player` shutdown;
- the headless console example wires the selected device, D3D11VA decoder,
  Video Processor interop, offscreen D3D11 renderer, and WASAPI audio path; its
  strict H.264/AAC CTest has passed with an active WASAPI render endpoint and
  audible output, while unavailable endpoints still return skip code 77
  instead of reporting a false device pass.

## Source and license boundary

Implementation is based only on QtAVCore's existing code, FFmpeg 8 public APIs,
and Microsoft public D3D11/DXGI APIs. Aleksoid1978/VideoRenderer is an isolated
GPL-3.0 behavioral reference only. Its C++, shaders, lookup tables, data, and
vendor-specific extensions must not be copied, vendored, or linked.

Primary API references:

- [FFmpeg 8 hardware-frame parameter contract](https://ffmpeg.org/doxygen/8.0/group__lavc__decoding.html)
- [FFmpeg 8 D3D11VA hardware-context implementation](https://ffmpeg.org/doxygen/8.0/hwcontext__d3d11va_8c_source.html)
- [Microsoft D3D11 device/context threading](https://learn.microsoft.com/en-us/windows/win32/direct3d11/overviews-direct3d-11-devices-intro)
- [Microsoft D3D11 bind-flag rules](https://learn.microsoft.com/en-us/windows/win32/api/d3d11/ne-d3d11-d3d11_bind_flag)
- [Microsoft Video Processor input-view contract](https://learn.microsoft.com/en-us/windows/win32/api/d3d11/nf-d3d11-id3d11videodevice-createvideoprocessorinputview)
- [Microsoft DXGI NV12/P010 view formats](https://learn.microsoft.com/en-us/windows/win32/api/dxgiformat/ne-dxgiformat-dxgi_format)
