# QtAVCore continuous integration

[`../.github/workflows/qtavcore-supported-targets.yml`](../.github/workflows/qtavcore-supported-targets.yml)
currently owns the repeatable Windows build, test, install, and installed-
package-consumer gate. Android and OHOS Actions jobs are temporarily disabled
by project direction; their local drivers and product support remain intact.
The workflow does not own physical-device qualification.

## Runner and toolchain contract

The active Windows job uses a 64-bit Windows self-hosted runner with the labels
`self-hosted`, `Windows`, and `X64`. This keeps the workflow on the same
repository-supported host path as the checked-in PowerShell build scripts.

Pull requests whose head repository is a fork do not execute these persistent
self-hosted jobs. A maintainer must inspect that contribution and move or
reproduce it on a trusted same-repository branch before running the matrix;
the workflow does not use `pull_request_target` to execute untrusted code.

The active runner provides:

- Visual Studio 18 2026 with the ClangCL and lld-link components, plus CMake;
- network access for vcpkg source acquisition and enough local storage for the
  Windows dependency and package build trees.

Each driver records the selected CMake/build-tool versions, target ABI/API,
dependency prefix, and pinned vcpkg submodule revision in the job log. The
Windows workflow requires vcpkg revision
`9e593bb18ea69cc5095e012465dcd675a822ed0d` (`2026.07.29`) and fails before a
build if the checked-out submodule differs. Workflow QtAVCore builds use an
explicit parallelism of eight to keep simultaneous static links bounded;
local callers may select a different value with `-Parallel`.

## Job ownership

The Windows job:

1. runs `ffmpeg/scripts/build-windows.ps1`, including
   `ffmpeg/cmake/verify-install.cmake`;
2. configures and builds both shared and static Release packages with ClangCL;
3. runs both full CTest suites, including the deterministic package-version
   requests, staged installation, and standalone installed-package consumer;
4. installs both package forms into their CI build trees.

A test that returns the repository's CTest skip code 77 remains a skip. The
Advanced Color display test returns that code before creating D3D resources
when the runner process has no interactive window station. A runner without an
audio endpoint, interactive display, active HDR display, or qualifying native
device is never converted into a passing device result.

## Temporarily suspended Actions jobs

The Android and OHOS Actions jobs are intentionally absent from the current
workflow. This is an operational suspension, not removal of those supported
targets and not evidence that their CI gates passed.

The retained Android driver:

1. runs `ffmpeg/scripts/build-android.ps1` for the repository
   `arm64-android-28-static` dependency prefix and verifies it;
2. cross-compiles the shared and static arm64/API 28 Release trees, including
   the configured native test targets, but does not execute ARM64 binaries;
3. installs each package and links
   `examples/android/install-consumer` against that installed package.

The retained OHOS driver follows the same build/install/consumer boundary for
`arm64-ohos-23-static`. It uses `ffmpeg/scripts/build-ohos.ps1` and
`modern/scripts/build-ohos.ps1`, compiles the configured tests and examples,
then links `examples/ohos/install-consumer`. Windows does not execute the
resulting AArch64 binaries and the job does not create, sign, or install a HAP.

Run these drivers manually only on a configured host. Restoring either Actions
job requires an accessible pinned SDK/toolchain and a successful same-revision
GitHub run before the corresponding CI item may be marked complete.

## Dependency cache boundary

The active Windows job has a dedicated `VCPKG_DEFAULT_BINARY_CACHE`. Its key
includes the pinned vcpkg revision and a hash of the FFmpeg manifest, overlay
ports, and triplets. There are no broad restore keys. The cache contains only
vcpkg binary archives; it does not contain or restore `vcpkg_installed`
prefixes.

Consequently, the Windows job still invokes the repository dependency build
script and finishes through `verify-install.cmake`, even on a cache hit. It
does not download another workflow's artifact as a dependency fallback.
Workflow artifacts contain only bounded test/configuration/build logs and are
retained for seven days. The suspended cross-target drivers preserve the same
local dependency verification boundary.

## Local reproduction on Windows

Initialize the pinned dependency submodule once:

```powershell
git submodule update --init ffmpeg/vcpkg
```

Run the active workflow job from the repository root:

```powershell
./modern/scripts/ci/build-windows.ps1 -Parallel 8
```

The suspended Android/OHOS Actions jobs can still be reproduced manually on a
configured host:

```powershell
./modern/scripts/ci/build-android.ps1 -Parallel 8
./modern/scripts/ci/build-ohos.ps1 -Parallel 8
```

The Android driver requires SDK CMake 4.1.2 and NDK 29.0.14206865. Pass
`-SdkRoot` when the SDK is outside the normal Android Studio location. The OHOS
driver accepts `-SdkRoot` or uses the same environment/DevEco discovery as the
repository dependency script.

For an already built local dependency prefix, `-SkipDependencies` skips the
vcpkg install operation but still runs `verify-install.cmake` before QtAVCore is
configured:

```powershell
./modern/scripts/ci/build-windows.ps1 -SkipDependencies -Parallel 8
./modern/scripts/ci/build-android.ps1 -SkipDependencies -Parallel 8
./modern/scripts/ci/build-ohos.ps1 -SkipDependencies -Parallel 8
```

This switch is a local iteration convenience; the active GitHub Actions job
does not use it.

## Explicitly separate device gates

The following remain in the recorded connected-device/manual matrices and are
not claims made by this workflow:

- APK or HAP signing, installation, replacement authorization, and lifecycle
  playback on a physical device;
- Android MediaCodec/AImageReader, compositor HDR-layer, native HDR surface,
  AAudio, and vendor-driver validation;
- OHOS OHCodec/NativeBuffer, strict explicit-plane Vulkan, native HDR, OHAudio,
  and signed-HAP validation;
- Windows vendor GPU cadence, active-display HDR, physical audio audibility,
  and the external Intel post-seek investigation.

Those gates must report their real hardware, driver/system, media, lifecycle,
and objective counters. Their absence from CI is neither a pass nor a failure
of the compile/package contract.
