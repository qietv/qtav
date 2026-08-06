# Building QtAVCore for OHOS on Windows

QtAVCore and its repository-pinned FFmpeg dependency closure can be
cross-compiled for OHOS arm64/API 23 directly on 64-bit Windows. WSL is not
required. The supported Windows route uses PowerShell plus the native tools
bundled with the OpenHarmony SDK installed by DevEco Studio.

## Prerequisites

- 64-bit Windows and 64-bit PowerShell;
- DevEco Studio with an OpenHarmony SDK containing `native/`;
- Git and the repository's pinned `ffmpeg/vcpkg` submodule;
- Visual Studio C++ Build Tools for vcpkg host utilities;
- network access for source downloads when the vcpkg download/cache is empty.

The scripts look for the SDK in this order: `-SdkRoot`, `OHOS_SDK_ROOT`,
`OHOS_NDK`, `DEVECO_SDK_HOME`, then the normal DevEco Studio installation
under `Program Files`. The target remains arm64-v8a/API 23 even when a newer
compatible SDK is installed.

Initialize vcpkg once from the repository root:

```powershell
git submodule update --init ffmpeg/vcpkg
```

## Build FFmpeg dependencies

Run the Windows OHOS dependency entry point:

```powershell
./ffmpeg/scripts/build-ohos.ps1
```

For a non-standard SDK or output location:

```powershell
./ffmpeg/scripts/build-ohos.ps1 `
  -SdkRoot D:/OpenHarmony/Sdk/openharmony `
  -InstallRoot D:/qtav-ohos/vcpkg_installed `
  -WorkRoot D:/qtav-ohos/vcpkg-work
```

`WorkRoot` must not contain spaces. DevEco normally resides below
`C:\Program Files`; OpenSSL and FFmpeg invoke MSYS tools which cannot reliably
consume that compiler path. The script therefore creates a stable, space-free
SDK junction under the work root. It does not modify the installed SDK. Choose
a different work root when intentionally switching between SDK installations.

The default package database and target prefix are:

```text
ffmpeg/build/arm64-ohos-23-static/vcpkg_installed/
├── vcpkg/
└── arm64-ohos-23-static/
```

The FFmpeg overlay requires `--enable-ohcodec` on this triplet. The final
verifier uses the SDK's `llvm-nm` to require both `ff_h264_oh_decoder` and
`ff_hevc_oh_decoder` plus the explicit surface release/timed-render symbols in
`libavcodec.a`, and requires the installed `ohcodec_surface.h`; a configure-
time fallback to an OHCodec-disabled or implicit-release-only package is
therefore an error.

## Build and install QtAVCore

The normal command builds missing dependencies, then configures, builds, and
installs a shared Release SDK:

```powershell
./modern/scripts/build-ohos.ps1
```

The default output is `build/modern-ohos-shared/`, with its install tree under
`build/modern-ohos-shared/install/`. To reuse an already verified dependency
database without invoking vcpkg again:

```powershell
./modern/scripts/build-ohos.ps1 -SkipDependencies
```

This skips compilation, not validation: `verify-install.cmake` still rejects
an older dependency package that does not contain the required OHCodec decoder
and explicit surface-output API symbols.

Build a static SDK with:

```powershell
./modern/scripts/build-ohos.ps1 `
  -LibraryType Static `
  -BuildDirectory build/modern-ohos-static
```

Useful options include `-BuildType`, `-BuildTests`, `-BuildExamples`,
`-InstallPrefix`, `-NoInstall`, and `-Parallel`. Cross-compiled tests and
examples can be built on Windows but cannot be executed as Windows programs;
runtime tests must be packaged in an OHOS application and run on a device.

For an external CMake consumer, set `QtAVCore_DIR` to the installed
`lib/cmake/QtAVCore` directory. The OHOS SDK intentionally restricts package
searches to its sysroot, so `CMAKE_PREFIX_PATH` alone may not discover an SDK
installed elsewhere on the Windows filesystem.

The script explicitly chainloads the repository OHOS toolchain through vcpkg.
Shared QtAVCore libraries receive `-Wl,-Bsymbolic` from the project CMake rules
so the PIC static FFmpeg closure, including its AArch64 assembly, can be linked
without interposable relocation failures. FFmpeg's pkg-config metadata supplies
libsmb2, OpenSSL, dav1d, and the OHCodec system libraries to the final link.

## Packaging and signing

The SDK scripts above produce native libraries and CMake package metadata.
The XComponent example adds a thin HAP staging and connected-device layer:

```powershell
./modern/examples/ohos/build-ohos-hap.ps1 `
  -ProjectRoot C:/path/to/signed-project
./modern/examples/ohos/run-connected-device.ps1 `
  -ProjectRoot C:/path/to/signed-project `
  -BundleName com.example.qtav
```

The repository template intentionally contains no signing identity. Passing
an existing DevEco project preserves its signing configuration, copies only
the example page/type declarations and arm64 libraries, packages generated
test media, and lets Hvigor produce its signed HAP. The device script installs
once, launches `EntryAbility`, and collects the native PASS/FAIL marker. The
current marker requires generated-media software decode through forced initial
OpenGL ES selection, a fresh Vulkan session, and injected fatal one-way
fallback to OpenGL ES without a media reopen. If
installation pauses for device-side approval, approve it manually before
retrying; the script does not bypass that authorization.

When diagnosing a manual CMake invocation, prefer the supported script. A
compiler test that links Windows system libraries means the OHOS chainload
toolchain was omitted. A shared-link `R_AARCH64_ADR_PREL_PG_HI21` failure means
the QtAVCore OHOS `-Bsymbolic` rule was bypassed or removed.
