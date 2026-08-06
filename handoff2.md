# QtAVCore original Intel Iris Xe D3D11 follow-up handoff

## Mission

Continue the Windows D3D11 post-seek investigation on the **original** Intel
Iris Xe machine. First validate the correction already present on this transfer
branch. Modify production code again only if that machine still reproduces
the distinct 40-49-ms libplacebo pass-submission stall and new evidence
identifies a remaining cause.

There are two separate findings. Keep them separate in all conclusions:

1. On a second Intel UHD 770 host, the reproducible cadence loss was caused by
   treating a bounded frame-latency waitable-object timeout as definitive swap
   chain backpressure. The output skipped the render pass without calling
   `Present`, then requeued it, amplifying delayed signals into repeated
   116-203-ms render gaps. This is fixed in the current worktree.
2. The original Iris Xe host previously showed a different, more specific
   symptom: roughly 40-49 ms of CPU wall time inside `pl_render_image()` before
   libplacebo's successful-pass callback, while asynchronous GPU pass time was
   only about 1.7-1.9 ms. The second host did not reproduce that exact internal
   delay. It is therefore still open until the corrected build is tested on the
   original adapter.

Do not claim that the Iris Xe issue is fixed merely because the UHD 770 result
passes. Conversely, do not discard the existing output correction if the Iris
Xe still has an additional driver/libplacebo stall.

## Root-cause priority

The primary instruction for the next Codex is to **find and prove the root
cause before proposing another repair**. Testing the current correction on the
original adapter is the first experiment, not permission to stop at subjective
smoothness or to add another workaround.

If the recurring 40-49-ms `pl_render_image()` interval remains, first map it to
an exact instrumented D3D11/libplacebo stage or an ETW stack. Preserve matching
application, probe, and ETL timestamps. Do not change synchronization, buffer
count, decode mode, output quality, or vendor behavior speculatively. A further
production change is justified only after the first blocking call or state
transition is identified and the proposed change directly addresses it.

If repeated exact-scene runs show that the current correction also removes the
old internal interval on the original Iris Xe, document that as original-device
root-cause confirmation: the previously observed long `pl_render_image()` wall
time was a downstream measurement associated with the capacity-timeout/requeue
cycle. Do not claim that relationship unless the new separated counters and
probe logs support it.

## Read first

Read these files before changing anything:

1. `AGENTS.md`
2. `handoff.md` for the complete pre-correction chronology
3. `modern/README.md`
4. `modern/MIGRATION.md`
5. `modern/PLAN.md`, especially `Next task` and the transferred Intel
   follow-up dated 2026-08-05
6. `modern/examples/winui3_player/AGENTS.md`
7. `modern/examples/winui3_player/TESTING.md`

`handoff.md` is historical and describes the state before the second-host
correction. This file is the current continuation entry point.

## Repository and worktree state

- Repository: `C:\vscode\qtav`
- Transfer branch: `codex/intel-d3d11-root-cause-handoff`
- Base commit before the transfer commit:
  `8ce66844c7213dda54178b3d792b9161f426f11f`
- Active implementation: `modern/`; do not modify the legacy root QtAV tree.
- The correction, diagnostics, supporting tests/documentation, and this
  handoff are one focused transfer commit on the branch. `modern/PLAN.md` is
  intentionally unchanged and is not part of the transfer commit.

Expected paths in the transfer commit:

```text
ffmpeg/ports/libplacebo/portfile.cmake
ffmpeg/ports/libplacebo/0006-add-d3d11-pass-diagnostics.patch
modern/DECISIONS.md
modern/MIGRATION.md
modern/README.md
modern/backends/output/d3d11/include/qtav/d3d11_video_output.h
modern/backends/output/d3d11/src/d3d11_video_output.cpp
modern/examples/winui3_player/ARCHITECTURE.md
modern/examples/winui3_player/MainWindow.xaml.cpp
modern/examples/winui3_player/TESTING.md
modern/tests/d3d11_video_output_test.cpp
```

Run `git status --short` and `git log -1 --stat` before editing. The transferred
tree should be clean; treat any additional changes as user work until proven
otherwise.

