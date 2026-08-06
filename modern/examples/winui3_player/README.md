# QtAVCore WinUI 3 player

This directory contains the Windows desktop player example for QtAVCore. It is
an unpackaged, self-contained C++/WinRT WinUI 3 application, not a reusable UI
control and not part of the legacy Qt player.

The example demonstrates:

- opening a local media file with the Windows file picker;
- opening an FFmpeg-supported URL;
- play, pause, stop, and asynchronous seek;
- a progress slider that does not issue a seek for every pointer movement;
- WASAPI audio with the device presentation clock as playback master;
- D3D11VA hardware decode and zero-CPU-map raw NV12/P010 presentation through
  libplacebo's D3D11 renderer when the media and device support them, with the
  library's software fallback;
- monitor-aware SDR/HDR composition output in a WinUI `SwapChainPanel`;
- an optional Debug window with playback, decode, render, device, and cadence
  diagnostics.

The D3D11 renderer defaults to a bounded same-device NV12/P010 texture ring. It
copies only the even-aligned visible decoder region, uses
`D3D11_COPY_DISCARD` on D3D11.1, and keeps the copied texture plus swap-chain
back buffer alive until GPU completion. Every successfully imported D3D11VA
frame uses libplacebo's fast sampling policy without an additional GPU
histogram peak-detection pass. Submission stays asynchronous without a
per-frame `pl_gpu_finish()`. Native D3D11 multithread protection, the shared
recursive guard, the bounded completion queue, and lifecycle drains remain
active. Direct decoder-texture sampling is available through the library's
explicit output option but is not enabled by this example. Software frames
keep the default render parameters.

The example's video surface is opaque, so it explicitly selects RGB10/PQ
presentation with `DXGI_ALPHA_MODE_IGNORE`. This bypasses the extra scRGB/DWM
conversion used by the general-purpose library default and matches the native
HDR10 presentation model commonly used by dedicated video renderers.

## Documentation

- [Architecture](ARCHITECTURE.md) describes components, threads, data flow,
  ownership, and shutdown.
- [Architecture decisions](ARCHITECTURE_DECISIONS.md) records the choices that
  should remain stable when the example evolves.
- [Testing](TESTING.md) contains build checks and the manual smoke matrix.
- [Local agent guide](AGENTS.md) contains scoped maintenance rules.
- [QtAVCore documentation](../../README.md) describes the library API and all
  supported backends.

## Runtime stack

| Responsibility | QtAVCore component |
| --- | --- |
| Media control, demux, decode, clocks, and queues | `qtav::Player` |
| Device audio | `QtAV::AudioWASAPI` |
| Audio format conversion | `QtAV::AudioResample` |
| High-level Windows video output | `QtAV::OutputD3D11` |
| Video rendering | `QtAV::RenderD3D11` |
| Hardware decoding | `QtAV::HWD3D11VA` |
| Hardware-frame interop | `QtAV::InteropD3D11` |
| Shared Windows D3D11 device access | `QtAV::PlatformWindows` |

`qtav::D3D11VideoOutput` owns the D3D11 device, composition swap chain,
render target, reason-aware render scheduling thread, bounded retry/handoff,
`Present()`, resize, display/HDR tracking, and teardown. The application
supplies only its HWND and
an `ISwapChainPanelNative::SetSwapChain` binding callback, attaches the
`qtav::Player`, and forwards panel-size changes.

