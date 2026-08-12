# QtAVCore OHOS XComponent example

The user-facing player demo is documented separately in
[`PLAYER_DEMO.md`](PLAYER_DEMO.md). It adds local-document and URL opening,
full-screen controls, pitch-preserving rates, audio/subtitle switching,
system picture-in-picture, presentation-timed subtitles, and a closeable 1 Hz
media/FPS overlay. Its clean HAP template contains no committed signing
material, and its build script never installs or launches the application;
local DevEco automatic signing can produce the signed device package. The
remainder of this file documents the automated production-path validation
harness.

This example is the OHOS production-path shell. ArkUI owns one `XComponent`;
its native lifecycle publishes `OHNativeWindow` generations to
`QtAV::RenderVulkanOHOS` and `QtAV::RenderOpenGLOHOS` through the shared
`QtAV::RenderMobile` selector. The native adapters own their OHOS
surface/context/swapchain resources while the platform-neutral Vulkan and
OpenGL ES engines render decoded software frames.

For every GPU renderer, libplacebo is the sole semantic authority for color
conversion, residual-disabled base-layer Dolby Vision reshaping, tone/gamut
mapping, scaling, and output encoding. Platform adapters and future
hardware-frame interop may only import native resources, preserve fences and
lifetime, and normalize a raw representation when direct wrapping is
impossible; they must not implement a competing color shader. Enhancement-layer
reconstruction and Dolby certification are not claimed.

The native module receives short packaged H.264/AAC and HEVC/AAC test clips in
one `start()` call and copies them into app storage. AAC is converted to
negotiated 48 kHz Float32 PCM through `QtAV::AudioResample` and presented
through `QtAV::AudioOHAudio`. Its first selector session deliberately
makes Vulkan unavailable and requires 20 OpenGL ES presentations. A new
renderer session then selects Vulkan, injects a fatal result after 12 presented
frames, and requires 30 more frames through the selector's one-way OpenGL ES
fallback. Between the two renderer sessions the harness pauses playback, seeks,
and resumes. The H.264 clip loops so the final marker can additionally
require OHAudio callback delivery, a valid hardware presentation clock,
non-negative latency, successful flush, and segment-end drain while retaining
exactly one application media open. After that software/audio regression, it
opens H.264 through the required `*_ohcodec` wrapper with software fallback
disabled. It exercises pause/resume and a two-second seek, then asks the
connected-device runner for one real background/foreground cycle. The runner
sends HOME once and relaunches the ability after the page destroys its
XComponent surface. The page reports foreground/background state to native
code and creates a fresh XComponent without restarting `start()`. The native
harness then replaces H.264 with HEVC, verifies bounded retained output across
stop, and emits PASS only after the complete lifecycle succeeds. It next opens
H.264 and HEVC through `QtAV::InteropOHCodecOpenGL`, requires exact normalized-
PTS native-image association, samples raw Y/Cb/Cr, and tracks Dolby Vision RPU
metadata from queued frame through matched image release. It finally opens
H.264 and HEVC through `QtAV::InteropOHCodecVulkan`, imports and shader-samples
30 outputs from each codec through the interop's private
`OH_ConsumerSurface`, and exercises the native-buffer path. It then runs two
selector-backed H.264 sessions. The first starts on OHCodec/Vulkan and, after a
real import or after a bounded injected failure, rebinds subsequent decoder
output to a
new `OH_NativeImage`/OpenGL ES interop surface without calling `setMedia()`.
The second starts from OHCodec/Vulkan again, clears the hardware-decode
configuration at the same selector transition, and requires software frames
to continue through OpenGL ES. These are independent policies: no frame from
the retired Vulkan surface is retried, mapped, or copied across APIs.
Native OHOS HDR output selection is implemented, cross-build verified, and
connected-device validated through an ArkUI `XComponentType.SURFACE`. ArkUI's
HDR brightness hint follows the native HDR policy; both adapters mark the
`OHNativeWindow` as a video source, set its HDR white point, synchronize the
selected SDR/HDR color space, and attach HDR type/static metadata before the
next surface-buffer request. Vulkan presented A2B10G10R10 BT.2020/PQ and
OpenGL ES presented exact RGB10_A2 BT.2020/PQ; both passed required-HDR mode
and triggered RenderService's HDR algorithm on the connected device. Explicit
SDR remained RGBA8/sRGB with deterministic tone mapping. The Vulkan interop retains the exact acquired
`OHNativeWindowBuffer`/`OH_NativeBuffer`, imports it through
`VK_OHOS_external_memory`, and can be called strict no-intermediate source
zero-copy only when the driver exposes an explicit `VkFormat` and plane mapping
that libplacebo wraps directly. An opaque format is imported with
`VkExternalFormatOHOS`; ordinary input uses the suggested YCbCr conversion,
while Dolby Vision uses `RGB_IDENTITY`, preserves the driver component mapping,
and reorders sampled `.gbr` from Vulkan's `(Cr,Y,Cb)` convention into
`(Y,Cb,Cr)` during one GPU normalization pass. The OpenGL ES
fallback likewise
requires raw `GL_EXT_YUV_target` sampling followed by crop-aware RGBA16F GPU
normalization before libplacebo; it is zero-CPU-copy but not strict source
zero-copy. Implicit `OH_NativeImage` external-OES YUV-to-RGB conversion
is not the target. OHCodec/NativeImage may propagate the codec PTS unchanged in
microseconds, so the interop compares the observed value and its
microsecond-to-nanosecond candidate against the exact queued-frame PTS set,
then stores and correlates the selected value in nanoseconds. Dolby Vision
validation additionally requires the exact metadata-bearing frame to survive
that match until image release.