## Correction already implemented

`D3D11VideoOutputStatistics` now exposes:

- `presentationCapacityWaits`
- `presentationCapacityTimeouts`
- `maximumPresentationCapacityWaitMicroseconds`
- `busyPresents`, now restricted to actual
  `DXGI_ERROR_WAS_STILL_DRAWING` results from nonblocking `Present()`

The render loop still performs the bounded 20-ms wait on the swap chain's
frame-latency waitable object. On `WAIT_TIMEOUT`, it records the timeout but no
longer abandons the pass. It renders and lets
`Present(1, DXGI_PRESENT_DO_NOT_WAIT)` make the authoritative backpressure
decision. `WAIT_FAILED` remains an error.

This behavior is generic for every Windows GPU using this D3D11 output path; it
has no Intel vendor check. Intel is mentioned because it exposed the bug. If
AMD or NVIDIA never times out, their normal path is unchanged. If a timeout
occurs and the swap chain truly remains busy, nonblocking `Present()` still
returns `DXGI_ERROR_WAS_STILL_DRAWING` and the existing retry path applies.

Relevant source locations:

- `modern/backends/output/d3d11/src/d3d11_video_output.cpp`: capacity wait near
  the start of the render pass and actual Present-busy handling near the end
- `modern/backends/output/d3d11/include/qtav/d3d11_video_output.h`: public
  statistics and semantics
- `modern/examples/winui3_player/MainWindow.xaml.cpp`: five-second cadence line
  containing `capacity-wait(timeout)` and `max-capacity-wait-ms`
- `modern/tests/d3d11_video_output_test.cpp`: WARP separation assertions

Do not reintroduce the old `requestRender(); continue;` behavior on
`WAIT_TIMEOUT` without evidence that the current Present-authoritative path is
incorrect.

## libplacebo D3D11 diagnostic patch

The repository libplacebo port now applies
`ffmpeg/ports/libplacebo/0006-add-d3d11-pass-diagnostics.patch`.

Set this before creating the player/GPU to append calls taking at least 0.5 ms:

```powershell
$env:QTAV_LIBPLACEBO_D3D11_DIAGNOSTICS = `
  'C:\QtAVTraces\libplacebo-d3d11-suzume-100-<timestamp>.log'
```

Use a unique file for every run. Covered stage names are:

```text
timer-start
timer-end
timer-query-disjoint
timer-query-end
timer-query-start
stream-map-discard
stream-map-no-overwrite
stream-unmap
state-bind
draw
state-unbind
message-queue
raster-pass-total
pass-run-total
```

Each record includes CPU timestamp, process, thread, stage, and duration in
microseconds. No file is created if no covered call crosses 0.5 ms.

For one diagnostic A/B only, this disables libplacebo's D3D11 GPU timer-query
path for a newly created GPU:

```powershell
$env:QTAV_LIBPLACEBO_D3D11_DISABLE_TIMERS = '1'
```

This is **not** a supported playback mode or production workaround. The second
host already showed that disabling timer queries did not remove its cadence
loss. Leave it unset for ordinary validation:

```powershell
Remove-Item Env:QTAV_LIBPLACEBO_D3D11_DISABLE_TIMERS `
  -ErrorAction SilentlyContinue
```

Because this patch changes `ffmpeg/**`, every further modification to it must
be validated with the repository Windows FFmpeg build script and
`cmake/verify-install.cmake`; editing only an ignored vcpkg buildtree is not
acceptable.

## Evidence from the second Intel host

Test host:

- Dell Pro Tower QCT1250
- Intel Core i5-14500, 14 cores / 20 threads, 32 GB RAM
- Intel UHD Graphics 770, `PCI\VEN_8086&DEV_4680&SUBSYS_0D181028`
- Intel driver `32.0.101.7085`
- inactive NVIDIA RTX 3050; display attached to Intel
- Windows 11 Enterprise 25H2 build `26200.8246`
- balanced power plan
- 3840x2160/60-Hz HDR display
- 1708x814 composition surface, RGB10/PQ, 240-nit SDR white, 1405-nit peak

