# QtAVCore OHOS player demo

The user-facing OHOS player is separate from the automated XComponent
validation harness in this directory. Its ArkUI template is under
[`player-hap/`](player-hap/), and its native N-API bridge is
[`native/ohos_player_napi.cpp`](native/ohos_player_napi.cpp).

## Features

- local document selection without copying the selected media into app
  storage: ArkUI opens the document URI, native code retains a duplicated file
  descriptor, and FFmpeg opens its `/proc/self/fd/<fd>` view;
- direct FFmpeg URL playback, including protocols enabled in the repository
  OHOS FFmpeg package, with the normal QtAVCore buffering and network-recovery
  policies;
- play, pause, stop, accurate seek, ten-second skip, and pitch-preserving
  0.5x through 2.0x playback presets;
- post-load audio-track and text/ASS subtitle-track switching;
- presentation-timed plain-text subtitle overlay;
- runtime-selectable automatic, required OHCodec hardware, or FFmpeg software
  decode, combined with automatic, Vulkan-only, or OpenGL ES-only rendering;
  the diagnostic software path fixes `avcodec.video.threads=4` and logs the
  actual FFmpeg thread configuration after decoder open;
- automatic Vulkan-preferred startup with OHCodec native-buffer interop,
  one-way OHCodec surface rebind to OpenGL ES, then software-decode fallback;
- native HDR preference/requirement plus an explicit SDR tone-map mode. The
  information panel reports input HDR/Dolby Vision, the negotiated output
  color space and format, and whether HDR was tone-mapped to SDR. The ArkUI
  video host is `XComponentType.SURFACE`; its HDR-brightness hint follows the
  native renderer policy while Vulkan/EGL configure the same
  `OHNativeWindow` as a video surface with a verified 10-bit BT.2020/PQ or
  BT.2020/HLG target and per-frame HDR metadata;
- OHAudio device output through libswresample and the optional FFmpeg `atempo`
  time stretcher;
- normal and landscape full-screen layouts using one persistent XComponent;
  switching layout is a presentation-only resize, while the OHOS Vulkan
  adapter owns WSI orientation and both backends preserve ordinary `Fit`
  geometry without rebinding the decoder;
- system picture-in-picture using the same ArkUI `XComponentController`, with
  play/pause and ten-second seek actions in the PiP control panel plus optional
  automatic PiP when returning home and an explicit stop/restore control in
  the main player;
- a closeable YouTube-style upper-left information panel showing source,
  resolution, active codecs/tracks, renderer, state, playback rate, successful-
  presentation FPS, decoded/presented frame totals, and Player queue/late
  drops. The information panel is refreshed at 1 Hz only while visible;
  progress and subtitle state use a separate 250 ms lightweight snapshot,
  and stable Select option arrays are not rebuilt on progress ticks.

QtAVCore currently exposes decoded plain text for text and ASS/SSA subtitles.
Bitmap subtitle rectangles are not rendered by this demo. Hardware decoding,
hardware-frame interop, rendering, and ArkUI surface ownership remain separate
objects even though the page exposes them as three concise selectors. `Auto`
is the production-oriented path; forced modes are diagnostic cells and fail
instead of silently substituting a backend that the user explicitly excluded.

## Build without deploying

A clean source template intentionally contains no committed signing material.
Build QtAVCore, stage the required arm64 shared libraries, compile ArkTS, and
package the HAP with:

```powershell
./modern/examples/ohos/build-ohos-player-hap.ps1
```

Without a local signing configuration the expected output is:

```text
modern/examples/ohos/player-hap/entry/build/default/outputs/default/
  entry-default-unsigned.hap
```

The script never calls HDC and never installs or launches the application.
It also stages the OHOS arm64 `libc++_shared.so` required by the audio
time-stretch backend; omitting that runtime prevents the XComponent native
module from loading before N-API initialization.

DevEco automatic signing may add a local `default` signing configuration to
the template and then produce `entry-default-signed.hap`. Keep certificate,
profile, keystore paths, and encoded passwords local; do not commit them.

## Signing boundary

Configure signing in a DevEco project first. Then synchronize the demo sources
and native libraries into that project while preserving its root signing
profile:

```powershell
./modern/examples/ohos/build-ohos-player-hap.ps1 `
  -ProjectRoot C:/path/to/signed-project
