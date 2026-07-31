# QtAVCore WinUI 3 player

This example is an unpackaged, self-contained C++/WinRT WinUI 3 application.
It uses the QtAVCore Windows output stack:

- `QtAV::OutputD3D11` owns the D3D11 device, composition swap chain, render
  target, display/HDR tracking, render scheduling thread, `renderVideo()`,
  and `Present()` for a `SwapChainPanel`;
- the output internally combines `QtAV::RenderD3D11`,
  `QtAV::HWD3D11VA`, and `QtAV::InteropD3D11` for zero-CPU-map hardware
  presentation with the library's normal software fallback;
- `QtAV::AudioWASAPI` and `QtAV::AudioResample` for device audio;
- `qtav::Player` for local files, FFmpeg-supported URLs, play/pause, stop,
  seek, status, and media events.

The example supplies its hosting HWND and
`ISwapChainPanelNative::SetSwapChain`; the library's default `PreferHdr`
policy creates an FP16 scRGB composition layer, tracks the window's current
monitor and Windows HDR setting per frame, and tone maps to SDR only when
Advanced Color is inactive. The Debug log reports whether the actual output
layer is HDR, its color representation, system SDR white, and display peak
luminance.

The Debug toggle opens and closes a separate log window. QtAVCore state/status
callbacks run on the playback worker, while frame/render notifications run on
the presentation worker. `D3D11VideoOutput` coalesces those notifications on
its own render thread and presents outside the WinUI dispatcher. The example
only provides an `ISwapChainPanelNative::SetSwapChain` binding callback,
attaches the player, and forwards panel size changes. UI state and log updates
are marshalled through `DispatcherQueue`; device audio submission remains on
the independent audio-output worker. While playback is active, the Debug
window also reports five-second cadence snapshots for scheduled and rendered
video rate, coalesced redraws, compositor-busy presents, retryable skipped
renders, gaps over 80 ms, maximum render/present time, and maximum
color/interop/buffer-update/draw stage time. These counters make a slow
decode/presentation cadence distinguishable from a blocking D3D11 driver
stage.

## Requirements

- Windows 10 1809 or newer;
- Visual Studio 2026 with Desktop C++ and WinUI development support;
- Windows SDK 10.0.26100;
- CMake 3.20 or newer;
- a shared QtAVCore build with FFmpeg 8+ and the Windows backends enabled.

The project pins Windows App SDK `2.3.1`. NuGet restore downloads the Windows
App SDK, C++/WinRT, and Windows SDK build packages on the first build.

## Build

From this directory:

```powershell
.\build.ps1 -Configuration Release
```

By default the script uses:

```text
<repository>\build\modern-shared
```

Override that location when necessary:

```powershell
.\build.ps1 `
  -Configuration Release `
  -QtAVBuildDir C:\path\to\build\modern-shared
```

The QtAVCore build directory must have been configured with
`BUILD_SHARED_LIBS=ON`. The script builds the required QtAV targets, restores
the WinUI NuGet packages, builds the application, and copies QtAVCore and
FFmpeg runtime DLLs beside the executable.

Run:

```powershell
.\bin\x64\Release\QtAVWinUI3.exe
```

You can also open `QtAVWinUI3.vcxproj` in Visual Studio. Set the MSBuild
property `QtAVBuildDir` when your shared QtAVCore build is not at the default
location.

## Controls

- **打开文件** selects a local media file.
- **播放 URL** opens the URL in the adjacent text box.
- **播放/暂停** changes playback state.
- The slider seeks when the media is seekable. Handled pointer events from the
  internal thumb are observed so one drag previews locally and submits exactly
  one asynchronous seek on release. While dragging or waiting for that seek,
  timer-based progress updates cannot move the thumb back to an obsolete
  position. Live media keeps it disabled.
- **Debug** toggles the separate status/event log window.

URL playback depends on the protocols and TLS support enabled in the linked
FFmpeg build. Seek/output starvation is exposed as `Buffering` with a frozen
timeline, and HTTP(S) playback uses bounded I/O timeout/reconnect defaults.
These are file-style HTTP recovery safeguards, not yet a complete adaptive or
live-stream packet-buffering policy; terminal failures remain visible in the
status area and Debug window.
