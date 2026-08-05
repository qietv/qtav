# QtAVCore OHOS XComponent example

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
stop, and emits PASS only after the complete lifecycle succeeds. It then opens
H.264 and HEVC through `QtAV::InteropOHCodecVulkan`, presents exactly one
output from each codec into the interop's private `OH_ConsumerSurface`, and
exercises the native-buffer capability gate.
Native OHOS HDR remains pending. The Vulkan interop retains the exact acquired
`OHNativeWindowBuffer`/`OH_NativeBuffer`, imports it through
`VK_OHOS_external_memory`, and can be called strict no-intermediate source
zero-copy only when the driver exposes an explicit `VkFormat` and plane mapping
that libplacebo wraps directly. An opaque format is rejected rather than
normalized. The OpenGL ES fallback likewise
requires raw `GL_EXT_YUV_target` sampling followed by crop-aware RGBA16F GPU
normalization before libplacebo; it is zero-CPU-copy but not strict source
zero-copy. Implicit `OH_NativeImage` external-OES YUV-to-RGB conversion
is not the target. OHCodec/NativeImage may propagate the codec PTS unchanged in
microseconds, so the interop compares the observed value and its
microsecond-to-nanosecond candidate against the exact queued-frame PTS set,
then stores and correlates the selected value in nanoseconds. This is a
correlation rule, not an additional device-validation result.

The 2026-08-06 Mate 60 Pro run acquired real H.264 and HEVC consumer buffers,
but Vulkan exposed only `VK_FORMAT_UNDEFINED` with opaque external format
`1000156003`. The harness therefore emits
`QTAV_OHOS_OHCODEC_VULKAN_RESULT UNSUPPORTED`, requires exactly two acquired
buffers and two opaque rejections, and verifies zero native imports, direct
planes, CPU maps, transfers, staging copies, uploads, and normalization passes.
The overall HAP result treats this as a validated capability rejection, not as
a texture-interoperability PASS. A device with an explicit multi-plane Vulkan
format is still required to validate direct sampling and release after GPU
completion.
The generated 440 Hz and 660 Hz tones allow a manual audibility check, while
automation validates delivery and hardware timing.

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
`-H264MediaSource`.

Run the signed result on exactly one connected device and collect the native
result marker with:

```powershell
./modern/examples/ohos/run-connected-device.ps1 `
  -ProjectRoot C:/path/to/signed-project `
  -BundleName com.example.bundle
```

Installation is attempted once. If HarmonyOS asks for device-side approval,
approve it manually and rerun instead of repeatedly retrying or bypassing the
prompt.
