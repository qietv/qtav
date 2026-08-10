# QtAVCore implementation plan

Last updated: 2026-08-10

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
  direct decoder-texture sampling is explicit opt-in. The separate Intel
  post-seek performance investigation remains an external-machine workstream.
- Android Vulkan/OpenGL ES/MediaCodec/AAudio, application-rendered raw-component
  paths, direct-Surface mode, and one-way renderer fallback are complete and
  retained as the mobile regression baseline.
- OHOS Vulkan/OpenGL ES software presentation, mobile selection/fallback,
  OHAudio, OHCodec H.264/HEVC, capability-gated VVC/H.266, direct-surface
  lifecycle, raw OpenGL ES interop, and the bounded opaque-format Vulkan
  workaround are implemented and connected-device validated. Strict direct
  multi-plane Vulkan wrapping and the corresponding Dolby Vision path remain
  device-gated.
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
- Supported-target CI workflow and local drivers are implemented. Current-
  revision reproduction passes Windows shared/static Release CTest (60/60
  each) plus Android arm64/API 28 and OHOS arm64/API 23 shared/static build,
  install, and installed-package consumers. The first GitHub Actions run is
  pending publication, so the CI task remains partial.

Detailed implementation and validation evidence for the version contract is in
[`PLAN_HISTORY_2026-08-10_VERSION_CONTRACT.md`](PLAN_HISTORY_2026-08-10_VERSION_CONTRACT.md).
The CI implementation and local validation record is in
[`PLAN_HISTORY_2026-08-10_CI.md`](PLAN_HISTORY_2026-08-10_CI.md).

## Next task — add supported-target continuous integration

- [~] Add repeatable CI for the supported Windows, Android, and OHOS build,
  test, install, and package-consumer matrix without converting device-only
  validation into a false hosted pass. The implementation and local
  runner-equivalent matrix pass; publication and the first GitHub Actions run
  remain the named completion gate.

Acceptance criteria:

1. [x] Build and test Windows static/shared Release packages, including the
   staged installed-package consumer and deterministic version requests.
2. [x] Cross-build and install Android arm64/API 28 and OHOS arm64/API 23
   static/shared packages from the repository dependency prefixes, then build
   their standalone package consumers.
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

### OHOS hardening and automation

- [~] Add QtAVCore OHOS CI execution. The workflow/driver implementation and
  local runner-equivalent shared/static cross-build, install, and package-
  consumer gate pass. Local signed-HAP playback and the current connected-
  device matrices are complete; the first GitHub execution awaits publication.
- [~] Broaden real-device software-frame coverage beyond the validated YUV420,
  fit/redraw, SDR Vulkan/OpenGL ES, native-window recreation, forced OpenGL ES,
  and fatal one-way fallback cases. Remaining coverage includes other upload
  families, rotation, explicit surface-loss injection, and native HDR.
- [~] Add shared helpers under `modern/platform/ohos/` only when code is truly
  shared across responsibility-specific OHOS backends. Do not move media,
  graphics, or audio ownership into a combined platform class.

### External Intel Windows performance workstream

- [~] On the administrator-capable Intel machine, capture the remaining
  post-seek failure with WPR/GPUView and objective source/scheduled/rendered,
  retry, gap, Present, lifecycle, and coarse-stage counters. Establish the
  first failing stage before changing code.
- [~] If evidence identifies a repair, keep AD-007 multithread protection and
  AD-010/AD-011 lifetime policies intact, add a narrow regression, and rerun
  the same revision on the recorded Intel, NVIDIA, and AMD matrix.
- [~] Use the supplied HDR10/P010 and Dolby Vision media, controlled seek and
  lifecycle scenarios, comparable output/power/display conditions, settled
  cadence windows, and 60–120-second runs. A visual impression or lower output
  quality is not acceptance evidence.

This external workstream does not block the next local task, but it must not be
described as closed until its evidence-backed correction and same-revision
cross-vendor gate pass.

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
  implementation and local reproduction pass, while publication and the first
  workflow run remain open.
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
| Milestone 5: Windows D3D11/D3D11VA/WASAPI | Complete; separate Intel performance workstream remains external |
| Milestone 6: Android Vulkan/OpenGL ES/MediaCodec/AAudio | Complete and retained as the mobile regression baseline |
| Milestone 7: OHOS production path | Implemented through current opaque-format policy; explicit-plane strict Vulkan and broader validation remain open above |
| Milestone 9: track switching, subtitles/libass, external sources, packet buffering/cache, live policy, recovery, accurate seek/step, pitch-preserving time-stretch | Complete and verified on Windows plus Android/OHOS static/shared cross-builds |
| General audio/video processing contracts and reference volume filter | Complete and verified on Windows plus Android/OHOS static/shared package consumption |
| Core C++ API and CMake package version contract | Complete at 2.0.0 with deterministic discovery and supported-target package consumers |
| Milestone 10: software Dolby decode and HDR/Dolby Vision metadata paths | Partial; passthrough, Atmos, strict OHOS Vulkan, licensing, and certification remain open |

## Task selection rules

1. Add supported-target continuous integration unless the user gives a
   different priority.
2. A device-gated OHOS item remains open but does not justify claiming a pass
   on unsuitable hardware or replacing it with a different platform task.
3. The external Intel workstream may proceed in parallel on its designated
   machine; preserve its unresolved status here.
4. Do not start the guarded Android HDR external-OES task early.
5. When a task completes, move detailed evidence to the history/decision/
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