Playback and device work do not run on the WinUI dispatcher. Player and output
callbacks copy only the data needed by the UI and enqueue it through
`DispatcherQueue`. For the complete threading contract, see
[Architecture](ARCHITECTURE.md#thread-model).

## Requirements

- Windows 10 version 1809 (build 17763) or newer;
- an x64 development environment;
- Visual Studio 2026 with Desktop C++ and WinUI development support;
- Windows SDK 10.0.26100;
- CMake 3.20 or newer;
- FFmpeg 8 or newer;
- a shared QtAVCore build with the required Windows backends enabled.

The project currently pins these NuGet packages:

- Microsoft Windows App SDK `2.3.1`;
- Microsoft C++/WinRT `3.0.260715.1`;
- Microsoft Windows SDK Build Tools `10.0.26100.8249`.

NuGet restore downloads them on the first build. The application is
self-contained with respect to Windows App SDK, but the build still copies the
QtAVCore and FFmpeg DLLs beside the executable.

## Build

From the repository root, configure a shared QtAVCore build once:

```powershell
cmake -S modern -B build/modern-shared `
  -DBUILD_SHARED_LIBS=ON `
  -DQTAV_CORE_BUILD_TESTS=ON `
  -DQTAV_CORE_BUILD_EXAMPLES=ON
```

Then build the player:

```powershell
Set-Location modern/examples/winui3_player
./build.ps1 -Configuration Release
```

By default, `build.ps1` uses `<repository>\build\modern-shared`. Override the
location when necessary:

```powershell
./build.ps1 `
  -Configuration Release `
  -QtAVBuildDir C:\path\to\build\modern-shared
```

The script:

1. verifies that `QtAVBuildDir` is a shared QtAVCore CMake build;
2. builds the core and required Windows backend targets;
3. restores NuGet packages and builds the x64 WinUI project;
4. copies QtAVCore and FFmpeg runtime DLLs beside the executable.

Run the Release build with:

```powershell
./bin/x64/Release/QtAVWinUI3.exe
```

Use `-Configuration Debug` for the Debug configuration. `bin/`, `obj/`,
`Debug/`, `Release/`, generated XAML files, NuGet packages, and Visual Studio
state are intentionally ignored by this directory's `.gitignore`.

### Visual Studio

You can open `QtAVWinUI3.vcxproj` directly. The project only defines x64 Debug
and Release configurations. Set the `QtAVBuildDir` MSBuild property if the
shared QtAVCore build is not at the default location. A link-time validation
error normally means that the matching QtAVCore configuration has not been
built yet.

## Controls

- **打开文件** selects and immediately plays a local media file.
- **播放 URL** or Enter in the URL box opens and plays the entered URL.
- **播放/暂停** toggles between playing and paused states.
- **停止** requests an asynchronous playback stop.
- The slider seeks only when the media reports a finite, seekable duration.
  Dragging previews the position locally and submits one seek on release;
  keyboard or other value changes are debounced for 180 ms.
- **Debug** opens or closes the separate diagnostics window. Closing that
  window also clears the toggle.

URL playback depends on the protocols and TLS support enabled in the linked
FFmpeg build. HTTP(S) inputs receive bounded I/O timeout and reconnect
defaults. These safeguards suit file-style HTTP playback; they are not a full
adaptive-streaming or live-stream buffering policy.

## Debug window

The Debug window keeps the most recent 1,000 lines and reports:

- state and media-status transitions;
- media and renderer errors;
- selected streams, dimensions, sample rate, pixel/sample formats, color
  metadata, and whether video decode is hardware or software;
- D3D11 device and actual SDR/HDR output information;
- first decoded audio/video frame and first presented video frame;
- five-second cadence snapshots for scheduled video, decoded audio, render
  requests/passes, presented frames, coalesced redraws, busy presents,
  reason-level no-frame/Player/renderer contention, retry wakeups, superseded
  and terminal frames, D3D11 context-owner and handoff counts, gaps over 80 ms,
  and maximum render-stage times. `render-skipped` is the compatibility mirror
  of terminal drops; a recovered retry is not counted as skipped.

The cadence line helps distinguish decode or network starvation from a slow
render/driver stage. It is diagnostic evidence, not a stable machine-readable
log format.

## Known limitations

- This is a focused example, not a production media-browser UI.
- There are no subtitle controls or post-load track selectors.
- There is no adaptive bitrate policy, download cache, or detailed network
  buffering UI.
- Only Windows x64 Debug and Release project configurations are supplied.
- Debug also requires ABI-compatible Debug variants of the repository FFmpeg
  dependencies. The current release-only Windows artifact has no Debug
  `placebo.lib`; ClangCL correctly rejects mixing its
  `_ITERATOR_DEBUG_LEVEL=0` library with Debug level 2 objects. Use Release for
  that artifact rather than forcing an ABI override.
- Debug logging is in-memory and is not written to disk.
- Closing the main window performs deterministic synchronous teardown. Active
  FFmpeg I/O is interrupted and workers are joined, so a broken protocol or
  graphics/audio driver can still make shutdown visibly slower than an idle
  close. See [ADR-005](ARCHITECTURE_DECISIONS.md#adr-005-deterministic-ordered-shutdown).

## Troubleshooting

- **No QtAVCore import library:** configure `BUILD_SHARED_LIBS=ON`, build the
  same Debug or Release configuration, or pass the correct `QtAVBuildDir`.
- **A DLL is missing at launch:** rebuild through `build.ps1`; it copies all
  DLLs from `<QtAVBuildDir>\bin\<Configuration>` to the app output directory.
- **A URL fails but a local file works:** check the linked FFmpeg protocol/TLS
  features and inspect the `open` or error event in the Debug window.
- **Video is black or the surface is lost:** inspect the renderer event and
  D3D11 device line, then reproduce after resize and monitor movement.
- **Playback stutters or audio is noisy:** capture at least two cadence lines
  and note media URL/type, seek history, output color mode, CPU/GPU load, and
  whether the audio endpoint changed. Follow the evidence guide in
  [Testing](TESTING.md#diagnosing-cadence-and-stalls).
- **Exit is slow:** reproduce while the Debug window is closed and record
  whether the delay occurs for local media, HTTP media, or both. Do not work
  around it by detaching QtAVCore threads; teardown must continue to join all
  owners safely.