The Store-app process had medium integrity. `wpr -start GPU -filemode` failed
with `0xc5585011`, so the investigation used the application counters and
libplacebo probe.

Before the correction, `legend.mkv` at 22:48 showed:

- actual nonblocking Present busy: zero
- frame-latency 20-ms wait timeouts: 3-8 per five-second interval
- measured timeout returns commonly around 31-35 ms
- 116-203-ms render gaps
- rendered cadence around 24.0-24.9 fps for a 25-fps source

Single-variable A/B results:

- three swap-chain buffers did not improve the repeated timeouts or gaps
- increasing the wait budget to 40 ms moved returns to about 50-53 ms but did
  not restore cadence
- disabling libplacebo timer queries did not remove the reproduced gaps
- no probed timer, Map/Unmap, bind, Draw, unbind, or total pass stage exceeded
  0.5 ms on this UHD 770 during the relevant runs

After the correction:

- `legend.mkv` at exactly 22:48 settled at 24.9-25.1 fps; timeout intervals no
  longer compounded, actual Present busy stayed zero, and warm draw was about
  14-17 ms
- `suzume.mkv` at exactly 1:00:00 and 1:40:00 sustained 23.8-24.2 scheduled
  and rendered fps for more than 50 seconds at each scene, with no warm >80-ms
  gaps, coalescing, capacity timeouts, Present busy, terminal drops, or decoder
  copies; warm draw was about 14-18 ms
- five-second pause/resume recovered to source cadence
- H.264/NV12 control at 00:59 sustained 29.9-30.1 fps
- same-process replacement with `wednesday.mp4` at 29:39 sustained
  23.8-24.1 fps
- close while playing completed in 385 ms and 332 ms without Application Error
  or Windows Error Reporting events

Second-host logs, if this worktree is being used on that same filesystem:

```text
C:\QtAVTraces\legend-capacity-timeout-fallback-debug.txt
C:\QtAVTraces\suzume-capacity-timeout-fallback-debug.txt
C:\QtAVTraces\h264-capacity-timeout-fallback-debug.txt
C:\QtAVTraces\wednesday-capacity-timeout-fallback-debug.txt
```

Completed build validation on that host:

- shared Release CTest: 36/36
- static Release CTest: 36/36
- Release WinUI player: zero warnings and errors
- `ffmpeg/scripts/build-windows.ps1`: passed
- `cmake/verify-install.cmake`: passed
- `git diff --check`, UTF-8/LF validation, and Qt-dependency scan: passed

During transfer-commit preparation on 2026-08-06, the 35 unaffected shared and
35 unaffected static CTests passed again, and package verification passed. The
composition-output test reached the new capacity-statistics assertions but its
existing later pause/resume assertion raced the one-second generated fixture:
at timeout the Player was already `Stopped`/`EndOfMedia`, with no output error,
busy result, or terminal drop. The same test had passed in both 36/36 validation
runs during the media investigation. No unrelated Player/test-fixture change is
included in this focused transfer commit; rerun the complete suite on the
original machine and report this separately if it recurs.

## Original Iris Xe target and old failure signature

Original target:

- Lenovo 21HW
- Intel Core i5-13500H, 12 cores / 16 logical processors
- 31.9 GB RAM
- Intel Iris Xe Graphics,
  `PCI\VEN_8086&DEV_A7A0&SUBSYS_3C4817AA`
- Intel driver `32.0.101.7088`
- Windows 25H2 build `26200.8894`
- balanced power plan, AC online
- primary Philips `PHL0979` display at 3840x2160/60 Hz, Windows HDR active
- expected WinUI surface near 1708x814 and RGB10/PQ output

The user states that the test media are now under `C:\test`. Treat these paths
as authoritative rather than the older Downloads paths in `handoff.md`:

```text
C:\test\suzume.mkv
C:\test\legend.mkv
C:\test\wednesday.mp4
C:\test\qtav-h264-nv12-control-1080p.mp4
```

Old exact-scene signature before the current correction:

