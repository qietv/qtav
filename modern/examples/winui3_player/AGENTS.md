# WinUI 3 player agent guide

This file applies to `modern/examples/winui3_player/` and supplements the
repository-level `AGENTS.md`. Follow both; the repository-level rules win if
they conflict.

## Purpose and scope

This project is a small, readable consumer of the public QtAVCore Windows API.
It should demonstrate local-file and URL playback in an unpackaged C++/WinRT
WinUI 3 application without becoming a second playback framework.

Keep changes inside this example unless the task explicitly requires a library
change. If a defect belongs to QtAVCore, fix it in the appropriate `modern/`
core or backend target and keep the example integration thin.

## Required reading

Before changing this example, read:

1. the repository `AGENTS.md`;
2. `modern/README.md`, `modern/MIGRATION.md`, and `modern/PLAN.md`;
3. this directory's `README.md`;
4. `ARCHITECTURE.md` and `ARCHITECTURE_DECISIONS.md` when changing behavior,
   ownership, threading, rendering, audio, or shutdown.

## Architecture constraints

- Do not add Qt or expose FFmpeg types through the UI layer.
- Continue using `qtav::Player` for playback control,
  `qtav::D3D11VideoOutput` for high-level video output,
  `qtav::WasapiAudioSink` for audio, and
  `qtav::SwresampleAudioConverter` for negotiated PCM conversion.
- Keep `MainWindow` responsible for XAML, user actions, surface binding, and
  diagnostics only. Demux, decode, synchronization, audio submission, render
  scheduling, and `Present()` belong to QtAVCore workers/backends.
- Do not move `renderVideo()`, `Present()`, blocking media I/O, or audio writes
  onto the WinUI dispatcher.
- Treat every Player or D3D11 output callback as a non-UI callback. Copy small,
  owned values and use `UiBridge::post()` before accessing XAML objects.
- Do not capture `MainWindowPrivate*`, XAML controls, frames, COM surface
  pointers, or borrowed backend objects directly in long-lived worker
  callbacks.
- Keep per-frame callbacks bounded. Atomic counters and one-time metadata
  capture are acceptable; formatting, XAML updates, file I/O, waits, and
  unbounded allocation are not.
- Preserve the one-seek-per-drag behavior and stale-completion serial check.
- Keep UI progress observational: `Player::position()` is a cached clock read;
  the UI timer must not become the playback clock or drive media delivery.

## Ownership and shutdown

`D3D11VideoOutput::attach()` borrows the Player and owns its render slot until
`detach()`. Preserve this shutdown order:

1. mark shutdown and clear `UiBridge::target`;
2. stop UI timers and close the Debug window;
3. request `qtav::State::Stopped`;
4. detach, close, and destroy `D3D11VideoOutput`;
5. destroy `qtav::Player` and allow it to interrupt I/O and join its workers.

Never detach threads, leak the player/output to shorten exit, or destroy either
object from one of its callbacks. If shutdown latency must be hidden, design an
explicit two-phase application teardown and update the architecture decision;
do not weaken deterministic ownership.

## Build and generated files

- This example is Windows x64 only. Confirm the host is Windows before making
  or validating Windows SDK, COM, D3D11, DXGI, WASAPI, or WinUI changes.
- Use `build.ps1` for the normal build because it builds dependencies, restores
  NuGet packages, and copies runtime DLLs.
- The QtAVCore dependency must be a shared build and the app configuration must
  match it (`Debug` with Debug, `Release` with Release).
- Do not hand-edit or commit `.vs/`, `bin/`, `obj/`, `x64/`, `Debug/`,
  `Release/`, `Generated Files/`, `AppPackages/`, or `packages/`.
- Keep source and Markdown UTF-8 without BOM and LF-only.

## Validation before handoff

Run checks proportional to the change. For any behavior or project change:

```powershell
Set-Location modern/examples/winui3_player
./build.ps1 -Configuration Release
```

For playback, rendering, audio, seek, or shutdown changes, also run the
relevant QtAVCore CTest suite and the manual scenarios in `TESTING.md`. Test at
least one local file and one reachable HTTP(S) file-style URL. Exercise Debug
open/close and close the main window while media is playing.

Before finishing:

- run `git diff --check`;
- verify changed text files have no UTF-8 BOM or CRLF;
- search new `modern/` code for accidental Qt dependencies;
- update `modern/PLAN.md` for a completed meaningful subtask;
- update `README.md`, `ARCHITECTURE.md`,
  `ARCHITECTURE_DECISIONS.md`, and `TESTING.md` when their contract changes.

Do not claim native playback, HDR, URL recovery, or shutdown validation unless
it was actually exercised on the relevant Windows system.
