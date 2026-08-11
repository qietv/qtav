# QtAVCore implementation plan

Last updated: 2026-08-11

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
  lifecycle, raw OpenGL ES interop, and the bounded opaque-format Vulkan
  workaround are implemented and connected-device validated. Strict direct
  multi-plane Vulkan wrapping and the corresponding Dolby Vision path remain
  device-gated.
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

## Next task — gated follow-up after the Windows-only CI pass

- [x] Publish and pass the repeatable Windows shared/static build, test,
  install, and package-consumer gate on the configured self-hosted runner.
  Android and OHOS Actions execution is temporarily disabled by explicit
  project direction; restoring those jobs remains a separate incomplete gate.
- [~] Do not start another local implementation solely to fill this slot. The
  remaining candidates below require explicit Android/OHOS CI re-enablement,
  eligible OHOS hardware, physical audio output, or the documented guarded
  Android HDR condition.

Acceptance criteria:

1. [x] Build and test Windows static/shared Release packages, including the
   staged installed-package consumer and deterministic version requests.
2. [x] Retain locally reproducible Android arm64/API 28 and OHOS arm64/API 23
   static/shared package, install, and standalone-consumer drivers while their
   Actions jobs are suspended.
3. [x] Use the repository platform dependency build and verification scripts;
   do not download workflow artifacts as a local dependency fallback.
4. [x] Keep signed-device playback, native HDR, physical audio, and strict
   native-buffer gates explicitly separate from hosted CI and never report an
   unavailable device check as a pass.
5. [x] Pin or record toolchain inputs, use bounded caches that cannot bypass
   install verification, publish useful failure logs, and document the CI
   ownership and local reproduction commands.

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
  OpenGL ES validation pass. The strict Vulkan half remains open until the
  explicit-plane gate above passes; implicit RGB, unmatched RPU, enhancement-
  layer residual reconstruction, and certification are not completion.

The connected Huawei devices currently expose real decoder output as
`VK_FORMAT_UNDEFINED` plus opaque external IDs. That result validates the
fail-closed/workaround behavior but cannot complete the strict gate. Do not
substitute Windows Vulkan work or silently reinterpret an unknown format.

Research instrumentation prepared on 2026-08-10 keeps the strict result
separate from general Vulkan-path success, rejects mismatched direct YCbCr/VU
plane order, and records NativeBuffer usage/stride/colorspace plus Vulkan
format/memory capabilities. OHOS shared/static cross-builds, installs,
standalone package consumers, and the isolated probe compile pass. A newly
signed HAP was installed and the full connected-device validation passed on the
ALN-AL80 with HarmonyOS 6.1.0.135. The real OHCodec run still reported
`vkFormat=VK_FORMAT_UNDEFINED`, `directPlanes=0`, `workaroundImports=60`, and
`normalization=60`, so it correctly emitted `strictExplicitPlane=GATED`; the
strict gate remains open despite the general Vulkan-path and lifecycle pass.

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
  still reported tone mapping. Physical audio identity and the remaining broad
  manual cells stay open.

The four-thread software-decode build, signed-device counters, temperatures,
and `hiperf` evidence are retained in
[`PLAN_HISTORY_2026-08-11_OHOS_SOFTWARE_DECODE.md`](PLAN_HISTORY_2026-08-11_OHOS_SOFTWARE_DECODE.md).

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
| Milestone 6: Android Vulkan/OpenGL ES/MediaCodec/AAudio | Complete and retained as the mobile regression baseline |
| Milestone 7: OHOS production path | Implemented through current opaque-format policy; explicit-plane strict Vulkan and broader validation remain open above |
| Milestone 9: track switching, subtitles/libass, external sources, packet buffering/cache, live policy, recovery, accurate seek/step, pitch-preserving time-stretch | Complete and verified on Windows plus Android/OHOS static/shared cross-builds |
| General audio/video processing contracts and reference volume filter | Complete and verified on Windows plus Android/OHOS static/shared package consumption |
| Core C++ API and CMake package version contract | Complete at 2.0.0 with deterministic discovery and supported-target package consumers |
| Milestone 10: software Dolby decode and HDR/Dolby Vision metadata paths | Partial; passthrough, Atmos, strict OHOS Vulkan, licensing, and certification remain open |

## Task selection rules

1. Preserve the passing temporary Windows-only CI gate; restore Android/OHOS
   Actions only after project direction re-enables them.
2. A device-gated OHOS item remains open but does not justify claiming a pass
   on unsuitable hardware or replacing it with a different platform task.
3. Do not start the guarded Android HDR external-OES task early.
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