The 2026-08-06 Mate 60 Pro run exposed `VK_FORMAT_UNDEFINED` with opaque
external format `1000156003` for NV12; a separate HEVC Main10/P010 fixture
exposed `1000156013`. The independent raw Vulkan object probe and the full
opaque shader path both succeeded. The harness emitted
`QTAV_OHOS_OHCODEC_VULKAN_RESULT PASS mode=opaque-ycbcr-normalized` with 30
H.264 plus 30 HEVC presentations, 60 imports, 60 GPU normalization passes, and
zero CPU maps, transfers, staging copies, or uploads. A device or public
contract with an explicit multi-plane Vulkan format is still required for the
narrower strict no-intermediate claim.

For Huawei's explicit-format experiment, configure
`QTAV_OHOS_EXTERNAL_FORMAT_PROBE_MODE=NATIVE` or `LIBPLACEBO`. Both modes map
`1000156003` to `VK_FORMAT_G8_B8R8_2PLANE_420_UNORM` and `1000156013` to
`VK_FORMAT_G10X6_B10X6R10X6_2PLANE_420_UNORM_3PACK16`, while omitting
`VkExternalFormatOHOS` from `VkImageCreateInfo::pNext`. The native sampler run
and the direct libplacebo 7.351.0 run each rendered 30 H.264/NV12 and 30 HEVC
Main10/P010 frames. The libplacebo marker reported `directPlanes=60` and
`normalization=0`, proving that libplacebo accepts both explicit formats. The
option defaults to `DISABLED`; the numeric mapping is not enabled for
production. Huawei's formal 2026-08-12 reply identifies that equality as a
driver implementation detail, confirms that the tested driver has no explicit-
format switch, and directs applications to the opaque external-format route.
It also confirms identity raw-component sampling and full P010 10-bit data
with the queried range/chroma properties.

The final signed player HAP kept `legend.mkv` on OHCodec/Vulkan at 25.0 FPS and
kept Dolby Vision `wednesday.mp4` on OHCodec/Vulkan at 24.1 FPS. Forced-SDR
captures first verified correct color for both suggested-RGB and raw-identity
sampling. The final same-process snapshot reported 1,988 opaque imports,
normalization passes, consumer-buffer releases, and callbacks, zero numeric-
workaround imports, and zero Player drops.
The same run also emitted `QTAV_OHOS_OHCODEC_FALLBACK_RESULT PASS` after the
OHCodec surface generation changed from 5 to 6 and 30 raw-YCbCr OpenGL ES
frames were presented. Its independent software route emitted
`QTAV_OHOS_OHCODEC_SOFTWARE_FALLBACK_RESULT PASS` with 30 software
presentations. Reconfiguring the decoder did not increment the application
media-open counter, and both paths reported zero decoded-source CPU map,
software transfer, staging, and upload calls.
The generated 440 Hz and 660 Hz tones allow a manual audibility check, while
automation validates delivery and hardware timing.

