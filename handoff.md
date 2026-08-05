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

The originating machine now proceeds with OHOS development in parallel. The
transferred Windows task owns only the unfinished Intel investigation and must
merge its evidence and fix back without discarding the OHOS work. The complete
chronological record is in [`modern/PLAN.md`](modern/PLAN.md), especially
`Next task`, `Intel checkpoint on 2026-08-05`, `Intel clean-load follow-up on
2026-08-05`, and `Intel D3D11 pass-submission follow-up on 2026-08-05`.

## Repository state

- Repository: `C:\vscode\QtAV`
- Current commit: `61afd1e4508e4817b9de4c8419df12880d19eb37`
- Active implementation: `modern/` (QtAVCore), not the legacy root QtAV tree.
- Existing uncommitted Intel diagnostic work modifies the D3D11 renderer and
  output statistics, WinUI cadence logging, `modern/README.md`,
  `modern/examples/winui3_player/TESTING.md`, and `modern/PLAN.md`. Preserve it;
  do not reset or overwrite it.
- The shared worktree also contains separate OHOS/FFmpeg changes, including
  `ffmpeg/scripts/build-ohos.ps1`, `ffmpeg/scripts/ohos-windows-common.ps1`,
  `modern/scripts/`, `modern/CMakeLists.txt`, and related FFmpeg port/verification
  files. These are not part of the Intel diagnosis. Do not discard, fold into
  an Intel-only commit, or edit them unless the OHOS task explicitly requires
  it.
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

The latest instrumented Windows build is based on the same commit plus the
uncommitted diagnostic statistics:

- CMake tree: `C:\vscode\QtAV\build\modern-shared-intel`
- WinUI executable:
  `C:\vscode\QtAV\modern\examples\winui3_player\bin\x64\Release\QtAVWinUI3.exe`
- Visual Studio 2026, ClangCL 22.1.3, shared Release
- repository `x64-windows-static-md` FFmpeg/libplacebo package
- complete CTest result: 36/36 passed
- WinUI Release build: zero MSBuild warnings and errors
- the final WinUI build restored the normal HDR-preferred RGB10/PQ path after a
  temporary `SdrOnly` A/B

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

## Administrator-machine test and investigation order

The previous non-elevated host has WPR/WPA installed, but
`wpr -start GPU -filemode` failed with `0xc5585011` while enabling the system
performance profiling policy. No trace was produced. Run the next steps from an
administrator-launched Codex or elevated Windows Terminal.

1. Recreate or transfer the instrumented worktree. Confirm that the diagnostic
   fields described above are present, build shared Release, run all 36 CTest
   tests, and rebuild the Release WinUI player. Do not mix the separate OHOS
   changes into an Intel-only commit.
2. Record the new machine's adapter PCI IDs, Intel driver, Windows build, CPU,
   memory, power mode/AC state, display resolution/refresh/HDR state, WinUI
   surface size, and reported output color mode. Differences from the Lenovo
   21HW baseline are expected but must be explicit.
3. Verify that no build process is active. Cold-open `suzume.mkv`, open Debug,
   and capture at least two settled pre-seek cadence lines. Confirm D3D11VA,
   one libplacebo pass, zero decoder copies, and normal warm CPU/GPU timings.
4. In an elevated PowerShell, check that no unrelated WPR recording is active:

   ```powershell
   wpr -status
   New-Item -ItemType Directory -Force C:\QtAVTraces
   wpr -start GPU -filemode
   ```

   Do not cancel another user's recording. If a stale QtAV-owned session exists,
   stop or cancel only that known session before retrying.
5. While recording, retain 5-10 seconds of settled playback, make one ordinary
   pointer seek to 1:00:00, release the pointer, and continue for 30-60 seconds.
   Save/copy the Debug log, then stop the trace:

   ```powershell
   wpr -stop C:\QtAVTraces\qtav-intel-suzume-seek.etl
   ```

   Repeat at 1:40:00 only after the primary trace is valid. Keep separate ETL
   files instead of overwriting the first capture.
6. In WPA/GPUView, correlate the render thread's 40-86-ms CPU intervals with
   D3D11 runtime and Intel user-mode-driver stacks, GPU queue packets, context
   submissions, scheduling, and Present. Determine whether the delay is in the
   timer start/query path, dynamic vertex-buffer `Map`/`Unmap`, state/resource
   binding, `Draw`, unbinding, or a driver allocation/recycle operation. Verify
   that the corresponding GPU pass remains near 1-2 ms.
7. If ETW stacks do not identify the call, create a temporary reproducible
   libplacebo 7.351.0 diagnostic patch that times each D3D11 operation listed in
   step 6. Because this modifies `ffmpeg/**`, run
   `ffmpeg/scripts/build-windows.ps1` and `cmake/verify-install.cmake` as required
   by `AGENTS.md`. Do not diagnose by editing only an ignored vcpkg buildtree.
8. Continue one-variable A/B testing at the same scene. RGB10/PQ versus
   `SdrOnly` BGRA8 is already complete and both fail. Remaining useful isolates
   are FP16 scRGB, reduced versus display-sized output, D3D11VA versus
   `configureHardwareDecoding=false`, two versus three swap-chain buffers, and
   reopen/seek behavior. Preserve libplacebo as the color authority.
9. Implement only an evidence-backed fix. Preserve:
   - native D3D11 multithread protection;
   - bounded context handoff and reason-aware retry;
   - asynchronous successful submissions;
   - bounded completion-query retention;
   - raw retained NV12/P010 decoder slice sampling;
   - zero decoded-source map/transfer/copy.
10. Re-run the validation matrix below. The Intel task is complete only when
    the root cause, correction, exact-scene cadence/gaps, and cross-vendor
    regression evidence are all recorded in `modern/PLAN.md`.

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
