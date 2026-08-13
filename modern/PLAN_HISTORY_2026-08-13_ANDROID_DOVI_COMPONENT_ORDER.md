# Android Vulkan Dolby Vision component-order repair

Date: 2026-08-13

## Scope and root cause

This repair covers the Android `MediaCodec + AImageReader/AHardwareBuffer +
Vulkan opaque external format + Dolby Vision` route. It does not change direct
Surface presentation, OpenGL ES raw sampling, software frames, or explicit
multi-plane Vulkan input.

The Android interop created an `RGB_IDENTITY` YCbCr conversion and pre-rotated
the driver-provided component mapping so its sampled result was already in
`(Y,Cb,Cr)` order. The shared external normalization shader later gained a raw
path for OHOS that converts Vulkan's sampled `(Cr,Y,Cb)` convention with
`.gbr`. Android therefore applied the rotation twice and delivered
`(Cb,Cr,Y)` to libplacebo, which is consistent with the reported strong Profile
5 color error. OHOS retained the driver mapping and needed the shared `.gbr`,
so its current behavior was correct.

## Repair

- Added an internal platform-neutral identity-sampler contract used by both
  MediaCodec/Vulkan and OHCodec/Vulkan.
- Android now retains `samplerYcbcrConversionComponents` for `RGB_IDENTITY`.
  The shared shader performs the only `.gbr` rotation into libplacebo's
  `(Y,Cb,Cr)` input order.
- OHOS calls the same identity helper, which returns its existing driver mapping
  unchanged. The shared shader itself was not modified.
- Updated the backend contract and Android/OHOS architecture documentation.
  No public layout, virtual ABI, core dependency, or installed target changed.

## Deterministic regression

`qtav_vulkan_raw_ycbcr_contract` uses a nontrivial driver component mapping and
requires both platform identities to retain it. It verifies that both platform
call sites use the shared helper, rejects the former Android `explicitSwizzle`,
checks the raw sample permutation `(Cr,Y,Cb) -> (Y,Cb,Cr)`, and confirms the
compiled source contract still uses the shared shader's `sampleValue.gbr` path.

Before the production repair, the test failed with:

```text
Android raw identity sampler changed the driver component mapping
```

After the repair, the same test passes.

## Validation

- Windows Visual Studio 2026 shared and static Release builds: passed.
- Windows shared Release CTest: 61/62 passed. The new raw-YCbCr contract passed.
- Windows static Release CTest: 61/62 passed. The new raw-YCbCr contract passed.
- The one failure in both Windows configurations is the unrelated
  `qtav_subtitle_libass_render` assertion
  `primaryGeneration != alternateGeneration`; three isolated shared retries
  failed identically. No subtitle code was changed in this task.
- Android arm64/API 28 shared and static targets
  `qtav_interop_mediacodec_vulkan` and
  `qtav_vulkan_raw_ycbcr_contract_test`: cross-build passed against the verified
  repository dependency prefix.
- OHOS arm64/API 23 shared and static target
  `qtav_interop_ohcodec_vulkan`: cross-build passed against the verified
  repository dependency prefix. This compiles the unchanged OHOS identity
  behavior through the shared helper.
- Android connected-device A/B on Xiaomi 2410DPN6CC (`haotian`), Android
  16/API 36, arm64-v8a, Adreno 830: passed with the real residual-disabled
  Profile 5 `wednesday.mp4`.
  - The installed pre-repair player selected
    `MediaCodec -> AImageReader -> Vulkan ZeroCopy -> libplacebo`, received
    RPU metadata and raw YCbCr imports, and reproduced the reported strong
    purple/yellow picture.
  - The repaired, signed audit APK selected the same Vulkan/HDR route. A
    close-up from the same opening sequence had natural skin and eye color,
    with no purple cast.
  - The paused acceptance checkpoint reported 582 callbacks/RPU frames, 578
    presented and `578/578/578` queued/acquired/imported frames, 17 persistent
    AHardwareBuffer/raw-YCbCr imports, zero queue/late drops, zero stale images,
    and maximum pending depth one.
  - `lastVulkanFormat=VK_FORMAT_UNDEFINED` and `lastExternalFormat=654`
    confirmed the opaque external-format route. All 578 imported images
    returned release fences, there were zero release-fence fallbacks, and the
    CPU map/software transfer/staging copy/renderer upload counters were
    `0/0/0/0`.
  - Android reported the output layer as HDR and its Dolby Vision display mode
    became active. The application requested and presented at 23.976/24 fps.
  - An OpenGL ES raw-YCbCr/libplacebo control at the same `00:24` face shot
    reported 581 RPU callbacks, 579 raw imports/presentations, zero queue/late
    drops, and the same natural skin, black-hair, and neutral-eye color as the
    repaired Vulkan capture.
  - After the A/B gate, the final same fixed native library was signed as the
    normal `org.qtav.core.player` version-code 3 APK and overwrite-installed
    without clearing application data. Its SHA-256 is
    `0227DA14CDFE052A2A2FD81BB5806A87BE210CBF2A0BC1204C1FE392586E6623`.

This closes the Android Profile 5 post-fix color gate. OHOS runtime testing was
not repeated because no OHOS device was requested or connected; its production
identity mapping and the shared `.gbr` shader are unchanged, and both OHOS
cross-build variants compile that shared contract.
