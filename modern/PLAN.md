# QtAVCore implementation plan

Last updated: 2026-08-12

This is the active, executable plan for the Qt-free rewrite. It contains only
current status, task ordering, incomplete gates, and the acceptance criteria
needed to choose or finish the next task. The frozen pre-cleanup plan, including
completed checklists, investigation narratives, device matrices, and detailed
validation output, is preserved in
[`PLAN_HISTORY_2026-08-10.md`](PLAN_HISTORY_2026-08-10.md).

Document ownership is intentionally separated:

- [`PLAN.md`](PLAN.md): current work, ordering, incomplete gates, and concise
  completion summaries;
- [`ARCHITECTURE.md`](ARCHITECTURE.md): the current component, ownership,
  threading, lifetime, and data-flow model;
- [`DECISIONS.md`](DECISIONS.md): durable choices, rejected alternatives, and
  their consequences;
- [`CI.md`](CI.md): supported-target workflow ownership, runner/toolchain
  contract, cache boundary, local reproduction, and device-only exclusions;
- [`README.md`](README.md): public build, API, and backend usage;
- [`MIGRATION.md`](MIGRATION.md): migration from legacy QtAV.

Status legend:

- `[~]` implemented partially, externally tracked, or blocked on a named gate;
- `[ ]` not implemented or not yet validated;
- completed work is summarized without reproducing its historical checklist.

## Target support and binding direction

QtAVCore supports Windows, Android, and OHOS only. macOS/iOS are archived under
[`../archived_apple/`](../archived_apple/), and Linux is outside the active
target matrix. Supported-target FFmpeg and transitive dependencies come from
the repository [`../ffmpeg/`](../ffmpeg/) vcpkg subproject.

The following accepted decisions constrain all remaining work:

- `modern/` remains Qt-free and exposes no FFmpeg or platform SDK types from
  core public headers;
- Windows remains D3D11-only; AD-012 rejected a production Windows Vulkan
  backend;
- hardware decode, native-frame interop, rendering, audio output, and native
  window ownership remain separate responsibilities;
- libplacebo remains the semantic color, Dolby Vision, tone/gamut, scaling,
  and output-encoding authority for D3D11, Vulkan, and OpenGL ES;
- zero-CPU-copy and strict no-intermediate source zero-copy retain the narrower
  definitions in [`ARCHITECTURE.md`](ARCHITECTURE.md);
- backends remain optional compile-time targets in this repository until a
  stable runtime boundary is justified; see AD-019;
- packet buffering, temporary disk spill, live-latency dropping, and network
  recovery retain the separate policies in AD-013 through AD-016.

## Current baseline

- The Qt-free core, portable reference backends, installable CMake package,
  asynchronous worker model, track/subtitle/external-source support, bounded
  packet buffering, network recovery, frame stepping, and accurate seek are
  implemented and verified.
- Windows D3D11/D3D11VA/WASAPI is the complete Windows production path. AD-010
  is closed: visible-region GPU copy is the default decoder-surface policy and
  direct decoder-texture sampling is explicit opt-in. The Intel post-seek
  investigation is complete: ETW localized redundant decoder/output-view
  teardown, the repository FFmpeg/QtAVCore reuse repair removed the persistent
  stall, and the broader zero-transient follow-up is no longer required.
  Detailed evidence remains in the frozen plan history.
- Android Vulkan/OpenGL ES/MediaCodec/AAudio, application-rendered raw-component
  paths, direct-Surface mode, and one-way renderer fallback are complete and
  retained as the mobile regression baseline.
- OHOS Vulkan/OpenGL ES software presentation, mobile selection/fallback,
  OHAudio, OHCodec H.264/HEVC, capability-gated VVC/H.266, direct-surface
  lifecycle, raw OpenGL ES interop, and standards-based opaque external-format
  Vulkan sampling are implemented and connected-device validated. Ordinary
  HDR uses the driver-suggested conversion; Dolby Vision uses `RGB_IDENTITY`
  raw-component sampling with P010 precision/range/chroma covered by Huawei's
  formal reply. Numeric external-format reinterpretation is diagnostic only
  and defaults off. Strict direct multi-plane/no-intermediate Vulkan wrapping
  remains device-gated.
