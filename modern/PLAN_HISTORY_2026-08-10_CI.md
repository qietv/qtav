# Supported-target CI implementation record

Date: 2026-08-10

This record preserves the implementation and local validation evidence for the
supported Windows, Android, and OHOS continuous-integration matrix. The active
task remains partial in [`PLAN.md`](PLAN.md) until the workflow is published and
its first GitHub Actions run completes on the named self-hosted runners.

## Matrix implemented

- Added a path-filtered GitHub Actions workflow with independent Windows,
  Android, and OHOS jobs on 64-bit Windows self-hosted runners. Fork-originated
  pull requests are excluded from those persistent runners; no
  `pull_request_target` execution path was added.
- Pinned checkout, cache, and log-upload actions by immutable commit SHA, pinned
  the vcpkg submodule revision, and pinned the Android NDK and SDK CMake inputs.
- Added target-specific PowerShell drivers for dependency build/verification,
  QtAVCore shared/static configuration, build, installation, and installed-
  package consumption. The Windows driver also runs both full Release CTest
  suites and emits JUnit results.
- Bounded workflow build parallelism at eight and separated each target's
  content-addressed vcpkg binary cache. The cache stores binary archives only;
  every job still invokes the repository dependency script and install
  verifier, and no workflow artifact is accepted as a dependency fallback.
- Kept Android/OHOS ARM execution, signing, application installation, native
  HDR/audio/codec validation, and physical-device qualification outside the
  hosted result. CTest skip code 77 remains a skip on Windows.
- Documented runner ownership, cache boundaries, job coverage, failure logs,
  exact local reproduction commands, and device-only exclusions in
  [`CI.md`](CI.md).

## Dependency metadata correction

The first clean Windows CI configure exposed that the installed
`libplacebo.pc` contained a host-absolute Vulkan SDK include from
`C:\VulkanSDK\1.4.350.0`. `pkgconf` reduced that Windows path to an invalid
`C:VulkanSDK...` include, so a clean consumer could not configure even though
an older CMake cache had hidden the leak.

The Windows libplacebo overlay now emits a relocatable Cflags line containing
only `${includedir}` and `PL_STATIC`. `verify-install.cmake` rejects future
Windows libplacebo metadata containing an absolute drive include. CI drivers
also invalidate the relevant CMake pkg-config cache variables before
reconfiguration so a repaired dependency prefix is observed immediately.

Because this changed `ffmpeg/**`, the directly affected repository dependency
scripts were run locally for Windows, Android, and OHOS. Their final
`cmake/verify-install.cmake` gates passed for:

- `x64-windows-static-md`;
- `arm64-android-28-static`;
- `arm64-ohos-23-static`.

## Windows validation

The repository Windows dependency script completed after rebuilding the
modified libplacebo/FFmpeg closure. The CI driver then reverified that prefix
and configured, built, tested, and installed both ClangCL Release package
forms with Visual Studio 18 2026:

- shared: 60 tests, 0 failures, 0 disabled, 0 skipped; JUnit time 35 seconds;
- static: 60 tests, 0 failures, 0 disabled, 0 skipped; JUnit time 110 seconds.

These suites include deterministic version requests, staged installation, the
standalone installed-package consumer, core/backend tests, and the Windows
D3D11/D3D11VA/WASAPI regression set.

## Android validation

`modern/scripts/ci/build-android.ps1 -Parallel 8` completed from the repository
root with Android NDK `29.0.14206865`, SDK CMake `4.1.2`, arm64-v8a, and API 28.
It rebuilt and verified the repository dependency prefix, cross-built the
configured native targets for shared and static Release packages, installed
both forms, and linked both standalone installed-package consumers.

Windows did not execute the resulting ARM64 binaries and no APK or physical
device operation was attempted.

## OHOS validation

`modern/scripts/ci/build-ohos.ps1 -Parallel 8` completed from the repository
root in 1391.3 seconds with the discovered DevEco/OpenHarmony native SDK,
arm64-v8a, and API 23. It rebuilt and verified the repository dependency
prefix, cross-built the configured tests and examples for shared and static
Release packages, installed both forms, and linked both standalone installed-
package consumers. The final consumer outputs were:

- shared `libqtav_ohos_render_install_consumer.so`: 191,608 bytes;
- static `libqtav_ohos_render_install_consumer.so`: 78,444,864 bytes.

Windows did not execute the resulting AArch64 binaries, create or sign a HAP,
or perform a physical-device operation.

## Remaining publication gate

The workflow has not yet run in GitHub Actions because this implementation has
not been committed and pushed as part of this task. After publication, require
all three jobs to complete at the same revision and retain their bounded logs
before marking the active CI task complete.
