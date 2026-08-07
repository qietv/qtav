# WinUI 3 player architecture decisions

This document records decisions that constrain future changes to the example.
Each record has one of these states: **Proposed**, **Accepted**, **Superseded**,
or **Rejected**. New decisions should be appended; superseded records should
remain with a link to their replacement.

## Decision index

| ID | Decision | Status |
| --- | --- | --- |
| [ADR-001](#adr-001-unpackaged-self-contained-winui-3-application) | Unpackaged, self-contained WinUI 3 application | Accepted |
| [ADR-002](#adr-002-use-the-high-level-d3d11-video-output) | Use the high-level D3D11 video output | Accepted |
| [ADR-003](#adr-003-ui-is-observational-not-part-of-playback-timing) | UI is observational, not part of playback timing | Accepted |
| [ADR-004](#adr-004-use-wasapi-as-the-playback-clock-master) | Use WASAPI as the playback clock master | Accepted |
| [ADR-005](#adr-005-deterministic-ordered-shutdown) | Deterministic ordered shutdown | Accepted |
| [ADR-006](#adr-006-keep-diagnostics-lightweight-and-opt-in) | Keep diagnostics lightweight and opt-in | Accepted |
| [ADR-007](#adr-007-coalesce-progress-interaction-into-one-asynchronous-seek) | Coalesce progress interaction into one asynchronous seek | Accepted |
| [ADR-008](#adr-008-use-opaque-rgb10pq-video-presentation) | Use opaque RGB10/PQ video presentation | Accepted |
| [ADR-009](#adr-009-retry-transient-render-contention-with-bounded-context-handoff) | Retry transient render contention with bounded context handoff | Accepted |
| [ADR-010](#adr-010-copy-the-visible-decoder-region-by-default) | Copy the visible decoder region by default | Accepted |

## ADR-001: Unpackaged, self-contained WinUI 3 application

- **Status:** Accepted
- **Date:** 2026-08-03

### Context

The example must be easy to build and launch from the repository without an
MSIX deployment workflow. It still needs modern WinUI controls and a
`SwapChainPanel` host.

### Decision

Build an x64 C++/WinRT desktop application with `WindowsPackageType=None`,
`AppxPackage=false`, and `WindowsAppSDKSelfContained=true`. Restore Windows App
SDK through NuGet and copy QtAVCore/FFmpeg runtime DLLs beside the executable.

### Consequences

- Launch does not require package installation or identity.
- The output directory is larger because Windows App SDK is self-contained.
- Package-identity-only APIs must not be assumed.
- Only x64 Debug and Release are supported until another architecture is added
  deliberately to the project, build script, and test matrix.

## ADR-002: Use the high-level D3D11 video output

- **Status:** Accepted
- **Date:** 2026-08-03

### Context

An earlier integration can manually own a D3D11 device, swap chain,
`D3D11VideoRenderer`, D3D11VA configuration, hardware-frame interop,
`renderVideo()`, and `Present()`. That duplicates production lifecycle and
threading logic in every UI toolkit example.

### Decision

Use `qtav::D3D11VideoOutput`. The example supplies the HWND, current surface
size/scales, and `ISwapChainPanelNative::SetSwapChain` binding callback. The
library owns device creation, swap-chain/color policy, renderer, hardware
decode/interop wiring, render thread, presentation, resize, and teardown.

### Consequences

- The sample remains small and exercises the same output path applications are
  expected to consume.
- D3D11VA surfaces and renderer interop share a validated device.
- Render scheduling and `Present()` stay off the WinUI dispatcher.
- `attach()` borrows Player and exclusively owns its render slot, so it must be
  detached before Player or the native surface is destroyed.
- Low-level renderer experiments belong in focused backend tests/examples, not
  in this user-facing player.

## ADR-003: UI is observational, not part of playback timing

- **Status:** Accepted
- **Date:** 2026-08-03

### Context

WinUI dispatch can be delayed by layout, input, diagnostics, window movement,
or unrelated application work. Driving audio or video from that dispatcher
causes stalls even when decode CPU usage is low.

### Decision

Player workers own demux, decode, bounded queues, and presentation scheduling;
`D3D11VideoOutput` owns a private render thread; WASAPI submission uses the
Player audio-output worker. Worker callbacks may only copy bounded data and
post UI work through `UiBridge`. The progress timer reads a cached position and
never advances the media clock.

### Consequences

- A busy UI cannot block audio submission or the D3D11 presentation loop.
- All XAML access must occur after dispatcher marshalling.
- Callback captures need independent lifetime guards because queued UI work may
  outlive the event that created it.
- Debug formatting and per-frame logging cannot be added directly to playback
  callbacks.

## ADR-004: Use WASAPI as the playback clock master

- **Status:** Accepted
- **Date:** 2026-08-03

### Context

Using a UI timer or an unconstrained wall clock as master can run video ahead
of the audio device, especially across buffer latency, pause, underrun, and
seek. Querying the audio device from UI code would also couple unrelated
threads.

### Decision

Install `WasapiAudioSink` and `SwresampleAudioConverter` on Player. When the
sink exposes a valid device presentation clock, Player uses it as master and
publishes a cached clock snapshot. The UI reads only `Player::position()`.
Player owns underrun freeze/recovery and seek re-anchoring.

### Consequences

- Video follows what the user actually hears rather than dispatcher cadence.
- Audio conversion and device writes remain outside the UI and video render
  paths.
- Audio endpoint behavior is a first-class part of stutter investigation.
- A future mute/no-audio mode needs an explicit fallback-clock policy in the
  library, not a timer added to the example.

## ADR-005: Deterministic ordered shutdown

- **Status:** Accepted
- **Date:** 2026-08-03

### Context

FFmpeg reads, codec work, audio drivers, and graphics drivers can be active when
the user closes a window. Letting the UI object disappear before callbacks or
detaching workers risks use-after-free, COM/device lifetime violations, and
process teardown races. Joining everything can make an external stall visible.

### Decision

Shutdown is idempotent and synchronous. First invalidate the UI callback
target and stop timers. Request Player stop, detach/close video output, then
destroy Player so its interrupt callback and condition-variable wakeups can
terminate and join every worker before resources are released.

### Consequences

- No callback may dereference a closed window.
- The borrowed Player/output relationship is released in the documented order.
- Exit latency remains measurable rather than hidden by leaked or detached
  work.
- If product UX later requires a non-blocking close animation, implement an
  explicit two-phase lifetime whose owner survives until teardown completes;
  do not detach workers or destroy on a playback callback.

## ADR-006: Keep diagnostics lightweight and opt-in

- **Status:** Accepted
- **Date:** 2026-08-03
- **Amended:** 2026-08-08 to disable cadence/timing collection while Debug is
  closed

### Context

Playback defects need enough evidence to separate network/decode starvation,
clock issues, render contention, and compositor/driver latency. Per-frame text
logging can itself create the symptoms under investigation.

### Decision

Always collect only one-time stream metadata. Opening Debug enables bounded
atomic cadence counters and `D3D11StatisticsMode::Timing`; closing it selects
`Off` and stops callback clocks, output clocks, and five-second formatting.
Keep the log in memory, cap it at 1,000 lines, and show it only in the
separately toggled Debug window. Treat the text format as diagnostic, not as a
public API. Retry classification uses `VideoRenderRetryReason`, never optional
statistics.

### Consequences

- Debug adds only bounded atomic cadence work to audio/video callbacks; closing
  it restores the uninstrumented callback path apart from one relaxed flag
  load.
- Long sessions have bounded log history.
- Diagnostics are easy to copy visually but are not persisted automatically.
- Stable telemetry or automated performance analysis should use a separate
  structured interface rather than parsing this log text.

## ADR-007: Coalesce progress interaction into one asynchronous seek

- **Status:** Accepted
- **Date:** 2026-08-03

### Context

A progress slider can emit many value changes during one drag. Treating each
change as a media command creates overlapping asynchronous seeks, repeatedly
flushes decoder and output generations, and lets an obsolete completion update
the current UI. It also makes a UI preview look like authoritative playback
time while the new audio/video generation is still buffering.

### Decision

For pointer input, slider movement updates only the local time preview and one
absolute seek is submitted when the interaction ends. Non-pointer value
changes are coalesced by a 180 ms debounce. While a seek is pending, the
progress timer does not overwrite the selected target. Every submitted seek
has a serial number, and media replacement, stop, or a newer seek invalidates
older completions. Normal progress resumes only from the cached
`Player::position()` after the current completion is observed.

### Consequences

- One drag causes one decoder/output-generation transition instead of a burst
  of intermediate transitions.
- The displayed target remains stable while Player reports `Buffering` and
  waits for real post-seek output.
- Keyboard and accessibility changes remain usable without issuing a seek for
  every transient value.
- Any future thumbnail or hover-preview feature must use a separate preview
  path; it must not turn slider movement back into repeated playback seeks.

## ADR-008: Use opaque RGB10/PQ video presentation

- **Status:** Accepted
- **Date:** 2026-08-03

### Context

The FP16 scRGB renderer readback produced the expected absolute luminance, and
Windows reported an active HDR composition layer. Those diagnostics did not
establish visual parity: both ordinary HDR10 and Dolby Vision media still
looked dim beside a trusted native-PQ player on the same display.

### Decision

The example's video surface is opaque, so it selects
`D3D11HdrPresentationMode::HDR10` together with `DXGI_ALPHA_MODE_IGNORE`.
QtAVCore retains FP16 scRGB as the general-purpose library default. Runtime
`colorInfo()` remains diagnostic evidence for the selected swap-chain format,
color space, display peak, and system SDR white level.

### Consequences

- The example submits native RGB10/PQ video instead of relying on DWM's scRGB
  conversion for this opaque composition surface.
- The video surface cannot use premultiplied alpha blending in this mode.
- SDR fallback, moving between monitors, and Windows HDR-setting changes remain
  responsibilities of the high-level D3D11 output.
- Manual side-by-side comparison with a trusted native-PQ player is still
  required; HDR-active metadata alone is not a brightness acceptance test.

### Validation

The player reported active RGB10/PQ output for ordinary HDR10 `legend.mkv` and
Dolby Vision Profile 5 `wednesday.mp4` on the same Windows HDR display. The
user compared the result with MPC-BE and confirmed matching brightness. The
separate cross-vendor imported-hardware-frame crash and synchronization
workaround are governed by project decision AD-007 in `modern/DECISIONS.md`.

## ADR-009: Retry transient render contention with bounded context handoff

- **Status:** Accepted
- **Date:** 2026-08-04

### Context

The original render callback carried no frame identity, and
`Player::renderVideo()` collapsed no-frame, Player-lock contention, and backend
contention into one negative value. `D3D11VideoOutput` consumed that request as
skipped. On the Radeon 880M, momentary collisions therefore became visible
missing frames even though scheduled cadence, D3D11VA throughput, renderer
stage times, and `Present()` remained within budget. Making the Player lock
blocking removed most symptoms but would put playback-control work in the
native render path. Removing that lock exposed a second issue: FFmpeg's decode
thread could immediately reacquire the shared D3D11 context before a timer-only
render retry.

### Decision

Use `Player::renderVideoDetailed()` on the output's private render thread. The
Player publishes immutable frame and renderer-binding snapshots atomically,
returns a frame sequence, presentation generation, and structured retry reason,
and rechecks generation after rendering. The output retries only classified busy results in a
latest-frame mailbox; a newer sequence supersedes the older pending frame.

Before every output pass makes its first non-blocking D3D11 context attempt, it
reserves the render thread ahead of new FFmpeg/internal acquisitions. An
uncontended attempt proceeds immediately; a contended attempt waits for at most
8 ms for the current owner to yield. A timeout returns to bounded
1/2/4/8/16-ms backoff rather than spinning. Ordinary public context users are
not blocked by the reservation, and every wait stays on the private output
thread. The context guard is released when rendering returns and is never held
through retry classification, statistics reads, or swap-chain presentation.
Statistics are read only on application request, not after every render.
Detach, stop, and
connection-generation changes cancel both pending retry and reservation.

Diagnostics report attempt reason, renderer lock stage, context-owner class,
handoff wait/timeout, retry wakeup, supersession, and terminal drop separately.
The legacy `render-skipped` field mirrors terminal drops so recovered retries
are no longer reported as missing frames.

### Consequences

- UI and playback-control threads remain non-blocking with respect to render
  contention; the private render worker absorbs the bounded handoff.
- A transient busy result can be recovered without requiring another decoder
  callback, while the mailbox remains bounded to one latest frame.
- Seek, stop, and media replacement cannot present a backend result completed
  for an obsolete presentation generation.
- On the recorded Radeon 880M, settled Dolby Vision Profile 5 and HDR10 runs
  retained D3D11VA, RGB10/PQ, and zero decoder copies with zero terminal render
  drops. All observed context collisions were reservation-aware FFmpeg-side
  ownership, and every bounded handoff completed without timeout.
- A later high-load check reproduced rare supersession while the reservation
  was still reactive. Moving it ahead of the first context attempt restored
  zero renderer-busy/superseded/terminal counts with zero handoff timeouts.
  Separate high draw times after prolonged build/UI-capture load disappeared
  in a controlled cold rerun, which restored both supplied files to source
  cadence and zero steady coalescing.
- The same-build Iris Xe regression later localized a separate persistent
  post-seek stall to redundant FFmpeg D3D11VA decoder teardown. Compatible
  frames-context and decoder reuse removed that teardown; the generic retry
  policy here remains independent and still does not claim cross-device
  performance equivalence.

## ADR-010: Copy the visible decoder region by default

- **Status:** Accepted
- **Date:** 2026-08-06
- **Amended:** 2026-08-07 for completion-retained source frames and bounded
  off-render-thread release

### Context

Directly sampling an FFmpeg D3D11VA texture array removes one GPU-local copy,
but forces shader-resource usage onto decoder allocations and retains scarce
decoder slices until the asynchronous draw completes. The policy is sensitive
to driver handling of decoder padding, seek/flush lifetime, and shutdown.
mpv's current D3D11VA backend uses a visible-region same-format copy by default
and exposes direct sampling only as a user opt-in with compatibility warnings.

### Decision

The example keeps `D3D11VideoOutputOptions::directDecoderTextureSampling`
disabled. QtAVCore copies the even-aligned visible NV12/P010 rectangle into a
bounded three-entry shader-readable ring. A D3D11.1 context uses
`CopySubresourceRegion1()` with `D3D11_COPY_DISCARD`; the fallback uses the same
source box without the flag. The source stays raw, so libplacebo remains the
only Dolby Vision and color authority. No CPU map, RGB intermediate, Video
Processor conversion, or per-frame GPU drain is introduced.

### Consequences

- The default path incurs one same-device GPU copy per rendered hardware frame
  and reports it in `decoder-copies`.
- Decoder textures remain retained through GPU completion. Their source-frame
  and interop references then move to a bounded recycler so Intel/other vendor
  allocation teardown cannot execute on the output render thread.
- Advanced applications can opt into direct sampling for an A/B performance
  test, but must accept the documented driver/lifetime risks.
- The remaining native policy gate uses the same Release revision on NVIDIA and
  AMD, `legend.mkv` only, with
  `D3D11VideoOutputOptions::directDecoderTextureSampling` both off and on.
  `decoder-copies` is auxiliary path evidence, not the tested switch.
- Compatible frames-context and decoder reuse is an independent shared-device
  lifetime decision governed by
  [QtAVCore AD-011](../../DECISIONS.md#ad-011-windows-reuses-compatible-d3d11va-frames-contexts-and-decoders)
  and [FFmpeg FD-005](../../../ffmpeg/DECISIONS.md#fd-005-reuse-a-compatible-d3d11va-decoder-with-its-frames-context).