- The separate OHOS user player demo now implements local-document and URL
  opening, full screen, pitch-preserving rates, audio/subtitle switching,
  text-subtitle presentation, system PiP controls, selectable software/OHCodec
  decode and Vulkan/OpenGL ES rendering, explicit HDR output policy and
  diagnostics, a closeable 1 Hz media/FPS overlay, and an isolated 250 ms
  progress/subtitle snapshot that does not rebuild track controls. The new
  native matrix, OHOS HDR color-space negotiation, ArkTS, and signed debug HAP
  cross-build pass. The updated signed HAP passed package verification and
  connected-device deployment: local `legend.mkv`/`wednesday.mp4`, the full
  soft/hard decode and Vulkan/OpenGL ES diagnostic matrix, live switching,
  HDR capability reporting, debug-overlay redraw comparison, Vulkan URL
  playback, successful-presentation FPS, rate/seek/skip, landscape full
  screen, live desktop PiP, dual-audio switching, subtitle disable/re-enable,
  and the document picker were exercised. A follow-up
  `XComponentType.SURFACE` run then passed native HDR through both renderers:
  Vulkan selected A2B10G10R10 BT.2020/PQ and OpenGL ES selected exact
  RGB10_A2 BT.2020/PQ, both passed required-HDR policy, and RenderService
  reported its HDR composition algorithm. Explicit SDR still selected
  RGBA8/sRGB and reported tone mapping. Subjective audible pitch/track
  confirmation remains a user manual check.
- The OHCodec/Vulkan deferred-frame freeze root cause is repaired in C++.
  Player now retains the exact backend-deferred frame per renderer key and
  automatically invalidates pending producer associations at every
  presentation-generation boundary, then completes decoder-cancelled
  associations after synchronous flush without joining native render calls.
  The OHCodec consumer path drains invalidated/orphaned buffers before
  admitting a new frame. Native redraw callbacks no longer enter renderer
  state locks held across producer calls, and an early callback is coalesced to
  exactly one redraw per OHCodec output. The repair adds no CPU copy, renderer
  fallback, caller-side flush, per-frame wait, or GPU-wide wait. Deterministic
  playback coverage, OHOS arm64/API 23 shared/static cross-builds, the player
  native link, and signed HAP packaging pass. The overwrite-installed HAP then
  passed cold-start, forward-seek, and backward-seek cells on both `legend.mkv`
  and `wednesday.mp4` at 24--25 FPS with balanced native release/callback
  counts; eight additional Wednesday forward/backward pairs continued decoding
  and presenting without a seek timeout, thread-block watchdog, or crash. The
  cold cells did not show `当前媒体不可定位`; see
  [`PLAN_HISTORY_2026-08-12_OHOS_DEFERRED_RENDER_SEEK.md`](PLAN_HISTORY_2026-08-12_OHOS_DEFERRED_RENDER_SEEK.md).
- The OHOS software-decode follow-up now keeps FFmpeg LTO/NEON and the shared
  small-build policy while overriding the effective arm64 optimization from
  `-Oz` to `-O3`. The demo requests four software-video decoder threads and
  reports the actual FFmpeg configuration. Connected `legend.mkv` playback
  sustained the 25 FPS decode cadence through 2:49; profiling assigned 91.6%
  of sampled cycles to the four HEVC workers and less than 1% to the ArkUI
  process, ruling out the information panel/progress text as the primary
  bottleneck. Successful-presentation totals still trail decoded totals, so
  this result is recorded as a decode-throughput repair rather than a claim of
  zero end-to-end frame loss.
- The OpenGL ES follow-up keeps software Y/Cb/Cr planar when a high-bit-depth
  upload needs libswscale normalization, so Dolby Vision metadata is no longer
  applied to already converted RGB components. The raw OHCodec external-image
  normalization plane is also marked vertically flipped at the libplacebo
  input boundary. OHOS shared/static builds, the signed player HAP package,
  and the Android native regression harness cross-build pass. The signed HAP
  was then installed with explicit approval on the connected ALN-AL80.
  `wednesday.mp4` passed software/OpenGL ES with correct Dolby Vision color and
  orientation, OHCodec/OpenGL ES passed upright at 24 FPS with zero Player
  drops, and software/Vulkan provided a normal-color control. `legend.mkv`
  passed both software/OpenGL ES at 24.4 FPS and, after a clean pipeline
  rebuild, OHCodec/OpenGL ES upright at 25.1 FPS. Six rapid consecutive seeks
  could still transiently retire the forced OHCodec/OpenGL ES renderer until a
  pipeline rebuild; that lifecycle observation is separate from the completed
  color and orientation repair.