An optional VVC-only mode is selected by packaging `-VVCMediaSource`. It
preflights the exact 600-frame 1280x720/60 `vvc1` fixture, queries the OHOS
hardware capability, presents the complete stream through `vvc_ohcodec`, then
covers pause/resume, seek/flush, explicit stop, a real XComponent surface
recreation, stale-generation rejection, and bounded direct-surface lifetime.
It finally forces a missing supplied OHCodec device and requires the same
media to reopen through FFmpeg's native software VVC decoder without a stale
hardware frame.

Build the QtAVCore shared libraries, native N-API module, and unsigned template
HAP with:

```powershell
./modern/examples/ohos/build-ohos-hap.ps1
```

The template intentionally contains no signing material. To reuse an existing
DevEco project whose signing is already configured, pass its root. The script
preserves its root signing profile and synchronizes only the validation page,
native type declarations, generated test media, and required arm64 libraries:

```powershell
./modern/examples/ohos/build-ohos-hap.ps1 `
  -ProjectRoot C:/path/to/signed-project
```

By default the script generates six-second, seekable 320x180/30 fps H.264/AAC
and HEVC/AAC clips. `-H264MediaSource` and `-HEVCMediaSource` can stage explicit
fixtures; the older `-MediaSource` option remains an alias for
`-H264MediaSource`. `-HEVCMediaDurationSeconds` makes a bounded stream-copy
fixture and passes `-strict unofficial` so FFmpeg preserves a Dolby Vision
configuration record.

Run the signed result on exactly one connected device and collect the native
result marker with:

```powershell
./modern/examples/ohos/run-connected-device.ps1 `
  -ProjectRoot C:/path/to/signed-project `
  -BundleName com.example.bundle
```

Run the capability-gated VVC matrix with the supplied fixture using:

```powershell
./modern/examples/ohos/run-connected-device.ps1 `
  -VVCMediaSource C:/Users/zzzhr/Downloads/vvc.mp4 `
  -TimeoutSeconds 120
```

The runner requires `OMX.hisi.video.decoder.vvc` on the recorded Pura X Max,
600 hardware presentations, all lifecycle counters, exactly one deliberate
software-fallback event, `maxPending=1`, `maxQueued=0`, and no decoded-source
map, transfer, staging, or upload operation.

For a residual-disabled base-layer Dolby Vision fixture, request a strict
profile assertion. The runner uses `ffprobe` before building, then requires
every rendered HEVC frame to complete the RPU queued/matched/released chain.
Profile `84` means Profile 8 with compatibility id 4:

```powershell
./modern/examples/ohos/run-connected-device.ps1 `
  -ProjectRoot C:/path/to/signed-project `
  -BundleName com.example.bundle `
  -HEVCMediaSource C:/media/profile5.mp4 `
  -HEVCMediaDurationSeconds 6 `
  -RequireDolbyVisionProfile 5

./modern/examples/ohos/run-connected-device.ps1 `
  -ProjectRoot C:/path/to/signed-project `
  -BundleName com.example.bundle `
  -HEVCMediaSource C:/media/profile84.mov `
  -RequireDolbyVisionProfile 84
```

The 2026-08-06 Mate 60 Pro runs passed both profiles with 45 rendered HEVC
frames and `doviQueued=45`, `doviMatched=45`, `doviReleased=45`. Both runs
reported raw YCbCr input, zero implicit-RGB images, and zero decoded-source CPU
map, software transfer, staging, or upload calls. P010 remains an opaque
external format; the generic opaque YCbCr conversion can consume it, but strict
raw-component Dolby Vision semantics remain pending.

Installation is attempted once. If HarmonyOS asks for device-side approval,
approve it manually and rerun instead of repeatedly retrying or bypassing the
prompt.
