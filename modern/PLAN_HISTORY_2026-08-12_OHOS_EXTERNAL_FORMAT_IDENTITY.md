# OHOS opaque external-format identity sampling

Date: 2026-08-12

## Scope

This record freezes the correction to AD-009 after Huawei's formal response,
the production `VkExternalFormatOHOS` implementation, and the connected Mate
60 Pro validation with HDR10 and Dolby Vision Profile 5 media. This completes
AD-009 for the opaque external-format production policy. It does not close the
separate strict explicit-plane/no-intermediate Vulkan gate.

## Vendor contract

Huawei confirmed the following for Maleoon 910 on HarmonyOS 6.1.0.135:

- `VkNativeBufferFormatPropertiesOHOS::format` is
  `VK_FORMAT_UNDEFINED` when no equivalent standard Vulkan format is exposed;
  this is expected, not a failed capability query.
- `externalFormat` is the implementation-defined format identifier for that
  case. Numerical equality with a standard `VkFormat` is an internal detail
  and must not be treated as a stable contract.
- The tested driver does not return an explicit standard multi-plane format,
  and no configuration switch changes that behavior.
- Standard independent plane image views cannot be created from the opaque
  import. `VK_SAMPLER_YCBCR_MODEL_CONVERSION_RGB_IDENTITY` can expose raw
  Y/Cb/Cr samples without implicit color conversion.
- P010 import and sampling preserve the full encoded 10-bit data. The queried
  range and chroma offsets match the physical codec buffer.

## Implementation

- `OHCodecVulkanInteropConfig::externalFormatWorkaroundEnabled` now defaults
  to `false`. The closed numeric allow-list and forced-format modes remain
  diagnostic only.
- Opaque input creates the driver-suggested sampler/view for ordinary SDR/HDR
  and a second `RGB_IDENTITY` sampler/view. Both preserve the implementation-
  defined component mapping returned by the driver. Vulkan assigns raw
  `(Cr, Y, Cb)` to sampled `(R, G, B)`, so the normalization shader applies
  `.gbr` only for the identity/raw route to store `(Y, Cb, Cr)` for libplacebo;
  the suggested RGB route remains `.rgb`.
- `VulkanVideoRenderer` selects the identity pair only for frames carrying
  Dolby Vision metadata. The external normalization shader copies the encoded
  values into RGBA16F; libplacebo receives the original bit depth, range, color
  properties, and Dolby Vision metadata.
- Immutable sampler/conversion ownership is decoupled from the decoded native
  allocation. After the normalization submission completes, the opaque
  consumer buffer is returned immediately instead of waiting for a later
  redraw. This prevents the consumer-buffer pool from being exhausted before
  the next frame-available callback.
- The player information overlay exposes opaque imports, normalization passes,
  acquired/released buffers, callbacks, workaround imports, and the last
  Vulkan/external formats for device auditing.

## Build and package validation

The repository OHOS dependency prefix passed `cmake/verify-install.cmake`.
The OHOS arm64/API 23 shared renderer, interop, player N-API module, ArkTS, and
signed debug HAP built successfully. The signed package was installed only
after the user explicitly requested connected-device testing.

## Connected-device validation

Device: HUAWEI Mate 60 Pro (`ALN-AL80`), Maleoon 910, HarmonyOS
6.1.0.135 SP8.

- Forced-SDR Vulkan captures of both files were compared with the established
  OpenGL ES control. They showed normal color after the raw-only `.gbr` fix;
  this also caught and rejected two earlier counter-only candidates whose
  Dolby Vision or ordinary-HDR component order was wrong.
- `legend.mkv`: 3840x2160 HEVC/E-AC-3, OHCodec/Vulkan, HDR BT.2020/PQ,
  `VkFormat 64` output, 25.0 FPS, 1,676 presentations at the final snapshot,
  zero Player drops, and `opaque=633`, `normalization=633`,
  `released=633/633` for that HDR renderer generation.
- `wednesday.mp4`: 3840x2160 HEVC/E-AC-3, recognized Dolby Vision,
  OHCodec/Vulkan, HDR BT.2020/PQ, `VkFormat 64` output, 24.1 FPS, 730
  presentations at the final snapshot, and zero Player drops.
- The final same-process cumulative diagnostics ended at `opaque=1988`,
  `normalization=1988`, `released=1988/1988`,
  `frameAvailableCallbacks=1988`, `workaround=0`, source `VkFormat 0`, and
  `externalFormat=1000156013`.

These results validate the opaque identity route, full consumer-buffer
release loop, and Profile 5 Vulkan use on the named device. They do not claim
Dolby Vision dynamic-metadata passthrough, enhancement-layer reconstruction,
licensing, certification, or strict direct-plane source zero-copy.

## Remaining gate

Strict Vulkan completion still requires hardware which reports a sampled
explicit multi-plane `VkFormat` and passes direct libplacebo plane wrapping,
precision, synchronization, and GPU-release validation with no normalization
image. The tested Huawei driver cannot provide that interface.
