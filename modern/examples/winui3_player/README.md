# QtAVCore WinUI 3 player

This example is an unpackaged, self-contained C++/WinRT WinUI 3 application.
It uses the QtAVCore Windows backends directly:

- `QtAV::RenderD3D11` for a `SwapChainPanel`;
- `QtAV::HWD3D11VA` and `QtAV::InteropD3D11` for hardware decode and
  zero-CPU-map presentation, with the library's normal software fallback;
- `QtAV::AudioWASAPI` and `QtAV::AudioResample` for device audio;
- `qtav::Player` for local files, FFmpeg-supported URLs, play/pause, stop,
  seek, status, and media events.

The Debug toggle opens and closes a separate log window. QtAVCore state/status
callbacks run on the playback worker, while frame/render notifications run on
the presentation worker. The example marshals rendering and all WinUI updates
through the window's `DispatcherQueue`; device audio submission remains on the
independent audio-output worker.

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
- The slider seeks when the media is seekable. Live media keeps it disabled.
- **Debug** toggles the separate status/event log window.

URL playback depends on the protocols and TLS support enabled in the linked
FFmpeg build. QtAVCore does not yet provide a production reconnect or network
buffering policy, so failures are surfaced in the status area and Debug
window.