- `suzume.mkv`, 3840x1608, HEVC Main 10, BT.2020/PQ, 24 fps
  - cold pre-seek playback: stable 23.8-24.2 fps, warm draw about 12-15 ms
  - after seek to 1:00:00 or 1:40:00: recurring 40-49-ms
    `pl_render_image()` CPU time, 82-102-ms render gaps, and old
    `present-busy`/coalescing reports
- `legend.mkv`, 3840x2160, HEVC Main 10, BT.2020/PQ, 25 fps
  - seek to 22:48: recurring roughly 49-52-ms draw/pass time and 84-131-ms
    render gaps, with occasional larger events
- during slow settled frames, libplacebo still reported one pass, zero pass
  graph changes, and only about 1.7-1.9 ms of asynchronous rolling GPU time
- the successful-pass callback arrived after the full 40-49-ms interval and
  return after the callback was effectively immediate, placing the old delay
  before that callback inside libplacebo's D3D11 pass path
- `SdrOnly` BGRA8 also reproduced the stall, so HDR10 output was not necessary
- D3D11VA remained active, decoder copies stayed zero, source scheduling stayed
  near cadence, and GPU utilization remained low

Important reinterpretation: the old `present-busy` counter combined capacity
wait timeouts with true Present busy. Those historical logs cannot distinguish
the two. Only a run with the new fields can establish what happened on the
original machine.

## Build on the original machine

First record `git status --short` and verify that the files listed above are
present. Then rebuild the repository FFmpeg package so the libplacebo probe is
actually included:

```powershell
Set-Location C:\vscode\qtav
& .\ffmpeg\scripts\build-windows.ps1

cmake `
  -DINSTALL_ROOT=C:\vscode\qtav\ffmpeg\build\x64-windows-static-md\vcpkg_installed `
  -DTRIPLET=x64-windows-static-md `
  -P C:\vscode\qtav\ffmpeg\cmake\verify-install.cmake
```

Configure and build the corrected shared Release tree using only the repository
dependency prefix:

```powershell
cmake -S modern -B build/modern-shared-intel `
  -G "Visual Studio 18 2026" -A x64 -T ClangCL `
  -DCMAKE_TOOLCHAIN_FILE="C:/vscode/qtav/ffmpeg/vcpkg/scripts/buildsystems/vcpkg.cmake" `
  -DVCPKG_INSTALLED_DIR="C:/vscode/qtav/ffmpeg/build/x64-windows-static-md/vcpkg_installed" `
  -DVCPKG_TARGET_TRIPLET=x64-windows-static-md `
  -DBUILD_SHARED_LIBS=ON `
  -DQTAV_CORE_BUILD_TESTS=ON `
  -DQTAV_CORE_BUILD_EXAMPLES=ON

cmake --build build/modern-shared-intel --config Release --parallel
ctest --test-dir build/modern-shared-intel `
  -C Release --output-on-failure

Set-Location modern/examples/winui3_player
& .\build.ps1 `
  -Configuration Release `
  -QtAVBuildDir C:\vscode\qtav\build\modern-shared-intel
