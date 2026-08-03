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

### Context

Playback defects need enough evidence to separate network/decode starvation,
clock issues, render contention, and compositor/driver latency. Per-frame text
logging can itself create the symptoms under investigation.

### Decision

Always collect bounded atomic cadence counters and one-time stream metadata.
Aggregate and format cadence on the UI every five seconds. Keep the log in
memory, cap it at 1,000 lines, and show it only in the separately toggled Debug
window. Treat the text format as diagnostic, not as a public API.

### Consequences

- Opening Debug does not insert synchronous work into audio/video workers.
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
separate Intel imported-hardware-frame crash and synchronization workaround are
governed by project decision AD-007 in `modern/DECISIONS.md`.
