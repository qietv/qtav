# WinUI 3 player testing

## Validation levels

Use the smallest level that covers the change, then increase coverage when a
failure crosses library/backend boundaries.

| Level | Use for | Required checks |
| --- | --- | --- |
| Documentation | Markdown-only changes | Links, spelling/content review, `git diff --check`, encoding/line endings. |
| Example build | XAML, C++/WinRT, project, or build-script changes | Release build; Debug too when configuration behavior changes. |
| Playback smoke | Controls, callbacks, seek, diagnostics, or lifetime changes | Local file, reachable HTTP(S) URL, Debug toggle, seek, pause/resume, stop, close while playing. |
| Backend regression | Player, clock, queue, audio, render, hardware, or output changes | Relevant QtAVCore CTest targets plus playback smoke. |
| Display/device | HDR, adapter, swap-chain, audio endpoint, or driver-sensitive changes | Native validation on the affected Windows display/GPU/audio device. |

Do not report native, HDR, URL, audio, or exit-performance validation when only
a compile or unit test was run.

## Automated build

From the repository root, configure the shared QtAVCore build if needed:

```powershell
cmake -S modern -B build/modern-shared `
  -DBUILD_SHARED_LIBS=ON `
  -DQTAV_CORE_BUILD_TESTS=ON `
  -DQTAV_CORE_BUILD_EXAMPLES=ON
```

Build and run the library tests:

```powershell
cmake --build build/modern-shared --config Release --parallel
ctest --test-dir build/modern-shared -C Release --output-on-failure
```

Build the WinUI player and its required backend targets:

```powershell
Set-Location modern/examples/winui3_player
./build.ps1 -Configuration Release
```

For configuration-specific project or runtime changes, repeat with
`-Configuration Debug`. The application should start from
`bin/x64/<Configuration>/QtAVWinUI3.exe` without relying on QtAVCore/FFmpeg
DLLs elsewhere in `PATH`.

## Manual smoke matrix

Use media for which codec, frame rate, resolution, audio layout, duration, and
expected seekability are known. For network testing, prefer a file-style
HTTP(S) URL under the tester's control; record whether it supports range
requests.

| Scenario | Action | Expected result |
| --- | --- | --- |
| Startup | Launch with no media. Toggle Debug twice. | Main window remains responsive; Debug opens with initialization/device information and closes cleanly. |
| Local file | Open a known A/V file. | Loading reaches Loaded/Playing, audio is clean, video cadence matches the source, and the progress display advances. |
| URL | Open a reachable file-style HTTP(S) URL. | Playback starts without blocking the UI; transient recovery is visible as Buffering rather than a frozen dispatcher. |
| Pause/resume | Pause for several seconds, then resume. | Audio and video stop advancing while paused and resume without a burst, clock jump, or persistent noise. |
| Seek | Drag backward and forward several times. | One seek is logged per drag; status transitions through Buffering as needed; new-generation audio/video resume in sync. |
| Stop/replay | Stop during playback, then press Play. | Workers do not leak stale media; playback can restart according to Player state semantics. |
| Resize | Repeatedly resize, minimize/restore, and change DPI if available. | Surface resize succeeds; no black/stretched persistent frame or surface-lost loop. |
| Monitor/HDR | Play ordinary HDR10 and Dolby Vision while Windows HDR is active; compare the same frames with a trusted native-PQ player, then move between SDR/HDR monitors or toggle Windows HDR when supported. | Debug reports RGB10/PQ only while the layer is HDR; highlights and diffuse white are comparable to the reference player, and SDR fallback remains viewable. |
| Invalid input | Enter an unreachable or invalid URL. | A bounded error/status transition appears; UI, Debug toggle, and close remain responsive. |
| Shutdown | Close the main window while local playback, URL loading, URL playback, pause, and seek are active. | No crash or callback after window destruction; process exits after deterministic worker teardown. |

For regressions, repeat a representative scenario for at least several minutes
and include multiple seeks. A short successful startup is not enough evidence
for queue growth, cadence, or shutdown behavior.

For cross-vendor D3D11 handoff, record the adapter PCI vendor and driver
version. Every vendor enables native immediate-context multithread protection,
retains imported resources through the bounded completion-query queue, uses
fast parameters for imported D3D11VA frames, and leaves successful per-frame
submission asynchronous. Exercise ordinary H.264/NV12, HDR10/P010, and Dolby
Vision when supported, including cold starts, a sustained run, repeated seeks,
and close while playing. `decoder-copies` must remain zero because Dolby Vision
samples the retained decoder array slice directly. Software frames and the
explicit software-mapping fallback retain their default render parameters.

## Diagnosing cadence and stalls

Capture at least two consecutive five-second cadence lines after startup has
settled. Also record source frame rate, codec, resolution, audio format, output
color mode, seek history, network/local source, CPU/GPU utilization, and audio
endpoint changes.

Interpret the main fields together:

- `scheduled-video` below source rate suggests input, decode, clock, queue, or
  Player presentation starvation before D3D11 output.
- Normal `scheduled-video` but low `rendered` suggests render contention,
  surface loss, compositor backpressure, or a D3D11 driver stage.
- Growing `coalesced` means the output receives redraws faster than it can
  service them; occasional coalescing is expected under pressure.
- `present-busy` shows non-blocking swap-chain backpressure. Correlate it with
  render gaps rather than treating any nonzero count as a failure.
- `render-skipped` means a retryable non-blocking Player/backend operation was
  busy. Persistent growth with low presentation rate needs investigation.
- `max-stage-ms(color/interop/buffer/draw)` localizes a long render operation.
- High `>80ms gaps(video/render)` with low CPU can indicate blocking I/O, clock
  starvation, driver waits, or a queue/lifetime bug rather than insufficient
  decode throughput.

For audio noise, also test another output endpoint and a local copy of the same
media. Determine whether the problem follows the stream, network path, format
conversion, device route, or seek/underrun transition. Do not infer an audio
clock defect from low CPU alone.

## Shutdown investigation

Measure from the main-window close action until the process exits. Compare:

1. idle application;
2. paused local playback;
3. active local playback;
4. active URL playback;
5. URL open/read failure;
6. close immediately after a seek.

Record the media protocol, duration of the delay, last Debug events, and
whether audio/video devices changed. QtAVCore intentionally interrupts media
I/O and joins all workers; a persistent multi-second delay should be localized
to the remaining worker/backend operation, not hidden by detaching it.

## Source hygiene

Before handoff:

```powershell
git diff --check
rg -n '#include\s*[<"]Q|QtCore|QtGui|QtWidgets' modern
git status --short
```

Confirm that changed text files are UTF-8 without BOM and use LF only. Do not
stage generated XAML, NuGet packages, `bin/`, `obj/`, Debug, or Release output.