- Dolby AC-3, E-AC-3, and TrueHD software decoding is decoded-PCM support only;
  it is not compressed passthrough, Atmos rendering, licensing, or
  certification.
- Pitch-preserving playback-rate audio is implemented as an optional
  post-conversion `AudioTimeStretcher` stage. Windows static/shared tests,
  installed-package consumption, WinUI integration, and OHOS static/shared
  cross-builds pass. Android arm64/API 28 static/shared cross-builds also pass
  against the locally built and verified repository dependency prefix.
- General processing is implemented through optional Qt-free
  `AudioFrameProcessor` and `VideoFrameProcessor` contracts. The bounded audio
  stage supports buffered zero-or-more output before the sink; the synchronous
  video stage supports explicit format/frame bypass before ordinary
  presentation. The optional `QtAV::AudioFilter` volume backend, deterministic
  lifecycle/failure tests, and Windows/Android/OHOS static/shared package
  consumption pass.
- QtAVCore 2.0.0 is formalized as the first rewrite release. A generated public
  version header, unconditional package variables, same-major discovery rules,
  coherent shared-target metadata, deterministic version probes, and separate
  installed-package consumers pass on Windows, Android, and OHOS. Compatibility
  remains scoped to documented C++/CMake boundaries and does not create a
  plugin ABI.
- Supported-target CI drivers are implemented and local reproduction passes
  Windows shared/static Release CTest (60/60 each) plus Android arm64/API 28
  and OHOS arm64/API 23 shared/static build, install, and installed-package
  consumers. The first published Actions run exposed runner-environment
  failures. By project direction, Actions now temporarily runs only the
  Windows gate; Android/OHOS jobs are suspended without changing target
  support or their retained local validation. Windows-only Actions run
  `31383223536` completed the shared/static Release tests, installs, and
  installed-package consumers without failure at commit `7e81a0f1`; the
  non-interactive Advanced Color cases were reported as skips rather than
  native display passes.

Detailed implementation and validation evidence for the version contract is in
[`PLAN_HISTORY_2026-08-10_VERSION_CONTRACT.md`](PLAN_HISTORY_2026-08-10_VERSION_CONTRACT.md).
The CI implementation and local validation record is in
[`PLAN_HISTORY_2026-08-10_CI.md`](PLAN_HISTORY_2026-08-10_CI.md).
The first published run and temporary Windows-only scope are recorded in
[`PLAN_HISTORY_2026-08-10_WINDOWS_ONLY_CI.md`](PLAN_HISTORY_2026-08-10_WINDOWS_ONLY_CI.md).

## Next task — Android MediaCodec seek-generation isolation

The OHOS deferred-render investigation exposed a related Android ownership
gap. Android does not have the same one-buffer `OH_ConsumerSurface` deadlock,
but MediaCodec/AImageReader producer callbacks can outlive a seek generation.
The Vulkan path now receives Player invalidation but can retain a late old-epoch
image or leave its bounded image wait asleep. The OpenGL ES path has an interop
`flush()` but no automatic `VideoRenderAPI::invalidatePendingFrames()`
propagation, so the shipped example currently performs a correctness-critical
manual flush before seek. Fix these boundaries in C++; applications must not
track pending frames or flush renderer internals around Player controls.

- [ ] Forward Player presentation-generation invalidation through
  `OpenGLVideoRenderer` and the Android OpenGL adapter to
  `OpenGLHardwareFrameInterop`, with a default no-op virtual for compatible
  third-party implementations and a MediaCodec implementation that invalidates
  pending producer associations.
- [ ] Give both MediaCodec Vulkan and OpenGL AImageReader interops an internal
  producer epoch. Outputs released before invalidation must remain represented
  by bounded invalidated association records until their late AImages are
  acquired and discarded; an image may not enter the current correlation set
  using timestamp proximity alone when its producer epoch is unproven.
- [ ] Make invalidation wake Vulkan's bounded exact-image wait immediately and
  close the render/invalidation race without waiting for GPU completion.
- [ ] Remove correctness dependence on explicit pre-seek interop `flush()`
  calls in the Android player and native regression harness. Keep public
  `flush()` only for standalone interop lifecycle control and make it obey the
  same epoch contract.