```

Before the first installation, confirm the generated HAP filename is signed,
unlock the device, and explicitly approve deployment. Do not install an
unsigned template HAP and do not repeatedly retry a device-side authorization
failure.

## Manual acceptance matrix after signing

1. Open a seekable local MP4 or MKV through the document picker; verify seek,
   pause/resume, stop, audio output, and surface recreation.
2. Open an HTTP and an HTTPS media URL; verify buffering/recovery status and
   certificate validation for the deployment environment.
3. With a multi-track fixture, switch every audio track and verify the selected
   track ID plus audible output. Switch each text/ASS subtitle track, disable
   subtitles, and verify old cues clear across switch and seek. A subtitle-only
   switch must not publish `Buffering`, reopen/flush audio, or make video PTS
   move backward.
4. Exercise every rate preset and verify audio remains pitch-preserved and A/V
   synchronized. With `legend.mkv` at 0.5x, enter and exit full screen at least
   three times; decoded/presented and native release/callback counters must
   continue advancing after every transition.
5. Enter and exit landscape full screen, then start/restore PiP. Exercise PiP
   play/pause and both ten-second seek controls. Verify the progress value does
   not restart, video remains upright, and a 16:9 source is not stretched in
   either landscape full screen or PiP.
6. Close and reopen the media-information panel. Record successful-presentation
   FPS, decoded/presented counters, queue/late drops, renderer selection, and
   any OHAudio or network event.
7. Run `legend.mkv` and `wednesday.mp4` through auto, forced software, forced
   OHCodec, Vulkan-only, and OpenGL ES-only cells. Compare FPS and drops with
   the information panel closed and open; progress ticks must not repeatedly
   relayout the track/rate selectors.
8. For HDR input, record `hdrInput`, `hdrOutput`, output color space/format,
   and `toneMappedToSdr`. `Require HDR` must fail rather than quietly creating
   an SDR surface; explicit SDR mode must report the tone-map.
9. With OHCodec/Vulkan, seek forward and then backward repeatedly in both
   `legend.mkv` and `wednesday.mp4`. Successful-presentation FPS and native
   buffer acquire/import/release counters must resume after every seek while
   audio continues; a stale image with advancing audio is a failure.

## Connected-device record

The 2026-08-11 signed-device run verified the HAP signing block and all fourteen
native libraries, then exercised local `legend.mkv` and `wednesday.mp4` through
the document picker. The retained descriptor survived live decoder/renderer
changes. `legend.mkv` used OHCodec/Vulkan at 25.0 FPS with 489 decoded hardware
frames, 484 successful presentations, and zero Player queue/late drops. Forced
OHCodec/OpenGL ES also held 25 FPS. Its diagnostic software cells reached
roughly 11--18 FPS through Vulkan and 6 FPS through OpenGL ES at 3840x2160;
these CPU-heavy modes are functional fallbacks, not the production default.
`wednesday.mp4` exercised the automatic Dolby Vision Vulkan-to-OpenGL ES
surface rebind at 24.0 FPS with 1,564 hardware frames, 1,560 successful
presentations, and zero drops.

Opening and closing the information panel did not change the 24.0 FPS result
or introduce drops. Separate 20-second observations reported zero AceMenu
relayouts and zero `ProcessJank` events. The page therefore keeps the 250 ms
progress update local to the slider/text state instead of rebuilding debug and
Select trees on every tick.

The 2026-08-13 signed-player follow-up reproduced the former 0.5x full-screen
freeze as one queued OHCodec output with no possible matching native-image
callback. After the shared one-shot output decision was made observable as
`AlreadyDecided`, the final no-instrumentation HAP passed the original
`legend.mkv -> 0.5x -> full screen` path and three further exit/re-enter
cycles. Each snapshot held 12.4 FPS and zero Player drops; the last snapshot
reached 1,980 hardware frames, 1,961 presentations, and balanced
`release=callback=1,963`.

A software-decode follow-up rebuilt the repository OHOS arm64 FFmpeg package
with LTO, NEON, runtime CPU detection, and effective `-O3` instead of the
`-Oz` selected by the retained small-build policy. The demo requested four
threads; `decoder.software.configuration` confirmed `threads=4` and frame
threading, and the process exposed exactly `av:hevc:df0` through `df3`.
`legend.mkv` then decoded 4,204 software frames by position 2:49, an average of
24.9 FPS, while the rolling successful-presentation rate reported 25.0 FPS.
The same snapshot contained 3,671 successful presentations and 28 Player
queue/late drops, so the run proves sustained decode throughput but does not
claim that every decoded frame reached the swapchain.

During a ten-second `hiperf` window, the four HEVC workers accounted for 91.6%
of sampled CPU cycles. The ArkUI process accounted for 0.56%, Ace layout code
for 0.12%, the slider library for 0.02%, and Skia for 0.01%. HEVC residual
coding and 10-bit NEON loop filters were the leading named functions. The
information panel, progress slider, and subtitle/bottom text are therefore not
the primary software-playback bottleneck. The device started near 32.7 degrees
C at `system_h` and reported 37.2 degrees C after the sustained run, below the
temperature reached by the earlier automatic-thread/`-Oz` observation.

The follow-up native-HDR run used the same `XComponentType.SURFACE` and proved
both application-rendered output paths. `legend.mkv` selected OHCodec/Vulkan,
`VK_FORMAT_A2B10G10R10_UNORM_PACK32` (`VkFormat 64`), and BT.2020/PQ at
25.1 FPS with 3,460 hardware frames, 3,434 successful presentations, and zero
Player drops. Forced OpenGL ES selected exact `RGB10_A2` plus BT.2020/PQ and
held 25 FPS; both renderers also passed `Require HDR`, so neither silently
fell back to SDR. RenderService emitted `HiHdrAlgo ... enter` while those
surfaces were active. Explicit SDR selected Vulkan RGBA8/sRGB (`VkFormat 37`),
set the XComponent HDR brightness hint to zero, and reported HDR-to-SDR tone
mapping.

`wednesday.mp4` then held 24.0 FPS with OHCodec/OpenGL ES, 329 hardware frames,
323 successful presentations, and zero drops. The input was recognized as
Dolby Vision and the target remained RGB10_A2 BT.2020/PQ. This is a native HDR
carrier for libplacebo's residual-disabled Dolby Vision/base-layer processing;
it is not Dolby Vision dynamic-metadata passthrough, enhancement-layer
reconstruction, licensing, or certification.

The 2026-08-12 Huawei-contract update removed that automatic fallback for the
working case. The production default now keeps `externalFormat` opaque and
uses a second `RGB_IDENTITY` Vulkan sampler for raw Dolby Vision components;
numeric explicit-format guessing defaults off. The driver component mapping is
kept unchanged; the normalization shader applies Vulkan's raw `(Cr,Y,Cb)` to
`(Y,Cb,Cr)` `.gbr` reorder only for the identity sampler, while ordinary HDR's
suggested RGB conversion remains `.rgb`. Forced-SDR captures of both files
matched the established OpenGL ES color control. On the same Mate 60 Pro,
`legend.mkv` held OHCodec/Vulkan at 25.0 FPS and `wednesday.mp4` held 24.1 FPS,
both with zero Player drops. The final same-process overlay reported 1,988
opaque imports, normalization passes, consumer-buffer releases, and frame
callbacks, `workaround=0`, and source `VkFormat 0`.

The Vulkan path uses a bounded OHOS WSI compatibility rule. The platform
swapchain layer supports A2B10G10R10 plus BT.2020/PQ at creation time on the
tested system even though its surface-format query omits the HDR color-space
pair. QtAVCore attempts that pair only when the swapchain-colorspace extension
was enabled, verifies the matching NativeWindow state, fails closed in
`Require HDR`, and retains a normal SDR retry in `Prefer HDR`. OpenGL ES uses
the exact advertised RGB10_A2 EGL configuration and treats the verified
NativeWindow color space as authoritative when the driver reports the default
EGL surface color space back.

Video remained visible throughout the test. The black ArkUI background sits
behind the `SURFACE` XComponent and provides letterbox color for `Fit`; it did
not cover the hardware-decoded layer. A system screenshot is useful for
checking geometry and overlays but is composited/captured as SDR, so its pixel
brightness is not accepted as HDR evidence.

The initial 2026-08-12 full-screen regression used device-local `legend.mkv`
on the connected ALN-AL80 and proved that one persistent XComponent removed
the media restart. A follow-up root-cause audit superseded its demo-level
Vulkan/GLES rotation compensation: a same-window resize now keeps the selector
candidate and OHCodec interop surface generation, Vulkan owns its WSI surface
transform, and the demo always requests ordinary `Fit` geometry. The original
PiP observation remains useful: its 1182x665 surface displayed as 672x378 with
no size distortion. The final signed-device acceptance then repeated five
full-screen/restore cycles without restart, backward movement, stop, or aspect
ratio drift.

The same run also exercised HTTPS H.264/AAC playback through Vulkan,
successful-presentation FPS, 1.5x rate, ten-second skip, landscape full screen,
live desktop PiP and explicit PiP stop. A generated seekable two-AAC/SubRip MKV
changed the active audio track from 1 to 2 and the subtitle track from 3 to -1
and back to 3 while loaded; the re-enabled Chinese subtitle was visible over
the video. Audible pitch/track identity remains a manual user confirmation.
