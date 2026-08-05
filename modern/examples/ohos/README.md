# QtAVCore OHOS XComponent example

This example is the OHOS production-path shell. ArkUI owns one `XComponent`;
its native lifecycle publishes `OHNativeWindow` generations to
`QtAV::RenderVulkanOHOS` and `QtAV::RenderOpenGLOHOS` through the shared
`QtAV::RenderMobile` selector. The native adapters own their OHOS
surface/context/swapchain resources while the platform-neutral Vulkan and
OpenGL ES engines render decoded software frames.

The native module copies a small packaged MPEG-4/AAC test clip into app storage
and opens it once through `qtav::Player`. AAC is converted to negotiated 48 kHz
Float32 PCM through `QtAV::AudioResample` and presented through
`QtAV::AudioOHAudio`. Its first selector session deliberately
makes Vulkan unavailable and requires 20 OpenGL ES presentations. A new
renderer session then selects Vulkan, injects a fatal result after 12 presented
frames, and requires 30 more frames through the selector's one-way OpenGL ES
fallback. Between the two renderer sessions the harness pauses playback, seeks,
and resumes. The one-second clip loops so the final marker can additionally
require OHAudio callback delivery, a valid hardware presentation clock,
non-negative latency, successful flush, and segment-end drain while retaining
exactly one media open. The harness does not use OHCodec, native-buffer
interop, or native OHOS HDR yet. The generated 440 Hz tone allows a manual
audibility check, while automation validates delivery and hardware timing.

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