- [ ] Add deterministic lifecycle coverage for forward/backward seek, repeated
  timestamps, late callback arrival after invalidation, media replacement, and
  a render overlapping seek. Cover Vulkan and OpenGL ES independently, then
  retain direct-Surface present/drop as a separate regression path.

Acceptance criteria:

1. [ ] `Player::seek()`, stop, media replacement, track-switch reopen, and
   surface-generation replacement invalidate Vulkan and OpenGL pending producer
   state without any application-side interop call.
2. [ ] A deferred frame is retried by exact Player frame identity, while an
   AImageReader image is accepted only for a current-epoch producer association;
   a repeated timestamp after backward seek cannot match an old-epoch image.
3. [ ] Late invalidated images are acquired and returned with their native
   fences, not left to consume AImageReader capacity, and cannot trigger a
   current-generation presentation.
4. [ ] A seek or replacement wakes the Vulkan 100-ms correlation wait promptly;
   an overlapping old render finishes as discarded and cannot republish stale
   pending state after invalidation.
5. [ ] The repair adds no decoded-source CPU map, transfer, staging copy,
   upload, software fallback, per-frame GPU wait, unbounded queue, or
   application-owned retry-frame state. Existing submitted GPU work retains its
   fence/timeline lifetime.
6. [ ] Windows shared/static Release CTest remains passing. Android arm64/API 28
   shared/static core, Vulkan, OpenGL ES, MediaCodec interop, player package, and
   install-consumer cross-builds pass with `git diff --check` and no new Qt
   dependency.
7. [ ] On the connected Android device, H.264 and HEVC/10-bit paths pass repeated
   forward/backward seek and media-replacement matrices through both Vulkan and
   OpenGL ES. Decoding, AImage callbacks, successful presents, audio, and media
   position must all continue; pending depth stays bounded and zero-CPU-copy
   counters remain zero.
8. [ ] MediaCodec direct-Surface present/drop, pause/resume, stop, surface
   recreation, and stale-surface rejection remain passing and are reported
   separately from application-rendered Vulkan/OpenGL ES evidence.

The repeatable Windows shared/static build, test, install, and package-consumer
gate remains complete. Android and OHOS Actions execution is still temporarily
disabled by explicit project direction; the local Android repair and connected
device gate above do not silently re-enable those jobs.

## Active incomplete and external gates

### OHOS strict Vulkan native-buffer and Dolby Vision gate

- [ ] On hardware that exposes a non-opaque sampled multi-plane `VkFormat`,
  validate `VK_OHOS_external_memory` import, explicit format/plane wrapping by
  libplacebo, acquire synchronization, and native-buffer release only after GPU
  completion. Strict source zero-copy forbids a normalization texture or draw.
- [ ] Record the applicable device, system/driver, native format, usage,
  modifier/compression, dataspace/HDR, and `formatFeatures` constraints; prove
  that any P010 mapping preserves raw Y/UV layout and full 10-bit precision.
- [~] Exact normalized-PTS Dolby Vision RPU attachment and Profile 5/8.4 raw
  OpenGL ES validation pass. The production opaque Vulkan Profile 5 path now
  also passes through `RGB_IDENTITY`, but the strict no-intermediate half
  remains open until the explicit-plane gate above passes. Implicit RGB,
  unmatched RPU, enhancement-layer residual reconstruction, and certification
  are not completion.

The connected Huawei devices expose real decoder output as
`VK_FORMAT_UNDEFINED` plus opaque external IDs. Huawei confirms this is
expected, the IDs are not portable `VkFormat` values, the tested driver has no
explicit-format switch, and identity sampling preserves raw P010 precision and
the queried range/chroma properties. This closes AD-009 for the production
opaque route but cannot complete the separate strict direct-plane gate. Do not
substitute Windows Vulkan work or reinterpret an external ID in production.

