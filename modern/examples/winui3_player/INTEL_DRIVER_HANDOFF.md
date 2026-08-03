# Intel D3D11 driver upgrade investigation handoff

This document is the continuation point for testing the WinUI 3 player after
upgrading the Intel graphics driver. Keep the stable synchronization workaround
enabled until the asynchronous experiment completes successfully for both test
files.

## Stable checkpoint before the driver upgrade

- Branch: `codex/winui3-hdr-intel-stability`
- Adapter: Intel Iris Xe, `PCI\VEN_8086&DEV_A7A0`
- Driver and `igd10um64xe.dll`: `32.0.101.6733`
- Windows HDR display: active, 240-nit system SDR white, 1405-nit reported peak
- WinUI output: opaque `R10G10B10A2_UNORM`,
  `DXGI_COLOR_SPACE_RGB_FULL_G2084_NONE_P2020`
- Color authority: libplacebo D3D11
- Decode/interoperability: D3D11VA NV12/P010 raw-plane import; Dolby Vision
  keeps the private GPU-only decoder-slice copy

Before the workaround, both media failed in `igd10um64xe.dll` with exception
`0xc0000005` at offset `0x5e56b`:

- `C:\Users\zzzhr\Downloads\wednesday.mp4` — Dolby Vision Profile 5
- `C:\Users\zzzhr\Downloads\legend.mkv` — ordinary BT.2020/PQ HDR10

The private Dolby Vision GPU copy did not prevent the crash. The stable renderer
therefore calls `pl_gpu_finish()` after every successful Intel imported
hardware-frame submission. Non-Intel submissions and Intel software frames
retain the bounded asynchronous completion queue.

Stable runtime observations:

| Media | Observed run | Scheduled | Rendered | Result |
| --- | ---: | ---: | ---: | --- |
| `legend.mkv` | 01:12 | about 25 fps | mostly 23.3-24.4 fps | Normal close |
| `wednesday.mp4` | 02:05 | about 24 fps | mostly 21-23.5 fps; brief low near 16 fps | Normal close |

The user compared the RGB10/PQ output with MPC-BE on the same display and
confirmed matching brightness. The Release build and all 36 CTest tests passed.

## Start the next Codex window

Use this as the opening request:

> Continue the Intel driver upgrade investigation from
> `modern/examples/winui3_player/INTEL_DRIVER_HANDOFF.md`. First record the new
> Intel driver and `igd10um64xe.dll` versions, rebuild the existing stable
> branch without changing behavior, and replay both `legend.mkv` and
> `wednesday.mp4`. Do not remove the Intel synchronization workaround until the
> stable post-upgrade baseline passes.

The new session must also read the repository `AGENTS.md`, `modern/README.md`,
`modern/MIGRATION.md`, `modern/PLAN.md`, and project decision AD-007 in
`modern/DECISIONS.md` before editing.

## Capture the post-upgrade baseline

Record the new adapter and driver identity:

```powershell
Get-CimInstance Win32_VideoController |
  Select-Object Name, DriverVersion, DriverDate, PNPDeviceID

Get-ChildItem C:\Windows\System32\DriverStore\FileRepository -Recurse `
  -Filter igd10um64xe.dll -ErrorAction SilentlyContinue |
  Select-Object FullName, @{N='FileVersion';E={$_.VersionInfo.FileVersion}}, `
    LastWriteTime
```

Confirm Windows HDR remains enabled and the player reports `HDR active,
RGB10/PQ`. Rebuild and run the stable baseline:

```powershell
cmake --build build\winui3-libplacebo-shared-fresh --config Release --parallel
ctest --test-dir build\winui3-libplacebo-shared-fresh `
  -C Release --output-on-failure

modern\examples\winui3_player\build.ps1 `
  -Configuration Release `
  -QtAVBuildDir C:\vscode\QtAV\build\winui3-libplacebo-shared-fresh
```

Play `legend.mkv` first, then `wednesday.mp4`. Preserve Debug-window logs for
source color metadata, RGB10/PQ output, scheduled/rendered cadence,
`decoder-copies`, and maximum draw time. A useful stable baseline is at least
two minutes per file plus a normal process close.

Check for a new driver crash after each run:

```powershell
$since = (Get-Date).AddMinutes(-15)
Get-WinEvent -FilterHashtable @{LogName='Application'; StartTime=$since} |
  Where-Object { $_.Message -match 'QtAVWinUI3.exe|igd10um64xe.dll' } |
  Select-Object TimeCreated, Id, ProviderName, Message
```

## Asynchronous experiment after the stable baseline

Create a separate experiment branch. Change only the Intel completion condition
in `modern/backends/render/d3d11/src/d3d11_video_renderer.cpp`; do not change
RGB10/PQ output, libplacebo parameters, the Dolby Vision copy, decoder policy,
or queue depth in the same experiment.

The current stable condition is:

```cpp
if (impl_->intelDevice_ && imported) {
    pl_gpu_finish(impl_->d3d11_->gpu);
}
```

For the experiment, bypass only that finish and retain the existing bounded
completion-query queue. Rebuild every shared library and the WinUI executable
to avoid mixing old test executables or DLL ABI layouts.

Test in this order:

1. `legend.mkv` for at least five minutes, including resize and stop/replay.
2. `wednesday.mp4` for at least five minutes, including the original first
   six-second interval, stop/replay, and one seek.
3. Close normally and inspect the Application log after each file.
4. Compare scheduled/rendered fps and maximum draw time with the stable table.
5. Run all 36 CTest tests and `git diff --check`.

If either file reproduces `igd10um64xe.dll` failure, keep AD-007 and the stable
Intel completion condition unchanged. If both remain stable, extend each run
before proposing removal of the workaround; one short pass is insufficient
because earlier short automated runs were contradicted by later manual tests.

## Interpretation boundaries

- Brightness is already resolved by native RGB10/PQ and is independent of the
  Intel synchronization experiment.
- `wednesday.mp4` may initially log unknown container color fields; per-frame
  Dolby Vision RPU processing and active RGB10/PQ output determine the active
  rendering path.
- `decoder-copies` applies to Dolby Vision isolation and is not expected for
  ordinary HDR10 direct import.
- A clean device-removed reason or retained D3D11 query is not proof that the
  Intel UMD will not access-violate. Windows Application Error/WER records and
  sustained real playback are the acceptance evidence.