```

Expected executable:

```text
C:\vscode\qtav\modern\examples\winui3_player\bin\x64\Release\QtAVWinUI3.exe
```

Do not allow CMake to fall back to a system or unrelated `C:\vcpkg` package.
Supported builds must use the repository `ffmpeg/` prefix.

## Original-machine test protocol

### 1. Preflight

1. Confirm no unrelated `cl`, `clang-cl`, `link`, `msbuild`, `cmake`, `ninja`,
   Gradle, or media-processing job is active.
2. Record adapter PCI ID, driver version, Windows build, CPU, memory, active
   power plan, AC state, display resolution/refresh/HDR state, WinUI surface
   size, selected output color mode, SDR white, and display peak.
3. Confirm the Release player reports D3D11VA, one libplacebo pass after
   settling, and `decoder-copies=0`.
4. Create `C:\QtAVTraces` if needed. Use unique names; do not overwrite an old
   failing or passing capture.
5. Leave `QTAV_LIBPLACEBO_D3D11_DISABLE_TIMERS` unset. Set a unique
   `QTAV_LIBPLACEBO_D3D11_DIAGNOSTICS` path before launching the player.

### 2. Non-elevated corrected-build validation first

Run each scene cold where practical and retain the WinUI Debug cadence text:

1. H.264/NV12 control: settle, seek to 00:59, observe at least 30 seconds.
2. `suzume.mkv`: cold beginning for 60-120 seconds; pause five seconds and
   resume; seek once to exactly 1:00:00 and observe 60 seconds; then seek to
   exactly 1:40:00 and observe another 60 seconds.
3. `legend.mkv`: seek once to exactly 22:48 and observe at least 60 seconds.
4. Replace media in the same process with `wednesday.mp4`, seek to 29:39, and
   observe at least 30 seconds.
5. Repeat a primary failing scene with Debug hidden, then reopen Debug and
   inspect the retained lines.
6. Close the main window while playing; measure exit latency and inspect
   Application Error / Windows Error Reporting events.

For every settled five-second line, preserve at least:

- source, scheduled, and rendered fps
- `capacity-wait(timeout)` and `max-capacity-wait-ms`
- actual `present-busy`
- coalesced, retry/superseded/terminal, and decoder-copy counts
- `>80ms gaps(video/render)`
- `max-stage-ms(color/interop/buffer/draw)`
- `max-render-detail-ms(retire/query/clear/pl-render/end/retain)`
- libplacebo pass count, graph changes, asynchronous GPU time, callback
  arrival, and time after callback

Run the primary `suzume` and `legend` scenes at least twice before declaring a
sporadic problem absent.

### 3. Success criteria for the existing correction

The corrected output passes the original-device gate only if:

- scheduled and rendered cadence settle at approximately source rate
- there are no recurring warm >80-ms render gaps
- the old recurring 40-49-ms `pl_render_image()` CPU pattern disappears
- capacity-wait timeouts, if any, do not amplify into repeated lost cadence
- actual Present busy is interpreted separately from capacity timeouts
- terminal drops and decoder copies remain zero
- pause/resume, media replacement, and close while playing remain deterministic

If this passes twice at every primary scene, do not invent another production
change. Record the original Iris Xe confirmation in `modern/PLAN.md`, retain the
diagnostic patch for review, and proceed to same-commit AMD/NVIDIA regression.

## When administrator WPR is needed

Administrator tracing is not required if the corrected build eliminates both
the cadence loss and the 40-49-ms internal pass pattern on repeated runs.

Use an elevated Windows Terminal or administrator-launched Codex only if the
original symptom remains or the application probe leaves the exact call
unresolved. The Store build previously failed to start WPR with `0xc5585011`.

Before recording, verify that no unrelated WPR session belongs to another
user:

```powershell
wpr -status
New-Item -ItemType Directory -Force C:\QtAVTraces
wpr -start GPU -filemode
```

While recording:

1. Keep 5-10 seconds of settled playback.
2. Perform exactly one ordinary pointer seek to `suzume.mkv` 1:00:00 and
   release the pointer.
3. Continue for 30-60 seconds until the application log captures slow events.
4. Stop to a unique ETL:

```powershell
wpr -stop C:\QtAVTraces\qtav-irisxe-suzume-100-<timestamp>.etl
```

Repeat 1:40:00 or `legend.mkv` 22:48 only after the first ETL is known to be
valid. Keep the matching WinUI cadence log and libplacebo diagnostic log beside
each ETL.

In WPA/GPUView, correlate the render thread's slow interval with D3D11 runtime
and Intel user-mode-driver stacks, GPU queue packets, context submissions,
scheduling, composition, and Present. Confirm whether GPU execution remains
near 1-2 ms while CPU submission is delayed.

Do not repeatedly retry WPR from a non-elevated process. If elevation is not
available, use the existing counters/probe first and add narrowly scoped
counters only around an uncovered region.

## Evidence-driven decision tree if the Iris Xe still fails

1. **`scheduled-video` is below source cadence**: investigate input, decode,
   clock, queue, or Player scheduling. This is not initially a D3D11 output
   problem.
2. **Repeated capacity timeouts, actual Present busy zero, but cadence remains
   low**: verify that the current worktree really continues after
   `WAIT_TIMEOUT`; inspect render time rather than restoring the old requeue.
3. **Actual Present busy is nonzero**: correlate with composition/Present and
   swap-chain capacity. Treat it separately from the waitable-object signal.
4. **`pl-render` is 40-49 ms and one probe stage is similarly slow**: the named
   stage is the first proven blocking call. Correlate it with ETW stacks before
   selecting a workaround or upstream libplacebo/driver change.
5. **`pass-run-total` or `raster-pass-total` is slow but no child stage is**:
   add instrumentation only around the uncovered region between existing
   probes. Preserve the 0.5-ms threshold and environment gate.
6. **`pl-render` is slow but the diagnostic file is absent or has no matching
   slow pass**: verify that the rebuilt repository libplacebo package is loaded.
   If it is, use WPR or extend the probe; do not guess which D3D11 call blocked.
7. **GPU time also rises to the CPU stall duration**: this is no longer the old
   CPU-submission-only signature; investigate shader workload, queue saturation,
   or synchronization as a new problem.
8. **Only the seek-transition window is slow and settled windows are clean**:
   distinguish expected transition work from the old persistent recurrence.

The timer-query-disable, three-buffer, 40-ms wait, and SDR-only A/Bs are already
negative for the questions they tested. Do not repeat them unless the original
machine produces evidence that materially differs.

## Constraints on any additional repair

Any new repair must preserve:

- D3D11 native multithread protection
- bounded context handoff and reason-aware retry
- asynchronous successful submissions
- bounded completion-query retention
- raw retained NV12/P010 decoder-slice sampling
- zero decoded-source CPU map, transfer, staging, or copy
- libplacebo as color/HDR authority
- nonblocking UI-thread behavior

Do not hide the issue with:

- per-frame `pl_gpu_finish()` or another unconditional GPU wait
- blocking Player/render locks
- decoder-surface copies, CPU maps, staging uploads, or forced software decode
- permanently disabling libplacebo timers
- silently reducing output resolution, bit depth, or HDR mode
- changing the accepted AD-007 imported-frame policy without new correctness
  evidence
- vendor-specific behavior before the exact vendor-specific failure is proven

## Validation after any further code change

At minimum:

1. Run the directly affected build and tests.
2. Run shared Release CTest, expected 36/36.
3. Run static Release CTest, expected 36/36, if public structures or shared
   D3D11 code change.
4. Rebuild the Release WinUI player with zero errors.
5. If `ffmpeg/**` changes, run `ffmpeg/scripts/build-windows.ps1` and
   `cmake/verify-install.cmake`.
6. Repeat all four media workloads and exact seeks listed above.
7. Repeat Debug open/closed, pause/resume, same-process replacement, and close
   while playing.
8. Save pre-fix and post-fix logs/traces under unique names.
9. Complete same-commit AMD and NVIDIA cadence/lifecycle regression before
   calling a generic Windows correction complete.
10. Run `git diff --check`; verify UTF-8 without BOM and LF line endings; scan
    new `modern/` code for accidental Qt dependencies.
11. Update `modern/PLAN.md`, public documentation, and migration/threading notes
    when applicable.

## Required final report from the original machine

Record all of the following for the next handoff or commit description:

- exact commit and dirty diff used
- adapter PCI ID, driver, OS build, display/HDR/output state
- build and CTest results
- exact scene and observation duration for every workload
- representative settled and failing cadence lines
- separate capacity-wait timeout and actual Present-busy counts
- libplacebo diagnostic stages and durations, including the absence of entries
  when relevant
- WPR/ETL path and stack conclusion if tracing was needed
- root cause stated no more broadly than the evidence supports
- exact repair and why rejected A/Bs did not solve the problem
- original Iris Xe post-fix evidence
- AMD/NVIDIA regression status
- remaining limitations or hardware confirmation still pending

The task is complete only when the original Iris Xe result is recorded and any
generic Windows correction has the required cross-vendor evidence. If the
original machine cannot be tested, leave the gate explicitly open.