Research instrumentation prepared on 2026-08-10 keeps the strict result
separate from general Vulkan-path success, rejects mismatched direct YCbCr/VU
plane order, and records NativeBuffer usage/stride/colorspace plus Vulkan
format/memory capabilities. OHOS shared/static cross-builds, installs,
standalone package consumers, and the isolated probe compile pass. A newly
signed HAP was installed and the full connected-device validation passed on the
ALN-AL80 with HarmonyOS 6.1.0.135. The real OHCodec run still reported
`vkFormat=VK_FORMAT_UNDEFINED`, `directPlanes=0`, `workaroundImports=60`, and
`normalization=60`, so it correctly emitted `strictExplicitPlane=GATED`.
Huawei's later formal reply superseded that default workaround policy and
closes AD-009 for the opaque external-format production path. The 2026-08-12
signed Mate 60 Pro player run used the opaque path with
`workaround=0`. Forced-SDR Vulkan captures first verified correct component
order for both ordinary HDR and Dolby Vision. Native HDR then held 25.0 FPS
for `legend.mkv` and 24.1 FPS for `wednesday.mp4`, both with zero Player drops;
the final same-process snapshot reported 1,988 opaque imports,
normalizations, releases, and callbacks. The strict gate remains open because
all those frames still used an RGBA16F normalization image.

### Android/OHOS CI suspension and OHOS hardening

- [~] Restore Android and OHOS Actions execution only after project direction
  re-enables deployment and their pinned SDKs are available to the runner
  service. The retained workflow drivers and local shared/static build,
  install, and package-consumer gates pass; no disabled job is reported as a
  CI pass.
- [~] Broaden real-device software-frame coverage beyond the validated YUV420,
  fit/redraw, SDR/native-HDR Vulkan and OpenGL ES, native-window recreation,
  forced OpenGL ES, and fatal one-way fallback cases. Remaining coverage
  includes other upload families, rotation, and explicit surface-loss
  injection.
- [~] Add shared helpers under `modern/platform/ohos/` only when code is truly
  shared across responsibility-specific OHOS backends. Do not move media,
  graphics, or audio ownership into a combined platform class.
- [~] Run the OHOS user-player manual acceptance matrix only after the user
  configures DevEco signing and explicitly approves the first deployment.
  The 2026-08-11 targeted signed-device pass completed local
  `legend.mkv`/`wednesday.mp4` soft/hard decode by Vulkan/OpenGL ES, live
  backend switching, debug-panel closed/open redraw comparison, descriptor
  lifetime, automatic Vulkan-to-OpenGL ES surface rebind, and explicit HDR
  capability reporting. Hardware paths held 24--25 FPS with zero Player drops;
  the follow-up XComponent SURFACE run passed `Require HDR` through Vulkan
  A2B10G10R10 BT.2020/PQ and OpenGL ES RGB10_A2 BT.2020/PQ, while forced SDR
  still reported tone mapping. The 2026-08-12 follow-up then replaced the
  former Profile 5 Vulkan-to-OpenGL fallback with opaque identity Vulkan
  sampling and passed repeated `wednesday -> legend -> wednesday` switching.
  The separate deferred-render regression now also passes cold-start plus
  forward/backward seek on both local files and an eight-pair Wednesday seek
  stress run without presentation freeze.
  Physical audio identity and the remaining broad manual cells stay open.

The four-thread software-decode build, signed-device counters, temperatures,
and `hiperf` evidence are retained in
[`PLAN_HISTORY_2026-08-11_OHOS_SOFTWARE_DECODE.md`](PLAN_HISTORY_2026-08-11_OHOS_SOFTWARE_DECODE.md).
The Huawei formal reply, opaque identity implementation, consumer-buffer
lifetime repair, and signed-device Vulkan Profile 5 evidence are retained in
[`PLAN_HISTORY_2026-08-12_OHOS_EXTERNAL_FORMAT_IDENTITY.md`](PLAN_HISTORY_2026-08-12_OHOS_EXTERNAL_FORMAT_IDENTITY.md).

### Dolby and device-output scope

- [ ] Validate multichannel PCM device output.
- [ ] Define an IEC 61937 compressed-passthrough contract.
- [ ] Implement and validate Windows HDMI/WASAPI passthrough only after that
  contract is accepted.
- [ ] Evaluate Atmos object-metadata preservation and rendering feasibility.
- [ ] Complete licensing and certification review before making product claims.

Codec decoding must never be described as Dolby certification or Atmos
rendering.

### Release and module boundaries

AD-019 keeps optional backends in this repository as compile-time targets while
interfaces evolve. The remaining release work is:

- [~] Add continuous integration for all supported host/target combinations;
  drivers and local reproduction pass, while Actions is intentionally limited
  to Windows and Android/OHOS execution remains suspended.
