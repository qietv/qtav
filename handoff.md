# QtAVCore Intel D3D11 performance handoff for administrator machine

## Status

The Intel Windows regression is **open and not fixed**. It has been transferred
to another Intel Windows machine for administrator-level WPR/GPUView tracing,
root-cause confirmation, repair, and validation. Do not close it merely because
`render-skipped`, `terminal`, and `decoder-copies` remain zero.

The original `legend.mkv` run overlapped unrelated compilation and the user
observed CPU thermal throttling, so that run's severity is not a clean
baseline. A later run with `suzume.mkv`, no build processes, normal processor
performance/frequency counters, and Debug hidden still reproduced intermittent
post-seek draw overruns, render gaps, Present backpressure, and occasional
five-second cadence loss. Thermal throttling may have amplified the first run,
but it is not a sufficient root-cause explanation.

An administrator WPR follow-up on 2026-08-05 used the Lenovo internal
2880x1800/60-Hz SDR panel because the Philips display was not active. It
captured and explained one 57.9-ms seek-transition frame, but it did **not**
reproduce the defining issue: repeated settled 40-49-ms `pl_render_image()`
calls after seeking. That trace is useful transition evidence, not a successful
reproduction or resolution of the open regression. The next run will use the
Philips 4K HDR display in a new conversation window.

The originating machine now proceeds with OHOS development in parallel. The
transferred Windows task owns only the unfinished Intel investigation and must
merge its evidence and fix back without discarding the OHOS work. The complete
chronological record is in [`modern/PLAN.md`](modern/PLAN.md), especially
`Next task`, `Intel checkpoint on 2026-08-05`, `Intel clean-load follow-up on
2026-08-05`, and `Intel D3D11 pass-submission follow-up on 2026-08-05`.

## Repository state

- Repository: `C:\vscode\QtAV`
- Historical handoff baseline: `61afd1e4508e4817b9de4c8419df12880d19eb37`.
  The branch has advanced since this file was created; run `git rev-parse HEAD`
  and `git status --short` at the start of the next window rather than resetting
  to this hash.
- Intel diagnostic instrumentation entered history in
  `8ce66844c7213dda54178b3d792b9161f426f11f`. Later commits primarily advance
  the parallel OHOS work.
- Active implementation: `modern/` (QtAVCore), not the legacy root QtAV tree.
- The shared worktree can contain separate uncommitted OHOS source,
  documentation, CMake, example, and interop changes. These are not part of the
  Intel diagnosis. Inspect the live status, then do not discard, fold into an
  Intel-only commit, or edit them unless the OHOS task explicitly requires it.
- This `handoff.md` is intentionally at the repository root as the next-chat
  entry point.
- No production source fix has been made for the Intel result. The source
  changes are diagnostics only and preserve asynchronous submission.

Read before editing:

1. `AGENTS.md`
2. `modern/README.md`
3. `modern/MIGRATION.md`
4. `modern/PLAN.md`
5. `modern/examples/winui3_player/AGENTS.md`
6. `modern/examples/winui3_player/TESTING.md`

## Known-good build and executable

The instrumented Windows binary used for the administrator trace contains the
diagnostic statistics now recorded in commit `8ce6684`:

- CMake tree: `C:\vscode\QtAV\build\modern-shared-intel`
- WinUI executable:
  `C:\vscode\QtAV\modern\examples\winui3_player\bin\x64\Release\QtAVWinUI3.exe`
- Visual Studio 2026, ClangCL 22.1.3, shared Release
- repository `x64-windows-static-md` FFmpeg/libplacebo package
- complete CTest result: 36/36 passed
- WinUI Release build: zero MSBuild warnings and errors
- the final WinUI build restored the normal HDR-preferred RGB10/PQ path after a
  temporary `SdrOnly` A/B

Reference SHA-256 values for the exact binaries used by the administrator
internal-SDR trace are:

