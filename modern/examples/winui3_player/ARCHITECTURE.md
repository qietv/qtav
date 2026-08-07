# WinUI 3 player architecture

## Scope

The WinUI 3 player is an integration example around QtAVCore's public Windows
targets. It owns application windows and translates UI actions into Player
operations. It deliberately delegates media I/O, decoding, buffering,
synchronization, audio output, video rendering, presentation, and hardware
fallback to the library.

The example has four goals:

1. keep the WinUI dispatcher independent from playback timing;
2. exercise the production Windows audio/video stack through public APIs;
3. make local-file and file-style URL behavior observable;
4. shut down every callback and worker with deterministic ownership.

It is not a reusable WinUI control, a playlist engine, or an adaptive streaming
client.

## Component map

```mermaid
flowchart LR
    UI["WinUI UI thread\nMainWindow + DebugWindow"]
    Bridge["UiBridge\nDispatcherQueue + lifetime gate"]
    Player["qtav::Player\ncontrol, demux, decode, clocks"]
    Audio["AudioResample + AudioWASAPI\nPCM conversion and device output"]
    Output["OutputD3D11\nprivate render thread"]
    GPU["D3D11 renderer + D3D11VA + interop\ncomposition swap chain"]
    Recycler["D3D11 frame recycler\nbounded deferred release"]

    UI -->|"setMedia / setState / seek"| Player
    Player -->|"bounded audio path"| Audio
    Player -->|"scheduled video + redraw request"| Output
    Output -->|"renderVideo + non-blocking Present"| GPU
    GPU -->|"completed imported-frame release"| Recycler
    UI -->|"HWND, SetSwapChain, resize"| Output
    Player -->|"state, status, event, frame metadata"| Bridge
    Output -->|"event, presented frame"| Bridge
    Bridge -->|"small UI updates"| UI
```

## Major classes

| Class | Role | Ownership |
| --- | --- | --- |
| `App` | Creates and activates the main window; reports startup exceptions. | Owns the WinUI `MainWindow` reference. |
| `MainWindow` | XAML event surface and public window lifetime. | Owns `MainWindowPrivate`. |
| `MainWindowPrivate` | Wires controls, Player, video output, timers, seeking, and diagnostics. | Owns Player before attaching output; explicitly releases output before Player. |
| `UiBridge` | Posts worker notifications to `DispatcherQueue` and gates callbacks during teardown. | Shared by callbacks; holds only an atomic, non-owning target. |
| `CallbackState` | Holds lock-free cadence counters and one-shot log flags. | Shared by callbacks independently of the window lifetime. |
| `DebugWindow` | Displays the bounded in-memory log. | Referenced by `MainWindowPrivate`; never used from playback workers. |
| `qtav::Player` | Asynchronous control, FFmpeg media work, decode queues, playback clock, and frame scheduling. | Uniquely owned by `MainWindowPrivate`. |
| `qtav::D3D11VideoOutput` | D3D11 device, swap chain, HDR/display state, renderer, hardware decode/interop integration, render thread, and presentation. | Uniquely owned by `MainWindowPrivate`; temporarily borrows Player while attached. |

## Thread model

The application assumes callbacks can arrive on non-UI threads even when the
current implementation commonly uses a particular worker. This keeps the UI
correct if callback routing changes later.

| Execution context | Work performed | Must not do |
| --- | --- | --- |
| WinUI dispatcher | XAML access, file picker, user commands, 250 ms progress sampling, debounced seek, surface binding/resize, Debug text updates. | Decode, block on network reads, submit audio, render frames, or call `Present()`. |
| Player control/demux worker | Opens media, reads packets, applies state/control requests, interrupts obsolete I/O, and emits control/status events. | Access XAML or depend on UI progress ticks. |
| Player audio decode worker | Decodes audio packets and feeds the bounded audio queue. | Wait for the UI or render thread. |
| Player audio-output worker | Converts negotiated PCM, writes the WASAPI sink, and samples/publishes the device clock. | Perform UI/log formatting or wait for video presentation. |
| Player video decode worker | Decodes video packets and feeds the bounded presentation path. | Present directly to the SwapChainPanel. |
| Player presentation worker | Applies playback timing, drops late video, publishes frame callbacks, and requests rendering. | Submit device audio or synchronously invoke the UI. |
| D3D11 output render thread | Coalesces redraws, calls `Player::renderVideoDetailed()`, retries classified transient contention, performs hardware interop or software fallback, tracks display color, and presents. | Access XAML controls or destroy/detach itself from its callback. |
| D3D11 frame-recycler worker | Releases completed imported wrappers and source hardware frames after renderer-owned GPU wrappers are destroyed. | Submit draws, access XAML, or outlive the renderer. |