- [ ] Define a versioned C ABI only if runtime-loaded plugins become necessary.
- [ ] Split a backend into another repository only when it has an independent
  license, team, release cycle, or closed-source delivery requirement.

## Deferred final task — guarded Android HDR external-OES fallback

Do not begin this task until every preceding implementation, acceptance, and
release gate in this plan is complete.

- [ ] Evaluate an opt-in
  `MediaCodecOpenGLInteropConfig::hdrExternalOesSamplingEnabled = true` path for
  Android P010/HDR `SurfaceTexture`/`GL_TEXTURE_EXTERNAL_OES` input.
- [ ] On the first explicit capability, import, sampling, or presentation
  failure, disable the trial for the current device/codec/session and perform
  exactly one one-way pipeline fallback.
- [ ] Reconfigure MediaCodec at the current playback position to direct-Surface
  presentation so Android composition preserves HDR. Do not retry the retired
  SurfaceTexture generation, map the failed hardware frame, or enter a rebuild
  loop.
- [ ] Preserve play/pause intent, seek position, A/V synchronization, HDR
  metadata, surface lifecycle, and explicit fallback diagnostics.
- [ ] Keep runtime failure detection separate from color validation. Successful
  presentation is not proof of P010 precision, range, BT.2020 primaries,
  PQ/HLG transfer, luminance, or absence of duplicate conversion. A persistent
  allowlist requires independent color goldens and Android compositor
  dataspace/HDR-metadata validation.
- [ ] Add deterministic policy tests and connected-device coverage for trial
  success, each failure class, single fallback, stale-generation rejection,
  seek, background/foreground recreation, and clean shutdown.

## Completed milestone summary

Detailed checklists and validation evidence are retained in
[`PLAN_HISTORY_2026-08-10.md`](PLAN_HISTORY_2026-08-10.md). The active summary
is intentionally short:

| Scope | Status |
| --- | --- |
| Milestones 0–3: Qt-free core, decomposition, backend contracts, portable video/audio references | Complete and verified |
| Milestone 4: former Apple production path | Archived and unsupported |
| Milestone 5: Windows D3D11/D3D11VA/WASAPI | Complete, including the Intel post-seek root-cause investigation and repair; AD-010 is closed |
| Milestone 6: Android Vulkan/OpenGL ES/MediaCodec/AAudio | Baseline complete; MediaCodec/AImageReader seek-generation isolation is the active next hardening task |
| Milestone 7: OHOS production path | AD-009 opaque external-format policy complete and connected-device validated; the separate explicit-plane strict Vulkan gate and broader validation remain open above |
| Milestone 9: track switching, subtitles/libass, external sources, packet buffering/cache, live policy, recovery, accurate seek/step, pitch-preserving time-stretch | Complete and verified on Windows plus Android/OHOS static/shared cross-builds |
| General audio/video processing contracts and reference volume filter | Complete and verified on Windows plus Android/OHOS static/shared package consumption |
| Core C++ API and CMake package version contract | Complete at 2.0.0 with deterministic discovery and supported-target package consumers |
| Milestone 10: software Dolby decode and HDR/Dolby Vision metadata paths | Partial; passthrough, Atmos, strict OHOS Vulkan, licensing, and certification remain open |

## Task selection rules

1. Preserve the passing temporary Windows-only CI gate; restore Android/OHOS
   Actions only after project direction re-enables them.
2. A device-gated OHOS item remains open but does not justify claiming a pass
   on unsuitable hardware or replacing it with a different platform task.
3. Complete the Android MediaCodec seek-generation isolation task before
   starting the guarded Android HDR external-OES work. Do not conflate the two.
4. When a task completes, move detailed evidence to the history/decision/
   architecture document that owns it and keep only a concise status in this
   file.

## Completion checklist for implementation turns

Before marking an implementation item complete:

1. Run tests proportional to the changed scope and the required native or
   cross-target builds.
2. Run `git diff --check`.
3. Search changed core code for accidental Qt, FFmpeg, graphics, and platform
   SDK exposure at forbidden boundaries.
4. Verify UTF-8 without BOM and LF line endings for changed text files.
5. Update this active plan, public documentation, migration notes, architecture,
   and decisions only where their respective ownership requires it.
6. Put detailed commands, device matrices, counters, and investigation history
   in a new dated history record rather than expanding this active plan or
   modifying the frozen snapshot again.