| Binary | SHA-256 |
| --- | --- |
| `QtAVWinUI3.exe` | `2C84EED7431E097ED6E51C54FB022B7410E821D25754586C2F2D7520D3E4F705` |
| `qtav_render_d3d11.dll` | `E232B7B0053CB47BE159D8119AC88F5425DE93CA7CF84C6F53441B20CFF0D3E7` |
| `qtav_output_d3d11.dll` | `C74CC47D45829486D68E3A13799DFE63E400D0377F385033B0A13354CF8A2FA3` |
| `qtav_hw_d3d11va.dll` | `3953ACDCC4169E9B301911152B05701F3F67BFD0ECE5F37ED1B629813C8E1F07` |
| `qtav_interop_d3d11.dll` | `CF85FE42E364E6D25F192E57E67C33FE0C186FFFC73A5A57CE27F6D6513F0427` |
| `qtav_core.dll` | `4909AE3EAF97B7FCE6FC1FCA562CD0624A38E415787AFB28D4E158DA390E18BE` |

Verify these before reusing the existing player. If they differ, record the new
hashes and do not compare the resulting trace as though it used the same
binary baseline.

Fresh configure command:

```powershell
cmake -S modern -B build/modern-shared-intel `
  -G "Visual Studio 18 2026" -A x64 -T ClangCL `
  -DCMAKE_TOOLCHAIN_FILE="C:/vscode/QtAV/ffmpeg/vcpkg/scripts/buildsystems/vcpkg.cmake" `
  -DVCPKG_INSTALLED_DIR="C:/vscode/QtAV/ffmpeg/build/x64-windows-static-md/vcpkg_installed" `
  -DVCPKG_TARGET_TRIPLET=x64-windows-static-md `
  -DBUILD_SHARED_LIBS=ON `
  -DQTAV_CORE_BUILD_TESTS=ON `
  -DQTAV_CORE_BUILD_EXAMPLES=ON
```

Build and test:

```powershell
cmake --build build/modern-shared-intel --config Release --parallel
ctest --test-dir build/modern-shared-intel -C Release --output-on-failure

Set-Location modern/examples/winui3_player
./build.ps1 `
  -Configuration Release `
  -QtAVBuildDir C:\vscode\QtAV\build\modern-shared-intel
