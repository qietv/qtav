# FFmpeg dependency subproject agent guide

## Scope

This directory is the reproducible dependency-build subproject for QtAVCore.
It builds FFmpeg 8.1.2 and the player dependency closure for these targets:

| Build host | Target | vcpkg triplet |
| --- | --- | --- |
| macOS | Android arm64-v8a, API 28 | `arm64-android-28-static` |
| macOS | OHOS arm64-v8a, API 23 | `arm64-ohos-23-static` |
| 64-bit Windows | Windows x64, Visual Studio ABI | `x64-windows-static-md` |

macOS is a cross-compilation host only. Do not add macOS or iOS outputs. Do
not claim a Windows change is validated until it has run on Windows.

Read [`ARCHITECTURE.md`](ARCHITECTURE.md) before changing the manifest,
triplets, toolchains, overlay ports, verification rules, or output layout.

## Required dependency policy

- Keep FFmpeg 8.0 or newer; the pinned package is currently FFmpeg 8.1.2.
- Keep GPL and version3 enabled.
- Use OpenSSL, never wolfSSL.
- Keep libsmb2 and its FFmpeg protocol integration.
- Keep Vulkan, libplacebo with glslang, OpenGL/OpenGL ES, and Dolby Vision
  reshaping, libass, and dav1d enabled.
- Preserve FFmpeg's native VVC/H.266 decoder. Do not add VVenC or another
  external VVC encoder.
- Keep the native decoder set available; do not add `--disable-decoders` or a
  hand-maintained decoder allowlist.
- Keep `--enable-lto --enable-small --disable-avdevice --disable-iamf`.
- The parent player links modular FFmpeg libraries. Do not replace the output
  with a single combined `libffmpeg.so` unless the parent architecture changes.

## Repository boundaries

- `vcpkg/` is a pinned upstream submodule. Do not make project-specific edits
  inside it. Put all changes in `ports/`, `triplets/`, or `triplets/toolchains/`.
- Do not commit `build/`, vcpkg packages, buildtrees, downloads, generated
  configuration files, or installed binaries.
- Keep `vcpkg.json`, `vcpkg-configuration.json`, overlay ports, and the
  verifier synchronized when dependencies or features change.
- Keep source and text files UTF-8 without BOM and use LF line endings.
- Preserve `scripts/build-android.sh` and `scripts/build-ohos.sh` as stable,
  directly callable local entry points. CI must call the same scripts.
- Preserve the default package layout under
  `build/<triplet>/vcpkg_installed/<triplet>/`.

## Local build commands

Initialize the pinned vcpkg submodule once:

```sh
git submodule update --init ffmpeg/vcpkg
```

On macOS, with SDKs in their standard locations:

```sh
./ffmpeg/scripts/build-android.sh
./ffmpeg/scripts/build-ohos.sh
```

Non-standard SDK paths may be passed directly:

```sh
./ffmpeg/scripts/build-android.sh /absolute/path/to/android-ndk
./ffmpeg/scripts/build-ohos.sh /absolute/path/to/openharmony-sdk
```

OHOS packaging requires a macOS-native `patchelf`. Install it with
`brew install patchelf`.

On Windows, run from PowerShell on a machine with Visual Studio C++ and C++
Clang tools installed:

```powershell
./ffmpeg/scripts/build-windows.ps1
```

Use `QTAV_FFMPEG_INSTALL_ROOT` on macOS or `-InstallRoot` on Windows only when
an alternate package location is required.

## Toolchain invariants

- Android is arm64-v8a/API 28 with NDK r29 and static dependency libraries.
- OHOS is arm64-v8a/API 23. vcpkg's Linux compatibility model is an
  implementation detail; all compilation must use the OHOS SDK toolchain and
  must never resolve host headers or libraries.
- Windows uses Visual Studio's `clang-cl` and `lld-link`, static dependency
  libraries, and the dynamic MSVC runtime (`/MD`). Keep the narrow FFmpeg LTO
  compatibility for the `msvc:lld-link` pair.
- Never reuse one target's install tree, pkg-config path, or binary cache as a
  different target's installed prefix.
- Fixed self-hosted runners rely on vcpkg's persistent local binary cache. Do
  not add `actions/cache` for the whole archive directory unless the runners
  become ephemeral and measurements show that remote transfer is beneficial.

## Overlay ownership

- `ports/ffmpeg/`: player feature policy, libsmb2 protocol integration, FFmpeg
  portability fixes, and Windows clang-cl/lld-link LTO support.
- `ports/libass/`: mobile/OHOS font-discovery policy and required portability
  fixes.
- `ports/libplacebo/`: glslang discovery and Windows linker normalization.
- `ports/libsmb2/`: pkg-config metadata needed for static Windows Winsock
  linkage.
- `triplets/`: target ABI, linkage, release-only policy, and target identity.
- `triplets/toolchains/`: OHOS SDK redirection and Windows compiler discovery.

Keep patches small, named by purpose, and limited to the target that needs
them. Prefer upstreamable fixes, but retain local patches while the pinned
baseline requires them.

## Validation before finishing

1. Run the directly affected native build script. Android and OHOS validation
   must run on macOS; Windows validation must run on Windows.
2. The script must finish `cmake/verify-install.cmake` successfully.
3. Confirm FFmpeg 8/libavcodec 62, OpenSSL, libsmb2, libass, libplacebo,
   glslang, OpenGL/OpenGL ES, Dolby Vision reshaping, dav1d, and Vulkan outputs
   are present.
4. Confirm wolfSSL and VVenC are absent.
5. Confirm installed FFmpeg CMake metadata has no vcpkg package/build-tree
   paths and the package remains consumable by the parent project.
6. Run `bash -n` on changed shell scripts and parse changed JSON files.
7. Run `git diff --check`.
8. Update `README.md`, `ARCHITECTURE.md`, and the parent `modern/PLAN.md` when
   the supported matrix, feature policy, package contract, or verified status
   changes.

The root `.github/workflows/ffmpeg-dependencies.yml` automatically validates
changes under `ffmpeg/**` on push and pull requests. A green job requires both
the build/verifier step and artifact upload to succeed.
