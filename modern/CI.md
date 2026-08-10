# QtAVCore continuous integration

[`../.github/workflows/qtavcore-supported-targets.yml`](../.github/workflows/qtavcore-supported-targets.yml)
owns the repeatable build, test, install, and installed-package-consumer gate for
the supported Windows, Android, and OHOS targets. It does not own physical-device
qualification.

## Runner and toolchain contract

All three jobs use a 64-bit Windows self-hosted runner with the labels
`self-hosted`, `Windows`, and `X64`. This keeps the workflow on the same
repository-supported host path as the checked-in PowerShell build scripts and
allows the OHOS job to use a DevEco/OpenHarmony native SDK whose installation
and license acceptance are runner-owner responsibilities.

Pull requests whose head repository is a fork do not execute these persistent
self-hosted jobs. A maintainer must inspect that contribution and move or
reproduce it on a trusted same-repository branch before running the matrix;
the workflow does not use `pull_request_target` to execute untrusted code.

The runner provides:

- Visual Studio 18 2026 with the ClangCL and lld-link components, plus CMake;
- Android SDK command-line tools. The workflow installs the exact missing
  Android NDK `29.0.14206865` and SDK CMake `4.1.2` packages when necessary;
- an OpenHarmony native SDK containing the API 23 toolchain, discoverable
  through `OHOS_SDK_ROOT`, `DEVECO_SDK_HOME`, or the standard DevEco location;
- network access for vcpkg source acquisition and enough local storage for the
  three independent build trees.

Each driver records the selected CMake/build-tool versions, target ABI/API,
dependency prefix, and pinned vcpkg submodule revision in the job log. The
workflow requires vcpkg revision
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

A test that returns the repository's CTest skip code 77 remains a skip. A
runner without an audio endpoint, active HDR display, or qualifying native
device is never converted into a passing device result.

The Android job:

1. runs `ffmpeg/scripts/build-android.ps1` for the repository
   `arm64-android-28-static` dependency prefix and verifies it;
2. cross-compiles the shared and static arm64/API 28 Release trees, including
   the configured native test targets, but does not execute ARM64 binaries;
3. installs each package and links
   `examples/android/install-consumer` against that installed package.

The OHOS job follows the same build/install/consumer boundary for
`arm64-ohos-23-static`. It uses `ffmpeg/scripts/build-ohos.ps1` and
`modern/scripts/build-ohos.ps1`, compiles the configured tests and examples,
then links `examples/ohos/install-consumer`. Windows does not execute the
resulting AArch64 binaries and the job does not create, sign, or install a HAP.

## Dependency cache boundary

Each target has a separate `VCPKG_DEFAULT_BINARY_CACHE`. Its key includes the
target/API, pinned vcpkg revision, and a hash of the FFmpeg manifest, overlay
ports, and triplets. There are no broad restore keys. The cache contains only
vcpkg binary archives; it does not contain or restore
`vcpkg_installed` prefixes.

Consequently, every job still invokes the repository platform dependency build
script and finishes through `verify-install.cmake`, even on a cache hit. Jobs do
not download another workflow's artifact as a dependency fallback. Workflow
artifacts contain only bounded test/configuration/build logs and are retained
for seven days.

## Local reproduction on Windows

Initialize the pinned dependency submodule once:

```powershell
git submodule update --init ffmpeg/vcpkg
```

Run the same complete jobs from the repository root:

```powershell
./modern/scripts/ci/build-windows.ps1 -Parallel 8
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

This switch is a local iteration convenience; the GitHub Actions workflow does
not use it.

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