```

Do not let CMake fall back to `C:\vcpkg\vcpkg\installed\x64-windows`; that
prefix lacks the required repository D3D11/libplacebo dependency closure.

## Intel test system

- Lenovo 21HW
- Intel Core i5-13500H, 12 cores / 16 logical processors
- 31.9 GB system memory
- Intel Iris Xe Graphics
  - `PCI\VEN_8086&DEV_A7A0&SUBSYS_3C4817AA`
  - driver `32.0.101.7088`
- Windows 25H2 build `26200.8894`
- balanced power plan, AC online
- primary Philips `PHL0979` display at 3840x2160/60 Hz
- Windows HDR active
- WinUI composition surface: 1708x814
- output: RGB10/PQ, 240-nit SDR white, 1405-nit display peak

Temperature telemetry was not available during the clean run. Processor
performance, frequency, queue depth, and GPU-engine counters were captured.
Use HWiNFO or equivalent sensor logging in the next run if available, but do
not replace the application cadence/stage counters with subjective thermal
observations.

## Media and exact reproduction scenes

### Primary clean-load reproduction: `suzume.mkv`

Path:

```text
C:\Users\zzzhr\Downloads\suzume.mkv
```

Media:

- 3840x1608 HEVC Main 10 / yuv420p10le
- limited-range BT.2020/PQ
- 24 fps
- 5.1 E-AC-3
- duration 2:01:22

Protocol:

1. Stop unrelated builds and confirm no active `cl`, `clang-cl`, `link`,
   `msbuild`, `cmake`, `ninja`, or Gradle process.
2. Start the Release WinUI executable cold.
3. Open `suzume.mkv` through **Open file** and open Debug.
4. Let the beginning play for at least 60-120 seconds and capture at least two
   consecutive settled cadence lines.
5. Pause for five seconds and resume.
6. Seek to **1:00:00**, then observe for at least 30-60 seconds.
7. Close Debug for at least 30 seconds, reopen it, and inspect the retained log.
8. Seek to **1:40:00**, then observe for at least 30-60 seconds.
9. Close the main window while playing and verify deterministic process exit
   plus the absence of Application Error / Windows Error Reporting events.

Clean-run result before seeking:

- more than 110 seconds at 23.8-24.2 scheduled/rendered fps
- warm draw normally 12-15 ms
- zero coalescing, Present busy, render-skipped,
  retry/superseded/terminal, decoder copies, and warm >80-ms gaps
- CPU utility 15.18%, processor performance 112.88%, average frequency
  2343 MHz with a 2600-MHz sample maximum, processor queue 0.08
- GPU 3D 3.81%, video decode 5.01%, copy 0.00%

Result after seek to 1:00:00:

- most windows remained at 23.8-24.1 rendered fps
- repeated roughly 41-48-ms draw maxima
- repeated 84-95-ms render gaps and intermittent Present busy
- one window rendered 23.3 fps
- with Debug hidden, one later window rendered 23.4 fps with a 161-ms draw and
  212-ms render gap
- Debug-hidden counters showed CPU utility 9.86%, processor performance
  114.32%, average frequency 2445 MHz with a 2600-MHz maximum, zero processor
  queue, GPU 3D 4.75%, video decode 6.13%, and copy 0.00%

Result after seek to 1:40:00:

- transition window: 22.6 fps, 70.8-ms draw, 197-ms render gap
- later windows returned to 23.9-24.0 fps
- intermittent 41-43-ms draw maxima, 82-102-ms render gaps, and occasional
  Present busy remained
- Player scheduling stayed at source cadence
- `render-skipped=0`, `retry/superseded/terminal=0/0/0`, and
  `decoder-copies=0`

Closing while playing exited in about 231 ms without an application error.

### Earlier HDR10 reproduction: `legend.mkv`

Path:

```text
C:\Users\zzzhr\Downloads\legend.mkv
```

Media: 3840x2160 HEVC Main 10 / yuv420p10le, limited-range BT.2020/PQ,
25 fps, 5.1 E-AC-3.

Reproduction scene: seek to **22:48**.

The first run produced persistent 24.0-24.8 rendered fps despite source-rate
scheduling, 48-60-ms draw maxima, recurring Present busy/coalescing, and
98-175-ms render gaps. A dedicated cold rerun usually retained 24.9-25.2 fps
but kept 49-52-ms draw, recurring 84-131-ms gaps, and later 155-ms draw with
176-177-ms gaps. The original severity may have been amplified by thermal and
compilation load; use `suzume.mkv` as the primary clean baseline.

### Passing controls

- `C:\Users\zzzhr\Downloads\wednesday.mp4`: 3840x2160 HEVC Main 10 Dolby
  Vision Profile 5 at 24000/1001 fps. Cold, sustained, seek to 29:39, media
  replacement, and close passed at source cadence with warm draw normally
  12-16 ms and all loss/backpressure counters zero.
- `C:\test\qtav-h264-nv12-control-1080p.mp4`: generated 120-second H.264/NV12
  1080p control at 30000/1001 fps. Sustained playback and seek to 00:59 passed
  at 29.8-30.1 fps with all loss/backpressure counters zero.
- No independent 4K SDR control was available.
- No Intel trusted-player comparison has been completed.

## Latest pass-level diagnosis

The current diagnostic build adds no finish/wait or decoded-surface copy. It
splits renderer CPU wall time across completion retirement/acquisition,
render-target clear, `pl_render_image()`, completion `End()`, and retained
resource insertion. A libplacebo render-info callback records pass count,
pass-graph changes, asynchronous rolling GPU time, callback arrival, and time
after the last callback.

Two clean RGB10/PQ runs of `suzume.mkv` reproduced the issue:

- before seeking, playback held 23.8-24.2 fps; warm `pl_render_image()` time
  was normally 11-16 ms and the single pass reported about 1.3-2.1 ms of
  rolling GPU time;
- after one pointer seek to approximately 1:00:00, repeated windows showed
  about 40-49 ms inside `pl_render_image()`, 82-98-ms render gaps, occasional
  Present busy/coalescing, and one separate roughly 182-ms Present;
- every settled slow window still used one pass, reported zero pass-graph
  changes, and showed only about 1.7-1.9 ms of rolling GPU time. Shader/pass
  churn and genuinely long GPU execution therefore do not explain the
  recurring 40-49-ms CPU wall time;
- the successful-pass callback arrived only after the complete 40-45-ms slow
  interval, while time from the callback to `pl_render_image()` return was
  effectively zero. The stall is before the callback in libplacebo's D3D11
  pass execution/timer-query path, not QtAV scheduling or post-pass cleanup;
- the seek transition itself may report one pass-graph change. Later recurring
  slow frames do not, so the transition change is not the persistent cause.

A temporary `SdrOnly` BGRA8/80-nit run reproduced post-seek pass CPU maxima of
roughly 42-86 ms while rolling GPU time stayed about 1.4-1.9 ms. Occasional
`ClearRenderTargetView()` maxima of 20-34 ms appeared in the same later
interval. RGB10/PQ, HDR metadata, and the HDR output shader are not necessary
conditions. The source and final executable were restored to RGB10/PQ after
this A/B.

libplacebo 7.351.0 executes the affected raster pass through its GPU timer
start, dynamic vertex/index stream-buffer upload (`Map` with
`WRITE_NO_OVERWRITE` or `WRITE_DISCARD`, then `Unmap`), immediate-context state
binding, `Draw`, resource unbinding, timer `End`, and non-blocking timer-query
poll. QtAV-level timings cannot yet distinguish the exact blocking call.

The progress-slider suspicion is closed as a cause:

- pointer press enters scrubbing; pointer release or capture loss exits
  scrubbing and commits one seek; passive hover has no seek handler;
- a real mouse press/release was used rather than directly assigning the slider
  value. The pointer remained at the released slider coordinate for about 45
  seconds and the log contained exactly one seek;
- moving the pointer into the video area did not remove the recurring stalls;
- a missed release would instead leave the progress display in scrubbing mode
  and omit the committed seek. It would not generate this repeated D3D11 pass
  pattern.

## Administrator WPR follow-up: internal SDR control

An elevated WPR GPU trace was successfully captured on 2026-08-05. The Philips
display was not active, so this was an exploratory control on the Lenovo
internal 2880x1800/60-Hz SDR display rather than the required final environment.
The WinUI surface was 2282x1091 and the output was BGRA8/80-nit SDR. D3D11VA,
HEVC Main 10/P010, BT.2020/PQ input, and direct imported-frame rendering stayed
active.

Artifacts are outside the repository under `C:\QtAVTraces`:

- primary ETL:
  `qtav-intel-suzume-005956-internal-sdr-20260805.etl`
  (3,236,954,112 bytes, 125.7455 seconds, zero lost buffers/events);
- one-millisecond CPU activity export:
  `activity-48.3s-50.5s-1ms.csv`;
- exact slow-range event dump: `dump-48629-48687.csv`;
- Direct3D11-provider export for the full trace: `d3d11-full.csv`;
- render-thread stack reports:
  `qtav-render-21344-48.629-48.687-stack.html` and
  `qtav-render-21344-48.629-48.687-profileevent-stack.html`;
- symbol retry report:
  `symbol-retry-render-48.629-48.687.html`, with failures recorded in the
  sibling `-errors.txt` file.

One real pointer seek committed `seek: 59:56`. The transition window reported
23.4 scheduled fps, 22.6 rendered fps, five coalesced redraws, 233.6/248.7-ms
gaps, 57.9-ms render, 55.6-ms draw, and 41.8 ms in `pl_render_image()`. The next
windows returned to approximately 24 fps. The historical repeated settled
40-49-ms post-seek calls did not occur on this panel.

The 58-ms render interval is 48.629-48.687 seconds in the ETL. Render thread
TID 21344 consumed about 33.644 ms of CPU and was not running for about
24.356 ms. The slow frame was reconstructed as follows:

- at 48.631219 D3D11 created a new 256x1 R32_FLOAT tone-mapping LUT;
- libplacebo `pl_gamut_map_generate()` created short-lived worker threads; the
  render thread accumulated about 15.35 ms of `Waiting/UserRequest` while those
  workers generated the gamut data;
- from 48.647908 through 48.650215, 108 DemandZero events at
  `qtav_render_d3d11.dll+0x3EA22` span exactly 3,145,728 bytes. Disassembly and
  embedded assertion strings identify this instruction as the output write in
  libplacebo `src/shaders/colorspace.c:fill_gamut_lut()`. The byte count is
  exactly the default 48x32x256, four-component, 16-bit gamut LUT;
- D3D11 created the new 48x32x256 R16G16B16A16_UNORM texture at 48.652516;
- the new LUT's paging packet completed in about 0.130 ms, so its GPU upload was
  not the long wait;
- the same transition destroyed the old 3840x1664x24 P010 decoder texture pool.
  VidMm uncommit/evict work caused about 4.584 ms of `Waiting/Executive`;
- DXGI Present itself was 48.672504-48.672824, only 0.320 ms. The actual GPU
  DmaPacket ran 48.672982-48.674152, about 1.170 ms, followed by an independent
  flip.

Across the complete trace, the 48x32x256 gamut texture was created only once
and one older gamut texture was destroyed. The tone LUT was likewise replaced
only once. This proves that the captured spike was a seek/reconfiguration
transition composed mainly of CPU LUT generation plus old decoder-pool
retirement. It was not a long shader, Present block, or persistent GPU queue
stall. It cannot explain the repeated settled 40-49-ms calls recorded in the
Philips runs.

Two interpretation cautions follow from this trace:

- the current `graph-change` counter does not expose LUT resource replacement;
- libplacebo's reported rolling GPU timer is the latest completed asynchronous
  query and can be stale. The ETL independently confirms approximately 1-2 ms
  of GPU work for this transition frame, but future settled slow frames still
  require ETW correlation rather than trusting `pass->last` alone.

## What the evidence currently excludes

Do not restart diagnosis from these already-tested explanations unless new
evidence contradicts them:

- not input/decode starvation: `scheduled-video` stays at source cadence
- not decoded-source CPU mapping or copying: D3D11VA remains active and
  `decoder-copies=0`
- not a terminal retry-semantics loss: `render-skipped=0` and `terminal=0`
- not GPU saturation: Intel 3D/video-decode/copy utilization remains low
- not CPU build saturation in the clean run: no build processes, normal
  performance/frequency counters, zero or near-zero queue depth
- not caused by the Debug window: reproduced with Debug closed
- normally not time spent in color setup, interop, or buffer update: those
  stages remain around 0-2 ms while `draw` grows
- normally not a blocking `Present()` call: most long windows report roughly
  0.3-0.6-ms Present duration, although one clean-run window recorded a
  separate 166-ms Present maximum and should be investigated
- not passive mouse hover over the released progress slider
- not pass/shader-graph churn after the seek transition: slow settled windows
  retain one pass and zero graph changes
- not a genuinely 40-86-ms GPU shader workload: rolling GPU pass time remains
  about 1.4-1.9 ms during the reproduced CPU stalls
- not specific to RGB10/PQ or HDR output: `SdrOnly` BGRA8 reproduces it

The first measured stage that exceeds the frame budget is usually the D3D11
pass path before libplacebo's successful-pass callback inside
`pl_render_image()`. The leading hypothesis is CPU-side Intel immediate-context
or user-mode-driver backpressure, but the exact D3D11 call is not proven.
Present busy and coalescing often appear as downstream consequences.

## Source navigation

- `modern/backends/render/d3d11/src/d3d11_video_renderer.cpp`
  - around lines 1560-1735: retirement, color/source/interop/buffer timing
  - around lines 1736-1815: clear, `pl_render_image()`, libplacebo pass probe,
    completion-query `End`, and in-flight retention
- `modern/backends/output/d3d11/src/d3d11_video_output.cpp`
  - around lines 1030-1110: context reservation/handoff and
    `Player::renderVideoDetailed()`
  - around lines 1210-1280: retry classification and non-blocking
    `Present(1, DXGI_PRESENT_DO_NOT_WAIT)`
- `modern/examples/winui3_player/MainWindow.xaml.cpp`
  - around lines 810-985: five-second cadence/statistics reporting
  - around lines 1080-1190: `D3D11VideoOutput` setup; the sample selects
    RGB10/PQ and an opaque swap chain
- `modern/backends/output/d3d11/include/qtav/d3d11_video_output.h`
  - output A/B controls: `outputPreference`, `hdrPresentationMode`,
    `bufferCount`, and `configureHardwareDecoding`
- `modern/platform/windows/`: shared D3D11 device/context access and bounded
  handoff implementation
- `modern/core/src/player.cpp`: immutable render snapshot and
  `renderVideoDetailed()` result classification

Relevant commits for comparison:

```text
dc23a09 Fix transient D3D11 render drops
a62b423 Relax imported D3D11 frame workarounds
737851c Generalize AD-007 D3D11VA workaround
85febe7 Remove obsolete Intel driver handoff
ac3ca93 Fix WinUI3 Intel HDR rendering
c9c89dc Route Windows D3D11 color through libplacebo
```

## Symbol cache and limits

Keep all trace and symbol artifacts outside the repository:

- Microsoft PDB downstream cache: `C:\QtAVTraces\symbols`
- xperf/WPA symcache: `C:\QtAVTraces\symcache`
- successful retry report:
  `C:\QtAVTraces\symbol-retry-render-48.629-48.687.html`
- retry diagnostics:
  `C:\QtAVTraces\symbol-retry-render-48.629-48.687-errors.txt`

Use these settings before opening or exporting a trace:

```powershell
$env:_NT_SYMBOL_PATH = @(
  'srv*C:\QtAVTraces\symbols*https://msdl.microsoft.com/download/symbols'
  'C:\vscode\QtAV\modern\examples\winui3_player\bin\x64\Release'
  'C:\vscode\QtAV\build\modern-shared-intel\bin\Release'
) -join ';'
$env:_NT_SYMCACHE_PATH = 'C:\QtAVTraces\symcache'
```

The target slow-frame stack's important Microsoft symbols are already cached
and load successfully:

| PDB | Cached symbol-server index |
| --- | --- |
| `d3d11.pdb` | `0B6BA9C93E14B5272985D031DDE8B8451` |
| `dxgi.pdb` | `BFCAB1F776380FC5672C4846BC4D15DD1` |
| `dxgkrnl.pdb` | `66623333773BF1D2CE7E1B23804E969E1` |
| `dxgmms2.pdb` | `BF12E6970158C5CF34CEC193303E99261` |
| `ntkrnlmp.pdb` | `2A6E73CB5CCE9CCDA64E151FAC38D45C1` |

`ntdll.pdb`, `kernelbase.pdb`, `ucrtbase.pdb`, DbgHelp, and the other system
stack dependencies are also present in the cache. The retry report completed
successfully; the remaining relevant failures are unavailable files rather
than network timeouts.

The installed Intel 32.0.101.7088 binaries identify the following private PDBs:

| Module | PDB | GUID plus age / symbol index |
| --- | --- | --- |
| `igd10um64xe.dll` | `igd10um64xe.pdb` | `BDB5499CA2EC427389EE8B9FA574491A1` |
| `igdgmm64.dll` | `igdgmm64.pdb` | `36048D7719CB4FD5A23C496F545F4F3A1` |
| `igdkmdn64.sys` | `igdkmdn64.pdb` | `20C0890D892043628EFBCA67AE33D6911` |
| `igd10iumd64.dll` | `igd10iumd64.pdb` | `3F37355B06A54D42878DB22855031C8B1` |
| `igd11dxva64.dll` | `igd11dxva64.pdb` | `BB745E5E76544B78B617F7CE7D7DA1E31` |
| `igddxvacommon64.dll` | `igddxvacommon64.pdb` | `BF5CC827517F4274BF835912B4E3049F1` |

Direct requests for the first three exact indexes, including raw `.pdb`,
compressed `.pd_`, and `file.ptr` forms, returned HTTP 404 from the
[Microsoft public symbol server](https://learn.microsoft.com/en-us/windows-hardware/drivers/debugger/microsoft-public-symbols).
Intel's
[official driver-binary service](https://www.intel.com/content/www/us/en/developer/topic-technology/gamedev/graphics-drivers.html)
explicitly provides matching DLLs but not PDB files. Longer timeouts will not
resolve Intel internal function names. Keep Intel frames as `module+RVA` unless
Intel support supplies the matching private PDBs. The exact 7088 user-mode
binaries remain installed under
`C:\Windows\System32\DriverStore\FileRepository\iigd_dch.inf_amd64_046f5bd4083e9122`;
Intel's hosted copies match their sizes.

The current QtAV `Release` DLLs, including `qtav_render_d3d11.dll`,
`qtav_core.dll`, and `qtav_hw_d3d11va.dll`, have empty PE debug directories and
therefore no CodeView GUID/age that a symbol server can satisfy. The local
`QtAVWinUI3.pdb` does match its executable, but it cannot symbolize the QtAV
DLLs. A newly generated PDB can never symbolize the old ETL: the binary and PDB
must be built together and retained with the new trace.

For the Philips retest, first preserve a reproduction with the known Release
binary. If deeper QtAV/libplacebo address resolution is needed, create a
separate **optimized** symbol-enabled build, verify that every DLL contains a
matching CodeView record/PDB, and capture a second ETL with exactly those
binaries. Do not substitute an unoptimized Debug run for the cadence baseline.

## Philips 4K HDR retest and investigation order

The administrator trace path is now proven. The next conversation should focus
on reproducing the original recurring failure on the Philips display, not on
re-analyzing the already-explained internal-panel transition spike.

1. In the new conversation, read `AGENTS.md` and this file, then run
   `git rev-parse HEAD` and `git status --short`. Preserve all parallel OHOS
   changes. Do not edit `modern/PLAN.md` merely for a diagnostic capture.
2. Prefer the already validated instrumented Release binaries and verify their
   SHA-256 values against the table above. Confirm no `cl`, `clang-cl`, `link`,
   `msbuild`, `cmake`, `ninja`, Gradle, or unrelated WPR session is active. If a
   rebuild is unavoidable, record that the binary baseline changed, run all
   configured CTest tests (36/36 for the recorded baseline), and rebuild the
   WinUI player with zero errors.
3. Make `PHL0979` the active presentation display and record all of these before
   playback:
   - 3840x2160 at 60 Hz;
   - Windows HDR enabled;
   - Intel Iris Xe A7A0 and driver 32.0.101.7088;
   - AC power, balanced plan, and no thermal/build load;
   - WinUI surface size;
   - RGB10/PQ output, SDR reference white, and display peak. The earlier target
     values were 1708x814, 240 nits, and 1405 nits, but record the live values
     rather than forcing stale ones.
4. Set `_NT_SYMBOL_PATH` and `_NT_SYMCACHE_PATH` as documented above. In an
   elevated PowerShell, start a fresh GPU trace only after confirming no other
   recording owns the session:

   ```powershell
   wpr -status
   New-Item -ItemType Directory -Force C:\QtAVTraces
   wpr -start GPU -filemode -recordtempto C:\QtAVTraces
   ```

   Do not cancel another user's recording. Stop or cancel only a stale session
   known to belong to this QtAV investigation.
5. Cold-open `C:\Users\zzzhr\Downloads\suzume.mkv`, open Debug, and let the
   beginning play for 60-120 seconds. Capture at least two consecutive settled
   cadence lines showing D3D11VA, one libplacebo pass, zero decoder copies, and
   normal warm timings. Pause for five seconds and resume before seeking.
6. Retain at least 5-10 seconds of settled playback in the trace, perform one
   ordinary pointer seek to 1:00:00, release the pointer, move it away from the
   slider, and continue for at least 60 seconds. Save the complete Debug log and
   screenshots of the relevant five-second windows. Stop to a unique file, for
   example:

   ```powershell
   wpr -stop C:\QtAVTraces\qtav-intel-suzume-philips-hdr-010000.etl
   ```

   Repeat 1:40:00 only after the first capture is valid, and never overwrite an
   ETL. Close the player while playing after the final capture and verify a
   deterministic exit.
7. A seek-transition spike is expected and is **not** sufficient reproduction.
   First identify and exclude any frame that creates the 48x32x256 gamut LUT,
   replaces the 256x1 tone LUT, or destroys the old P010 decoder pool. The
   primary trace is successful only if settled playback later contains repeated
   40-86-ms `pl_render_image()` or Clear/Draw intervals without those transition
   events, ideally with the associated 80-ms-or-longer render gaps or cadence
   loss. If the Philips run remains stable, record non-reproduction instead of
   manufacturing a fix from the internal-panel transition.
8. For each settled slow frame, correlate the exact render-thread interval with
   CPU running/waiting time, ReadyThread sources, D3D11/DXGI events, Intel UMD
   `module+RVA` stacks, DxgKrnl queue/DMA packets, allocation/paging events,
   Present, and independent flip. Determine whether the delay occurs in the
   libplacebo timer Begin/End/GetData path, dynamic vertex/index-buffer
   `Map`/`Unmap`, resource/state binding, `Draw`, unbinding, completion-query
   retirement, or a driver allocation/recycle operation. Do not treat rolling
   `pass->last` as the current frame's GPU duration without ETW confirmation.
9. The strongest remaining diagnostic A/B is frame-latency pacing. Current
   `d3d11_video_output.cpp` waits on the frame-latency object only after a prior
   `Present(..., DXGI_PRESENT_DO_NOT_WAIT)` returned busy. Microsoft documents
   waiting before rendering every frame, including the first frame. Instrument
   the wait duration and compare a one-variable per-frame-wait build against the
   current busy-latched behavior; do not present the A/B as a fix until the
   settled trace identifies matching backpressure. See
   [`IDXGISwapChain2::GetFrameLatencyWaitableObject`](https://learn.microsoft.com/en-us/windows/win32/api/dxgi1_3/nf-dxgi1_3-idxgiswapchain2-getframelatencywaitableobject)
   and [`DXGI_PRESENT_DO_NOT_WAIT`](https://learn.microsoft.com/en-us/windows/win32/direct3ddxgi/dxgi-present).
10. If ETW cannot isolate the D3D11 call, create a temporary reproducible
    libplacebo 7.351.0 diagnostic patch that separately times timer queries,
    stream-buffer Map/Unmap, bindings, Draw, and cleanup. Because this modifies
    `ffmpeg/**`, run `ffmpeg/scripts/build-windows.ps1` and
    `cmake/verify-install.cmake` as required by `AGENTS.md`. Do not diagnose by
    editing only an ignored vcpkg buildtree.
11. Continue later one-variable A/B testing only at the same failing scene.
    RGB10/PQ versus `SdrOnly` BGRA8 already fails in both modes. Remaining useful
    isolates are FP16 scRGB, reduced versus display-sized output, D3D11VA versus
    `configureHardwareDecoding=false`, two versus three swap-chain buffers,
    temporary protected-frame `pl_gpu_finish()`, and active context reservation.
    A diagnostic `pl_gpu_finish()` result must not become the production fix.
12. Implement only an evidence-backed correction. Preserve native D3D11
    multithread protection, bounded context handoff and reason-aware retry,
    asynchronous successful submission, bounded completion-query retention,
    raw retained NV12/P010 decoder-slice sampling, and zero decoded-source
    map/transfer/copy.
13. Re-run the validation matrix below. The Intel task is complete only when
    the root cause, correction, exact-scene cadence/gaps, and cross-vendor
    regression evidence are recorded in `modern/PLAN.md`.

## Validation required after a fix

At minimum:

1. Fresh shared Release build and all 36 CTest tests.
2. Repeat the D3D11 device-access, D3D11VA lifecycle, D3D11 zero-copy, and
   composition-output tests multiple times if synchronization changed.
3. Rebuild the Release WinUI player with zero errors.
4. On the administrator-capable Intel system, repeat:
   - H.264/NV12 control, including seek;
   - `wednesday.mp4`, including seek and media replacement;
   - `legend.mkv` at 22:48;
   - `suzume.mkv` at 1:00:00 and 1:40:00;
   - Debug open/closed, pause/resume, and close while playing.
5. Require source cadence plus no recurring warm >80-ms render gaps or
   over-budget draw/pass-submission pattern. Zero terminal drops alone is
   insufficient. Save the post-fix trace and cadence log beside the failing
   capture.
6. Re-run the relevant AMD and NVIDIA cadence/lifecycle baseline before calling
   a generic Windows correction complete.
7. If the original Lenovo 21HW remains available, repeat the exact
   `suzume.mkv` scenes there after the transferred fix; otherwise record that
   the original-machine confirmation remains pending rather than silently
   treating the second Intel adapter as identical.
8. Run `git diff --check`, scan new `modern/` code for Qt dependencies, preserve
   UTF-8 without BOM and LF line endings, and update `modern/PLAN.md`.

## Guardrails

- Do not modify the legacy QtAV tree for this issue.
- Do not use system or independently downloaded FFmpeg packages; use the
  repository `ffmpeg/` package.
- Do not disable D3D11 multithread protection or make Player/render locks
  blocking to hide a timing issue.
- Do not add a per-frame `pl_gpu_finish()`, decoder-surface copy, CPU map,
  staging transfer, or software fallback and call the reduced throughput a
  fix.
- Do not change the accepted AD-007 imported-frame policy without new
  correctness evidence.
- Do not close the Intel task from subjective smoothness or zero terminal
  counters. Close it only after the clean exact-scene cadence/gap baseline and
  required cross-vendor regression pass.
- Do not attribute the regression to passive slider hover; real-pointer A/B and
  the WinUI event path already exclude it.
- Do not reset, overwrite, or absorb the parallel OHOS changes while preparing
  the Intel trace/fix. Coordinate overlapping `modern/PLAN.md` edits carefully.
