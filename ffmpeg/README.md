# QtAVCore FFmpeg dependencies

This subproject builds the pinned FFmpeg 8 dependency closure consumed by the
Qt-free player under `../modern/`. It follows the vcpkg overlay model used by
[`qietv/qie-vcpkg-overlay`](https://github.com/qietv/qie-vcpkg-overlay), but
uses an explicit player feature set instead of `ffmpeg[all]`.

See [`ARCHITECTURE.md`](ARCHITECTURE.md) for the package structure and
[`DECISIONS.md`](DECISIONS.md) for the rationale and retirement criteria of
local compatibility patches.

Supported build hosts and targets:

| Host | Target | Triplet |
| --- | --- | --- |
| macOS | Android arm64-v8a, API 28 | `arm64-android-28-static` |
| macOS | OHOS arm64-v8a, API 23 | `arm64-ohos-23-static` |
| 64-bit Windows with DevEco Studio | OHOS arm64-v8a, API 23 | `arm64-ohos-23-static` |
| 64-bit Windows with Visual Studio and Clang tools | Windows x64 | `x64-windows-static-md` |

macOS itself is not a QtAVCore target. Windows dependencies must be built and
validated on Windows; `build-windows.ps1` is not a cross-compiler. The separate
`build-ohos.ps1` entry point uses the DevEco native SDK to cross-compile OHOS.

## Pinned dependency policy

The vcpkg submodule and registry baseline are pinned to commit
`9e593bb18ea69cc5095e012465dcd675a822ed0d`. At that baseline the principal
versions are FFmpeg 8.1.2, OpenSSL 3.6.3, libsmb2 6.2, libass 0.17.5, dav1d
1.5.3, libplacebo 7.351.0, Vulkan-Headers 1.4.350.1, and glslang 16.4.0.

The FFmpeg overlay applies this policy:

- `--enable-gpl --enable-version3`;
- `--enable-openssl` (wolfSSL is not included);
- `--enable-libsmb2`, using the FFmpeg integration patch and protocol sources
  imported from the reference `avbuild2` tree;
- `--enable-vulkan --enable-libplacebo --enable-libass --enable-libdav1d`;
- `--enable-ohcodec` for the OHOS triplet, with H.264 and HEVC wrapper
  decoders plus the opaque explicit surface-output decision API required by
  installed-package verification;
- FFmpeg's native VVC/H.266 decoder remains enabled because the build never
  disables the native decoder set; no external VVC encoder is included;
- `--enable-lto --enable-small --disable-avdevice --disable-iamf`;
- FFmpeg command-line programs are omitted because the parent player links the
  libraries directly.

The libplacebo overlay enables its built-in Dolby Vision reshaping component
with `-Ddovi=enabled`. The separate optional `libdovi` raw-RPU parser remains
disabled because FFmpeg supplies decoded Dolby Vision frame metadata to the
libplacebo integration. The FFmpeg overlay also enables the built-in RPU
decoder for `hevc_mediacodec` and associates parsed metadata with hardware
output presentation timestamps; it does not add an external parser. This
dependency capability is not a Dolby licensing, certification, or
end-to-end playback claim.

The overlay also builds libplacebo with `-Dopengl=enabled`. Its build virtual
environment supplies glad 2 to generate the merged OpenGL, OpenGL ES, and EGL
loader. Android and OHOS therefore install libplacebo with
`PL_HAVE_OPENGL 1`. Windows additionally enables libplacebo's D3D11 backend
and installs the complete static SPIRV-Cross C/glsl/hlsl/msl/cpp/reflect
closure. QtAVCore uses only that D3D11 backend on Windows; the installed
OpenGL capability is not a QtAVCore Windows rendering path. See
[FD-002](DECISIONS.md) for the static-link contract.

The mobile/OHOS libass overlay disables automatic system-font discovery and
does not pull fontconfig. Applications must supply an explicit default font or
subtitle fonts; this avoids treating OHOS as a Linux desktop solely because of
vcpkg's current platform model.

The OHOS FFmpeg overlay installs `libavcodec/ohcodec_surface.h`. Its opaque
token permits exactly one immediate render, monotonic timed render, or drop of
an OHCodec surface output without exposing FFmpeg's private decoder structure.
Releasing the last frame reference without an explicit decision unconditionally
drops the output; it never implicitly renders an abandoned output.
It does not expose `OH_AVBuffer`/`OH_NativeBuffer` texture interop. The API and
retirement criteria are recorded in [FD-004](DECISIONS.md).

The resulting FFmpeg binaries are GPLv3. Review the complete notices under
each installed triplet's `share/` directory before distribution.

## Checkout

```sh
git submodule update --init ffmpeg/vcpkg
```

The port overlay is self-contained. Do not edit the pinned `vcpkg/` submodule
to add project-specific features.

## Android on macOS

Install Android NDK r29. The script automatically checks Android Studio's
standard path (`$HOME/Library/Android/sdk/ndk/29.0.14206865`), so the normal
local invocation is:

```sh
./ffmpeg/scripts/build-android.sh
```

For a non-standard installation, pass the NDK root directly or set
`ANDROID_NDK_HOME`/`ANDROID_NDK_ROOT`:

```sh
./ffmpeg/scripts/build-android.sh /absolute/path/to/android-ndk
```

## OHOS on macOS

The script automatically checks `$HOME/Library/OpenHarmony/Sdk/23`. A
macOS-native `patchelf` is also required because vcpkg currently models the
OHOS target as Linux during packaging.

```sh
brew install patchelf
./ffmpeg/scripts/build-ohos.sh
```

For a non-standard installation, pass the SDK root containing `native/`
directly or set `OHOS_SDK_ROOT`:

```sh
./ffmpeg/scripts/build-ohos.sh /absolute/path/to/openharmony-sdk
```

The OpenHarmony compiler target and CMake platform are fixed to OHOS API 23.

## OHOS on Windows

Run from 64-bit PowerShell with DevEco Studio/OpenHarmony native SDK and Visual
Studio C++ Build Tools installed:

```powershell
git submodule update --init ffmpeg/vcpkg
./ffmpeg/scripts/build-ohos.ps1
```

The script locates the normal DevEco SDK automatically. Use `-SdkRoot` for a
non-standard SDK, `-InstallRoot` for a different vcpkg database, or `-WorkRoot`
for a different space-free work directory. Because DevEco is normally under
`Program Files`, the script creates a stable no-space SDK junction below the
work root for OpenSSL/FFmpeg MSYS build steps. Windows `patchelf.exe` is
acquired by vcpkg when required; a separate WSL or macOS installation is not
needed.

See [`../modern/OHOS_WINDOWS.md`](../modern/OHOS_WINDOWS.md) for the complete
dependency plus QtAVCore build and install workflow.

## Windows x64 target with Visual Studio

Run from 64-bit PowerShell on a Windows machine with Visual Studio C++ tools
and the **C++ Clang tools for Windows** component installed:

```powershell
git submodule update --init ffmpeg/vcpkg
./ffmpeg/scripts/build-windows.ps1
```

The script stops before bootstrapping vcpkg unless it is running on 64-bit
Windows in a 64-bit PowerShell process and can locate CMake, Visual Studio's
`clang-cl`, and `lld-link`. It prints the resolved compiler, linker, triplet,
and install root before starting the manifest installation.

libplacebo does not support the MSVC `cl.exe` C compiler. The Windows triplet
therefore uses Visual Studio's `clang-cl` compiler with the Windows SDK and
MSVC ABI. It produces release static libraries using the dynamic MSVC runtime
(`/MD`). FFmpeg keeps LTO and external NASM enabled with one narrow MLP
inline-assembly exception, and libplacebo preserves 16-byte allocation
alignment across its Windows x64 public/private boundary. These are intentional
compatibility contracts; see [FD-001 and FD-003](DECISIONS.md).

## Outputs and parent-project consumption

The default install root is:

```text
ffmpeg/build/<triplet>/vcpkg_installed/<triplet>/
```

It contains modular static FFmpeg libraries (`avcodec`, `avformat`,
`avfilter`, `avutil`, `swresample`, and `swscale`) plus their transitive
dependencies, headers, CMake/pkg-config metadata, and license notices. This is
intentional: QtAVCore already discovers the modular FFmpeg libraries, and no
combined `libffmpeg.so` is generated.

Point a QtAVCore target build at the same install root and overlay triplet:

```sh
cmake -S modern -B build/modern-android \
  -DCMAKE_TOOLCHAIN_FILE="$PWD/ffmpeg/vcpkg/scripts/buildsystems/vcpkg.cmake" \
  -DVCPKG_TARGET_TRIPLET=arm64-android-28-static \
  -DVCPKG_OVERLAY_TRIPLETS="$PWD/ffmpeg/triplets" \
  -DVCPKG_INSTALLED_DIR="$PWD/ffmpeg/build/arm64-android-28-static/vcpkg_installed"
```

Use the corresponding OHOS or Windows triplet for those targets. The build
scripts accept `QTAV_FFMPEG_INSTALL_ROOT` on macOS and `-InstallRoot` on
Windows when a different artifact directory is needed. For a Windows-hosted
OHOS parent build, prefer `modern/scripts/build-ohos.ps1`, which supplies the
required vcpkg and OHOS chainload toolchains together.

See [`ARCHITECTURE.md`](ARCHITECTURE.md) for the subproject's layering,
dependency graph, target model, and parent-project integration contract, and
[`DECISIONS.md`](DECISIONS.md) for compatibility-patch rationale.
Maintenance rules for agents and contributors are in [`AGENTS.md`](AGENTS.md).

## CI

`.github/workflows/ffmpeg-dependencies.yml` builds Android and OHOS on a
self-hosted macOS arm64 runner and Windows on a self-hosted Visual Studio
runner. The OHOS job uses `OHOS_SDK_ROOT` when set and otherwise checks
`$HOME/Library/OpenHarmony/Sdk/23`; macOS-native `patchelf` must be installed.
This describes the current CI assignment only; local OHOS builds are supported
on both macOS and Windows.
