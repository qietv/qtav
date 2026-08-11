# OHOS strict Vulkan multi-plane research checkpoint

Date: 2026-08-10

This checkpoint starts the active PLAN gate and records its first signed-device
rerun without claiming a strict-device pass. The required completion remains a
real OHCodec decoder buffer for which
`vkGetNativeBufferPropertiesOHOS` reports a non-opaque sampled multi-plane
`VkFormat`, followed by direct libplacebo plane wrapping, precision validation,
and release only after GPU completion.

## Connected device identity and deployment

HDC queries reported:

- device: HUAWEI Mate 60 Pro, `ALN-AL80`;
- system: `ALN-AL80 6.1.0.135(SP8C00E120R4P11)`;
- OpenHarmony: `OpenHarmony-6.1.1.120`, API 24;
- ABI: `arm64-v8a`;
- kernel: HongMeng 1.12.0, 2026-07-06 build.

This is the same device family as the earlier opaque-format evidence, not a new
explicit-format-capable candidate. After the user configured DevEco automatic
signing, the staged product `default` already referenced signing configuration
`default`; its certificate, profile, and keystore paths existed under the
current Windows profile, so no project signing edit was required.

## Research instrumentation

- Preserved the existing production-default opaque external-format workaround.
- Kept general Vulkan source-path success separate from
  `strictExplicitPlane=PASS`; forced-format, workaround, and opaque-normalized
  modes now report the strict result as `GATED`.
- Added an additive `nativeBufferObservation()` API while preserving the
  existing statistics structure layout. It records native dimensions, stride,
  usage, color-space query/result, Vulkan format features, physical-device
  optimal-tiling features, allocation size, and memory-type bits.
- Required an explicit direct-plane candidate to match the OH native raw plane
  order. A YCrCb/VU buffer is no longer accepted as a YCbCr/UV libplacebo
  source solely because the format family and bit depth resemble each other.
- Marked native modifier and vendor compression as `not-exposed`; the public
  OHOS NativeBuffer and `VK_OHOS_external_memory` structures available in API
  24 do not provide those fields.
- Kept decoded-source CPU map, software transfer, staging, upload, and source
  normalization counters in the connected marker. The strict path must keep
  all five at zero and retire NativeBuffers after renderer GPU completion.

## Offline validation

The repository `arm64-ohos-23-static` dependency prefix passed
`cmake/verify-install.cmake`. With the DevEco OHOS Clang 15 toolchain:

- the Shared OHOS configuration rebuilt the interop and XComponent
  `libentry.so` successfully;
- the Static OHOS configuration rebuilt the interop and linked the XComponent
  `libentry.so` successfully;
- Shared and Static install trees were refreshed, and the independent OHOS
  package consumer compiled and linked the additive observation API from each
  installed package;
- the standalone `nativebuffer-vulkan-probe` configured against repository
  libplacebo 7.351.0 and compiled with `-Werror`.

The retained full OHOS CI driver was also started with dependencies skipped.
Its five-minute outer command limit expired during the Static configuration's
large batch of AArch64 test-executable links. Those binaries cannot execute on
Windows and were outside this interop checkpoint, so only that orphaned
`build/ci/ohos` CMake/Ninja/Clang/LLD process group was stopped. The required
Static `libentry`, install, and package-consumer targets were then completed
individually. This record does not claim that the interrupted all-test-target
cross-link batch passed.

The staged HAP contains a six-second H.264 High/yuv420p control and a 6.2-second
3840x2160 HEVC Main 10/yuv420p10le BT.2020/PQ fixture. Hvigor completed native
library staging, resources, ArkTS compilation, and `PackageHap`, producing:

```text
C:\vscode\qtav\build\ohos-hap-strict-vulkan-20260810\entry\build\default\outputs\default\entry-default-unsigned.hap
size: 36866112 bytes
SHA-256: 13EB9C56B035A619E99D4BBA1EB9A2371FD06C36D14CE84F68D9B6BCADFA2282
```

Following that user authorization, Hvigor completed `SignHap` and produced:

```text
C:\vscode\qtav\build\ohos-hap-strict-vulkan-20260810\entry\build\default\outputs\default\entry-default-signed.hap
size: 37229460 bytes
SHA-256: DCBC6122C08F98C910822416ED61E57123A2ED262F1832D7A8B3254051FC2D59
```

The signed HAP installed successfully on the USB-connected ALN-AL80 and its
existing H.264/NV12 plus HEVC Main10/P010 lifecycle completed with
`QTAV_OHOS_RESULT PASS`. The component markers also passed for OHCodec direct
surface lifecycle, OpenGL ES native-image interop, Vulkan NativeBuffer interop,
Vulkan-to-OpenGL ES native fallback, and Vulkan-to-software-decode fallback.

## Connected strict Vulkan evidence

The real OHCodec Vulkan marker reported general path success but correctly
kept the strict result gated:

```text
QTAV_OHOS_OHCODEC_VULKAN_RESULT PASS
mode=external-format-workaround strictExplicitPlane=GATED
acquired=60 imported=60 directPlanes=0 opaqueImports=0 workaroundImports=60
forcedVkFormatImports=0 forcedNativeSamples=0 forcedLibplacebo=0
releasedAfterGpu=59 exactQueueMax=1 acquireFences=0
nativeFormat=35 nativeSize=3840x2160 nativeStride=7680 nativeUsage=34312
nativeColorSpace=10 nativeColorSpaceResult=0
vkFormat=0 forcedVkFormat=1000156013 externalFormat=1000156013
formatFeatures=8785921 optimalFeatures=13094913
allocationSize=25276416 memoryTypeBits=2
nativeModifier=not-exposed nativeCompression=not-exposed
normalization=60 cpuMap=0 transfer=0 staging=0 upload=0
```

The 59 releases for 60 imports are bounded by the observed single in-flight
image and the marker's GPU-completion accounting. However, the driver still
returned `VK_FORMAT_UNDEFINED`, every import used the external-format
workaround, and all 60 sources required normalization. Therefore this run does
not provide explicit Y/UV plane wrapping or raw P010 10-bit precision proof.
The strict multi-plane gate remains open and needs hardware/driver behavior
that exposes a real sampled multi-plane `VkFormat`.
