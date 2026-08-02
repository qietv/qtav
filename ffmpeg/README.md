# QtAVCore FFmpeg dependencies

This subproject builds the pinned FFmpeg 8 dependency closure consumed by the
Qt-free player under `../modern/`. It follows the vcpkg overlay model used by
[`qietv/qie-vcpkg-overlay`](https://github.com/qietv/qie-vcpkg-overlay), but
uses an explicit player feature set instead of `ffmpeg[all]`.

Supported build hosts and targets:

| Host | Target | Triplet |
| --- | --- | --- |
| macOS | Android arm64-v8a, API 24 | `arm64-android-24-static` |
| macOS | OHOS arm64-v8a, API 12 | `arm64-ohos-12-static` |
| 64-bit Windows with Visual Studio | Windows x64 | `x64-windows-static-md` |

macOS itself is not a QtAVCore target. Windows dependencies must be built and
validated on Windows; the PowerShell entry point is not a cross-compiler.

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
- FFmpeg's native VVC/H.266 decoder remains enabled because the build never
  disables the native decoder set; no external VVC encoder is included;
- `--enable-lto --enable-small --disable-avdevice --disable-iamf`;
- FFmpeg command-line programs are omitted because the parent player links the
  libraries directly.

The mobile/OHOS libass overlay disables automatic system-font discovery and
does not pull fontconfig. Applications must supply an explicit default font or
subtitle fonts; this avoids treating OHOS as a Linux desktop solely because of
vcpkg's current platform model.

The resulting FFmpeg binaries are GPLv3. Review the complete notices under
each installed triplet's `share/` directory before distribution.

## Checkout

```sh
git submodule update --init ffmpeg/vcpkg
```

The port overlay is self-contained. Do not edit the pinned `vcpkg/` submodule
to add project-specific features.

## Android on macOS

Install Android NDK r29 and export its path:

```sh
export ANDROID_NDK_HOME="$HOME/Library/Android/sdk/ndk/29.0.14206865"
./ffmpeg/scripts/build-android.sh
```

## OHOS on macOS

Set the SDK directory that contains `native/`. A macOS-native `patchelf` is
also required because vcpkg currently models the OHOS target as Linux during
packaging.

```sh
brew install patchelf
export OHOS_SDK_ROOT="$HOME/Library/OpenHarmony/Sdk/23"
./ffmpeg/scripts/build-ohos.sh
```

The OpenHarmony compiler target and CMake platform are fixed to OHOS API 12.
The host SDK may be newer as long as it still supplies that native platform.

## Windows with Visual Studio

Run from 64-bit PowerShell on a Windows machine with Visual Studio C++ tools:

```powershell
git submodule update --init ffmpeg/vcpkg
./ffmpeg/scripts/build-windows.ps1
```

The Windows triplet produces release static libraries using the dynamic MSVC
runtime (`/MD`).

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
  -DVCPKG_TARGET_TRIPLET=arm64-android-24-static \
  -DVCPKG_OVERLAY_TRIPLETS="$PWD/ffmpeg/triplets" \
  -DVCPKG_INSTALLED_DIR="$PWD/ffmpeg/build/arm64-android-24-static/vcpkg_installed"
```

Use the corresponding OHOS or Windows triplet for those targets. The build
scripts accept `QTAV_FFMPEG_INSTALL_ROOT` on macOS and `-InstallRoot` on
Windows when a different artifact directory is needed.

## CI

`.github/workflows/ffmpeg-dependencies.yml` builds Android on a GitHub-hosted
macOS arm64 runner and Windows on a GitHub-hosted Visual Studio runner. OHOS is
manual because the SDK is not installed on public runners: enable the
`build_ohos` workflow input and provide a self-hosted macOS arm64 runner with
the `ohos` label, `OHOS_SDK_ROOT`, and macOS-native `patchelf`.
