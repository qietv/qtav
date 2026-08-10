# QtAVCore 2.0.0 version-contract completion record

Date: 2026-08-10

This record preserves the implementation and validation evidence for the
completed core C++ API and CMake package version contract. The active task order
remains in [`PLAN.md`](PLAN.md).

## Contract implemented

- Confirmed from repository history that `project(QtAVCore VERSION 2.0.0)` has
  been present since the rewrite's initial commit; 2.0.0 is the first formal
  QtAVCore release and is independent of the legacy QtAV ABI.
- Added generated `<qtav/version.h>` macros plus `qtav::coreVersion` and
  `qtav::coreVersionString`, also reachable from `<qtav/qtav.h>`.
- Installed package configs now always publish `QtAVCore_VERSION` and its
  major/minor/patch components, including when `find_package` has no explicit
  version request.
- Retained `SameMajorVersion` package selection and added deterministic probes
  for exact 2.0.0 acceptance, compatible 2.0 acceptance, newer 2.1 rejection,
  and previous-major 1.0 rejection.
- Added a staged installed-package consumer that validates package variables,
  the installed version header, `QtAV::Core`, compilation, and final linking.
- Added a recursive configure-time check that every production shared target
  uses the full project `VERSION` and major `SOVERSION` before examples and
  tests introduce non-package targets.
- Defined separate public-source, shared-ABI-domain, exported-target, package-
  discovery, and future-plugin boundaries in README, migration, architecture,
  and AD-022. No runtime loader or C ABI was added.

## Dependency verification

- `ffmpeg/cmake/verify-install.cmake` passed for the repository Windows
  `x64-windows-static-md` and Android `arm64-android-28-static` prefixes.
- The existing OHOS prefix initially failed current verification because it
  lacked `ff_vvc_oh_decoder`. Running `ffmpeg/scripts/build-ohos.ps1` rebuilt
  FFmpeg 8.1.2 overlay port revision 7 with
  `0058-ohcodec-vvc-decoder.patch`; the script's final
  `cmake/verify-install.cmake` gate passed for `arm64-ohos-23-static`.

## Windows validation

Shared ClangCL Release:

```powershell
cmake -S modern -B build/modern-shared-mpv
cmake --build build/modern-shared-mpv --config Release --parallel
ctest --test-dir build/modern-shared-mpv -C Release --output-on-failure
```

Result: 60/60 passed. This includes all four package-version probes, staged
installation, the separate shared-package consumer, core/backend tests, and the
Windows D3D11/D3D11VA/WASAPI regression set.

Static ClangCL Release:

```powershell
cmake -S modern -B build/modern-static-clang
cmake --build build/modern-static-clang --config Release --parallel
ctest --test-dir build/modern-static-clang -C Release --output-on-failure
```

Result: 60/60 passed, including final linkage of the standalone static-package
consumer.

## Android validation

The locally verified repository `arm64-android-28-static` dependency prefix and
the existing NDK 30/API 28 Ninja configurations were used for both library
forms. Each configuration rebuilt QtAVCore, installed it, then reconfigured and
linked `modern/examples/android/install-consumer` against the installed package.

- shared: 35/35 build steps passed; install and consumer link passed;
- static: 35/35 build steps passed; install and consumer link passed.

The consumer requests `find_package(QtAVCore 2.0 CONFIG REQUIRED)` and compiles
the public version assertions. `llvm-readelf` confirmed Android retains the
NDK-standard unversioned `libqtav_core.so` soname; the public header and CMake
package are therefore the Android release-version authority.

## OHOS validation

The repository Windows OHOS script reverified the rebuilt dependency package,
then configured, built, and installed both arm64/API 23 library forms. Each
installed package was consumed by
`modern/examples/ohos/install-consumer` with the same compatible version request
and public C++ assertions.

- shared: dependency verification passed, 116/116 cross-build steps passed,
  install passed, and the standalone consumer linked;
- static: dependency verification passed, 37/37 cross-build steps passed,
  install passed, and the standalone consumer linked.

The shared install produced full, major, and unversioned names such as
`libqtav_core.so.2.0.0`, `libqtav_core.so.2`, and `libqtav_core.so`;
`llvm-readelf` reports soname `libqtav_core.so.2`. Windows cannot execute these
AArch64 binaries, so this gate is intentionally build/install/package
consumption rather than device runtime validation.