All UI-bound notifications use this pattern:

1. a worker callback copies primitive values or owned strings;
2. it calls `UiBridge::post()`;
3. the dispatcher lambda atomically checks that the target is still alive;
4. only then does it update XAML or the Debug buffer.

Per-frame callbacks only update atomic counters. First-frame callbacks capture
a small metadata snapshot once. This prevents an open Debug window or a busy
dispatcher from applying backpressure to audio or video delivery.

## Playback flow

### Startup and media replacement

1. `MainWindowPrivate` creates Player and installs
   `SwresampleAudioConverter` plus `WasapiAudioSink`.
2. It registers callbacks, starts UI timers, and observes panel/slider events.
3. When `SwapChainPanel` is loaded, it creates `D3D11VideoOutput`, supplies the
   HWND and `SetSwapChain` callback, opens it, and attaches Player.
4. Opening a file or URL resets only UI/diagnostic generation state, calls
   `Player::setMedia()`, then requests `State::Playing`.
5. Player asynchronously interrupts any obsolete media operation, opens the
   new input, selects streams, configures decode, and starts delivery.

`D3D11VideoOutput::attach()` exclusively owns Player's default render API and
render callback while attached. By default it also configures D3D11VA using
the same D3D11 device required by the renderer and interop path. Core retains a
compatible initialized D3D11 hardware-frames context across repeated FFmpeg
format selection, and the paired repository FFmpeg package retains the matching
decoder and output views with that context. A changed device, format, size,
pool requirement, profile, texture, or decoder configuration creates a new
context/decoder through the ordinary path.

### Audio path and master clock

```text
FFmpeg packet -> audio decode worker -> bounded audio queue
              -> audio-output worker -> libswresample -> WASAPI
              -> cached device presentation clock -> Player position/A-V timing
```

Audio submission is independent of the UI and video render threads. A valid
WASAPI presentation clock becomes playback master; `Player::position()` reads
a cached snapshot and does not query the device from the UI thread. During
underrun or a seek generation change, Player freezes or re-anchors the clock
rather than allowing an arbitrary UI timer to advance playback.

### Video path

```text
FFmpeg packet -> video decode worker -> bounded presentation queue
              -> presentation worker -> current frame + render request
              -> D3D11 output latest-frame mailbox
              -> render thread -> Player::renderVideoDetailed()
              -> D3D11VA interop or software fallback -> swap-chain Present
```

The presentation queue is bounded and late frames may be dropped. Render
requests are coalesced. A retryable Player/backend result remains in a
latest-frame mailbox; a newer sequence supersedes it instead of growing a
queue. Before each output pass makes its first non-blocking D3D11 context
attempt, it reserves the render thread ahead of new FFmpeg-side acquisitions.
An uncontended attempt proceeds immediately; contention enters an at-most-8-ms
handoff and uses bounded timer backoff only after a timeout. This wait, the
frame-latency cap, and non-blocking presentation all live on the private output
thread, so neither context contention nor a busy compositor stalls WASAPI or
the WinUI dispatcher.

Hardware frames remain on the selected D3D11 device. Their raw NV12/P010
planes pass through libplacebo color processing and rendering without a CPU
map. By default the renderer copies only the even-aligned visible rectangle
into a bounded same-format ring and uses `D3D11_COPY_DISCARD` on D3D11.1. The
decoder slice, imported wrapper, copied texture, borrowed target, and swap-chain
back buffer stay retained until GPU completion. The renderer then destroys its
GPU wrappers and transfers the imported wrapper plus source frame to a bounded
64-entry recycler; the recycler drops the interop reference before the source
frame so final FFmpeg/D3D11VA destruction does not execute on the output render
thread. If the recycler is full, the three-entry completion queue applies
bounded render backpressure rather than releasing a frame inline. Flush,
resize, replacement, and teardown drain both stages in order.

Every successfully imported D3D11VA frame uses fast libplacebo parameters. The
high-level `D3D11VideoOutputOptions::directDecoderTextureSampling` switch is
off (`false`) for the default GPU-copy mode. Turning it on (`true`) removes that
copy, requests shader-readable decoder arrays, and directly samples the retained
decoder slice; it does not change the completion/recycler lifetime. Software
frames remain outside this imported-frame policy. Unsupported media or devices
fall back through QtAVCore's software decode/render path rather than an
application-side decoder. The durable contracts are recorded in
[QtAVCore AD-010](../../DECISIONS.md#ad-010-windows-copies-the-visible-decoder-region-by-default),
[QtAVCore AD-011](../../DECISIONS.md#ad-011-windows-reuses-compatible-d3d11va-frames-contexts-and-decoders),
and [FFmpeg FD-005](../../../ffmpeg/DECISIONS.md#fd-005-reuse-a-compatible-d3d11va-decoder-with-its-frames-context).

### Seek and progress

The progress timer samples cached `Player::position()` every 250 ms. It stops
changing the slider while the user is scrubbing or while a seek completion is
pending.

Pointer dragging submits one seek on release. Other slider value changes are
debounced for 180 ms. Each submitted seek receives a serial number; completion
from an older seek or media generation is ignored. Player performs the actual
seek asynchronously and uses `Buffering` while the new generation is waiting
for valid output.

## Output color policy

The example uses `PreferHdr` but explicitly selects an opaque RGB10/PQ
composition layer. It follows the window's current monitor and
checks Windows Advanced Color state, SDR reference white, and display peak
luminance per frame. HDR source is preserved on an active HDR display and tone
mapped to SDR when Advanced Color is inactive. The Debug window reports the
actual output state; the source metadata alone is not treated as proof that
Windows is presenting an HDR layer.

## Diagnostics

The in-memory log is capped at 1,000 lines. Opening the Debug window displays
the existing history, enables callback cadence and
`D3D11StatisticsMode::Timing`, and starts a fresh measurement interval. Closing
it disables callback cadence and selects `D3D11StatisticsMode::Off`.

Every five seconds while Debug is open, the UI exchanges the atomic frame
counters and calls `D3D11VideoOutput::takeStatistics()`, which atomically
returns and resets output counters. Important interpretations are:

- low scheduled-video cadence usually points before the renderer: input,
  decode, clock, or Player presentation scheduling;
- normal scheduled cadence with low rendered cadence points at render
  contention, surface state, or the graphics driver;
- `present-busy` means non-blocking `Present()` found compositor backpressure;
- `decoder-copies` counts default raw NV12/P010 visible-region GPU copies and
  should rise with rendered hardware frames; it remains zero only in explicit
  direct decoder-texture mode or when hardware frames are not rendered;
- `coalesced` means multiple redraw notifications were intentionally combined;
- `no-frame/player-busy/renderer-busy` classifies unsuccessful attempts, while
  `renderer-busy(state/serialize/context/in-flight)` locates backend
  contention;
- `context-owner(reservation-aware/unreserved)` identifies an acquisition that
  still reached the renderer as busy after the proactive handoff, separating
  FFmpeg/internal owners from ordinary public owners;
- `handoff(wait/timeout)` records contention caught before the renderer and
  whether its bounded exchange succeeded, and
  `retry/superseded/terminal` distinguishes recovered attempts from frames that
  were actually lost;
- `render-skipped` is retained for compatibility and mirrors terminal drops;
- maximum color/interop/buffer/draw times identify the expensive D3D11 stage.

The format is intentionally human-readable and may change with diagnostics.

## Error handling

- Empty inputs are rejected on the UI thread.
- File-picker errors are shown in the status overlay and Debug log.
- Player open/decode/network failures arrive as media events and status
  transitions.
- Output failures are classified as a lost surface or general render error.
- A failed video-output initialization leaves playback unopened rather than
  silently playing audio with an unbound surface.
- HTTP(S) input uses bounded FFmpeg read-timeout and reconnect defaults.

## Shutdown sequence

Closing the main window invokes an idempotent ordered shutdown:

1. set `shuttingDown_` and clear `UiBridge::target` so queued callbacks become
   no-ops;
2. stop progress and seek timers;
3. close the Debug window;
4. request `State::Stopped`, which also makes active media I/O obsolete;
5. detach and close `D3D11VideoOutput`, draining GPU completion and the frame
   recycler, stopping its render/recycler workers, and releasing its Player
   render slot before either borrowed object disappears;
6. destroy Player, which signals its condition variables, interrupts FFmpeg
   I/O, joins control/demux, audio decode/output, video decode, and presentation
   workers, then closes the audio sink and media resources.

Teardown is synchronous by design. It may expose latency in an unresponsive
protocol or driver, but no worker is detached and no Player/output object is
allowed to outlive its dependencies. See
[ADR-005](ARCHITECTURE_DECISIONS.md#adr-005-deterministic-ordered-shutdown).

## Change boundaries

Changes that belong in this example include WinUI controls, surface-host
integration, UI marshalling, and presentation of diagnostics. General clock,
queue, codec, network-recovery, audio, rendering, hardware-interop, or shutdown
bugs belong in QtAVCore. If an integration change alters a decision here,
update `ARCHITECTURE_DECISIONS.md` in the same change.
