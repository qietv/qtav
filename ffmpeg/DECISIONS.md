# FFmpeg dependency-build decisions

This document records dependency-build choices whose rationale is not obvious
from the installed package contract alone. `ARCHITECTURE.md` defines the
current structure and invariants; this file explains why narrowly scoped local
patches exist, which alternatives were rejected, how they are validated, and
when they may be removed.

Review the affected decision whenever the pinned vcpkg baseline, FFmpeg,
libplacebo, Visual Studio LLVM toolchain, or Windows SDK is upgraded. A local
compatibility patch should be retired once the pinned upstream versions provide
the same behavior and the stated validation passes without it.

## FD-001: Preserve Windows FFmpeg LTO with a narrow MLP exception

- Date: 2026-08-03
- Status: Accepted
- Scope: Windows x64 `clang-cl`/`lld-link` FFmpeg build

### Context

The Windows dependency package uses Visual Studio's `clang-cl` because the
pinned libplacebo C build does not support the MSVC `cl.exe` compiler. FFmpeg
is built with `--enable-lto`, producing LLVM bitcode archives that are linked
by `lld-link` under the MSVC ABI.

FFmpeg's MLP x86 filter optionally defines non-local dispatch labels such as
`ff_mlp_firorder_*` inside GNU inline assembly and references them from C.
Under Windows clang-cl LTO those labels do not remain externally resolvable at
the final archive/link boundary, causing unresolved-symbol failures even
though FFmpeg's separately assembled NASM objects are otherwise compatible.

### Decision

Keep FFmpeg LTO enabled and keep the external NASM SIMD paths enabled. Under
the specific `__clang__ && _MSC_VER` combination, compile out only the MLP
GNU-inline-assembly non-local-label implementation and use FFmpeg's C fallback
for that filter.

The policy is implemented by:

- [`0052-allow-clang-cl-lld-link-lto.patch`](ports/ffmpeg/0052-allow-clang-cl-lld-link-lto.patch),
  which admits the otherwise compatible compiler/linker driver pair; and
- [`0054-fix-clang-cl-mlp-lto-labels.patch`](ports/ffmpeg/0054-fix-clang-cl-mlp-lto-labels.patch),
  which applies the narrow MLP exception.

### Rejected alternatives

- Disabling LTO for all of FFmpeg would discard a deliberate package-wide
  optimization to work around one inline-assembly implementation.
- Disabling all x86 assembly would unnecessarily remove compatible external
  NASM acceleration.
- Switching the dependency package to `cl.exe` would make the pinned
  libplacebo build unsupported.
- Publishing the assembly labels outside their current translation boundary
  would be a broader FFmpeg source change with higher maintenance risk.

### Consequences and validation

TrueHD/MLP decoding remains available through the C implementation for the
affected filter. The rest of FFmpeg and its external assembly retain LTO and
SIMD support.

Validation requires `scripts/build-windows.ps1`, the installed-package
verifier, and a final QtAVCore static and shared link. Audio regression
coverage must continue to include TrueHD/MLP decode when generated media is
available.

### Retirement condition

Remove the MLP patch only when the pinned FFmpeg/LLVM combination can link the
unmodified implementation with LTO through `lld-link`, followed by the complete
Windows dependency build, verifier, QtAVCore static/shared link, and TrueHD/MLP
decode validation.

## FD-002: Publish the complete static libplacebo D3D11 shader closure

- Date: 2026-08-03
- Status: Accepted
- Scope: Windows x64 libplacebo D3D11 package metadata

### Context

libplacebo's D3D11 backend translates generated SPIR-V to HLSL through
SPIRV-Cross. The repository package is fully static, but the pinned libplacebo
Meson definition requests the shared SPIRV-Cross C dependency. In addition,
the pinned `spirv-cross-c` pkg-config metadata publishes the C wrapper archive
without every C++ backend/core archive called by that wrapper.

The incomplete metadata can build the dependency in isolation yet fail later
when linking QtAVCore's D3D11 shared library or an installed-package consumer.
Windows SDK libraries emitted as `shlwapi.lib` or `version.lib` also need
normalization before FFmpeg/pkg-config consumers can interpret the flags
consistently.

### Decision

Enable libplacebo D3D11 only for the Windows dependency target and use the
static SPIRV-Cross C package. Publish the ordered static C, GLSL, HLSL, MSL,
CPP, reflection, utility, and core archive closure in `libplacebo.pc`, together
with normalized Windows system-library flags.

The policy is implemented by:

- the Windows `spirv-cross` dependency in
  [`ports/libplacebo/vcpkg.json`](ports/libplacebo/vcpkg.json);
- [`0002-fix-windows-shlwapi-linkage.patch`](ports/libplacebo/0002-fix-windows-shlwapi-linkage.patch);
- [`0003-use-static-spirv-cross.patch`](ports/libplacebo/0003-use-static-spirv-cross.patch); and
- the installed pkg-config normalization in
  [`ports/libplacebo/portfile.cmake`](ports/libplacebo/portfile.cmake).

QtAVCore exposes only its D3D11 renderer on Windows. Installing libplacebo's
other compiled capabilities for the shared Android/OHOS dependency policy does
not create a QtAVCore Windows OpenGL or Vulkan backend.

### Rejected alternatives

- Using shared SPIRV-Cross libraries would violate the Windows static
  dependency-package contract.
- Adding the missing archives directly to QtAVCore would leak a dependency-port
  implementation detail and leave other installed consumers broken.
- Disabling D3D11 would prevent the required Windows libplacebo rendering path.
- Enabling QtAVCore Vulkan or OpenGL on Windows would not repair D3D11's static
  shader-translation closure and is outside the supported backend policy.

### Consequences and validation

The installed libplacebo pkg-config module is intentionally more explicit than
the pinned upstream module. Archive order is part of the Windows static-link
contract and must be reviewed during SPIRV-Cross upgrades.

Validation requires `scripts/build-windows.ps1`, verifier checks for
`PL_HAVE_D3D11` and SPIRV-Cross headers, QtAVCore static/shared D3D11 builds,
and static/shared external `QtAV::RenderD3D11` package consumers.

### Retirement condition

Remove the overlay workaround when the pinned libplacebo and SPIRV-Cross/vcpkg
metadata natively expose the complete static closure and all validation above
passes without local archive-list or Windows-library normalization.

## FD-003: Guarantee 16-byte libplacebo allocator alignment on Windows x64

- Date: 2026-08-03
- Status: Accepted
- Scope: Windows x64 clang-cl libplacebo allocation boundary

### Context

The first working static D3D11 link failed at runtime while libplacebo created
a D3D11 render pass. Debugger inspection localized the access violation to
`pl_d3d11_pass_create`: clang-cl emitted an aligned SIMD load, while the
private object returned through libplacebo's parent/public-to-private allocator
boundary was only 8-byte aligned.

The allocator used `alignof(max_align_t)` both for its flexible data member and
for the offset from a public object to its private object. Under the exercised
Windows x64 MSVC ABI this did not guarantee the 16-byte address required by the
generated code. Fixing only one side of that boundary would leave another
allocation layout inconsistent.

### Decision

On `_WIN64`, define the allocator boundary alignment as 16 bytes, apply it to
the flexible data member, and use the same value when rounding the public to
private object offset. Other targets retain their native
`alignof(max_align_t)` behavior.

The policy is implemented by
[`0004-align-windows-clang-allocations.patch`](ports/libplacebo/0004-align-windows-clang-allocations.patch).

### Rejected alternatives

- Disabling compiler optimization or SIMD generation would hide the allocator
  contract violation and impose broad performance costs.
- Patching only the D3D11 pass structure would leave other libplacebo private
  allocations exposed to the same boundary mismatch.
- Switching to `cl.exe` is not supported by the pinned libplacebo C build.
- Adding a QtAVCore-side allocation workaround cannot fix objects allocated
  inside libplacebo.

### Consequences and validation

Windows x64 libplacebo allocations can consume a small amount of additional
padding. No public ABI type changes, and non-Windows targets are unaffected.

Validation requires the complete Windows dependency build and verifier,
followed by WARP and hardware D3D11 renderer creation, Advanced Color output,
static/shared builds, and bounded real Dolby Vision Profile 5 playback through
libplacebo D3D11. A successful link alone is insufficient because this was a
runtime alignment failure.

### Retirement condition

Remove the patch only after an upstream allocator fix with an equivalent
Windows x64 guarantee is present in the pinned libplacebo version and the full
D3D11 runtime validation passes with the local patch absent.

## FD-004: Expose an opaque explicit OHCodec surface-output decision

- Date: 2026-08-05
- Status: Accepted
- Scope: OHOS arm64 FFmpeg OHCodec surface output

### Context

FFmpeg 8.1.2's OHCodec surface decoder owns the output index and decoder
reference in a private structure. Its public frame exposes an `OH_AVBuffer`
pointer, while the last frame release implicitly renders unless the buffer is
marked discard. QtAVCore therefore cannot implement deterministic present,
timed-present, and drop decisions without copying private FFmpeg state or
depending on destructor timing.

The OHCodec buffer-output branch is not an alternative: it maps the native
buffer and copies pixels through `av_image_copy2()`, violating the project's
direct-surface and future zero-CPU-copy boundaries.

### Decision

Patch the OHCodec surface path to expose an opaque `AVOHCodecBuffer` through
installed `libavcodec/ohcodec_surface.h`. The public functions
`av_ohcodec_release_buffer()` and `av_ohcodec_render_buffer_at_time()` make one
native render/drop decision. An atomic flag guarantees that the native output
is decided at most once. A repeated decision through another retained frame
view returns `AVERROR(EALREADY)` rather than reporting a second successful
queue operation. If the last frame reference is released without an explicit
decision, the fallback unconditionally frees/drops the output rather than
rendering it. The opaque token retains FFmpeg's decoder reference until the
frame buffer is released.

The policy is implemented by
[`0055-ohcodec-explicit-surface-release.patch`](ports/ffmpeg/0055-ohcodec-explicit-surface-release.patch).
The installed-package verifier requires the header and both symbols.

### Rejected alternatives

- Reconstructing FFmpeg's private OHCodec structure in QtAVCore would couple
  the backend to an unstable layout and violate the library boundary.
- Calling OHCodec directly from the exposed `OH_AVBuffer` is insufficient
  because the output index and retained decoder are not public.
- Relying on upstream last-frame destruction leaves presentation timing
  implicit and cannot express a deterministic drop.
- Using the FFmpeg buffer-output branch would introduce a decoded-pixel CPU
  map and copy and still would not provide native-buffer GPU ownership.

### Consequences and validation

The OHOS package carries a small public overlay ABI that must stay paired with
the QtAVCore OHCodec backend. This API controls direct surface output only and
makes no native-buffer interop or zero-copy texture claim. Android and Windows
packages are unchanged. QtAVCore maps `AVERROR(EALREADY)` to a stale import;
the interop must not install a new pending producer association because no
second native buffer or frame-available callback will be produced.

Validation requires `scripts/build-ohos.ps1`, `cmake/verify-install.cmake`,
QtAVCore OHOS shared/static target compilation, an installed
`QtAV::HWOHCodec` consumer, and connected-device HAP coverage that observes
successful timed presentations and explicit drops. The 2026-08-05 Mate 60 Pro
run completed 30 timed H.264 presentations and three explicit drops. The
2026-08-13 player regression additionally held `legend.mkv` at 0.5x through
repeated full-screen swapchain recreation with balanced native release and
callback counts.

### Retirement condition

Remove the patch when the pinned upstream FFmpeg OHCodec decoder exposes an
equivalent retained, single-decision surface-output API. Re-run the dependency
build and verifier, QtAVCore shared/static consumption, and the connected
present/drop lifecycle matrix before adopting that replacement.

## FD-005: Reuse a compatible D3D11VA decoder with its frames context

- Date: 2026-08-07
- Status: Accepted
- Scope: Windows x64 FFmpeg 8.1.2 D3D11VA decoder lifetime

### Context

HEVC can call `ff_get_format()` again when an active SPS changes even when the
selected D3D11 device, pixel format, dimensions, decoder texture, and required
configuration remain unchanged. FFmpeg 8.1.2 first calls
`ff_hwaccel_uninit()`, then creates a new `ID3D11VideoDecoder` and a complete
set of output views. Reusing only QtAVCore's initialized
`AVHWFramesContext` therefore reuses the texture array but not the decoder.

Each decoded frame retains the old libavcodec decoder reference. ETW on Intel
Iris Xe showed the final old-frame release entering
`igd11dxva64.dll`/`igddxvacommon64.dll` allocation teardown from QtAVCore's
frame recycler. That teardown serialized the shared D3D11 device while the
renderer itself used less than one millisecond of GPU time. High temperature
amplified the duration, but the decoder reconstruction and release sequence
also reproduced in a cold, non-throttled sample.

### Decision

Patch the Windows D3D11VA path with an opt-in
`AVD3D11VADeviceContext::reuse_decoder` field. When enabled, libavcodec stores
an `AVBufferRef`-owned decoder resource on the initialized
`AVD3D11VAFramesContext`. The resource owns the `ID3D11VideoDecoder`, every
output view, and a texture reference. It is reused only when the texture,
array size, profile GUID, sample dimensions, output format, and complete
decoder configuration still match. The normal FFmpeg path is unchanged when
the field is zero.

QtAVCore enables the extension only for its repository-built D3D11VA device
and also retains a compatible initialized frames context across repeated format
selection. The policy is implemented by
[`0057-d3d11va-reuse-compatible-decoder.patch`](ports/ffmpeg/0057-d3d11va-reuse-compatible-decoder.patch),
and the installed-package verifier requires the public opt-in field.

### Rejected alternatives

- Reusing only the frames context does not retain libavcodec's decoder or
  output views and did not remove the Intel driver teardown.
- Moving final frame destruction to another QtAVCore thread keeps the render
  thread responsive to ordinary release cost but cannot prevent a driver-wide
  serialization on the shared device.
- Forcing software decoding or a decoded-pixel CPU copy would avoid the driver
  path by giving up the required D3D11VA zero-map pipeline.
- Enabling decoder reuse unconditionally would change upstream behavior for
  unrelated FFmpeg consumers and could preserve an incompatible decoder.

### Consequences and validation

The Windows package carries a small public overlay ABI and must stay paired
with the QtAVCore D3D11VA backend. A compatible decoder now lives until the
frames context is destroyed; an incompatible descriptor or configuration still
creates a new decoder. Android and OHOS packages do not enable this path.

Validation completed with `scripts/build-windows.ps1` and
`cmake/verify-install.cmake`, followed by fresh QtAVCore static and shared
Release builds and 37/37 CTest in each tree. The D3D11VA lifecycle test proves
that a seek retains the same decoder texture. Post-fix Iris Xe ETW no longer
contained the recycler-to-`igd11dxva64.dll` decoder teardown stack; remaining
QtAV process allocation events resolved to WinUI/DirectComposition. In a
separate cold run, CPU Package peaked at 72 degrees Celsius with no thermal,
critical-temperature, or power-limit flags. After one seek the renderer
returned from three transient 5-second windows to 24.8-25.2 fps and then held
that cadence with zero coalescing and zero greater-than-80-ms gaps.

### Retirement condition

Remove the patch when the pinned upstream FFmpeg version retains an equivalent
compatible D3D11VA decoder across repeated format selection, or no longer
uninitializes unchanged hardware acceleration in that path. Rebuild and verify
the Windows dependency package, remove the QtAVCore opt-in, pass static/shared
tests, and repeat the cold Intel seek/ETW matrix before adopting the upstream
replacement.

## FD-006: Add a capability-gated OHCodec VVC decoder wrapper

- Date: 2026-08-08
- Status: Accepted
- Scope: OHOS arm64 FFmpeg VVC/H.266 hardware decode

### Context

FFmpeg 8.1.2, official `n9.0`, and current upstream master provide the native
software VVC decoder and `vvc_mp4toannexb` bitstream filter but do not register
an OHCodec VVC wrapper. The recorded Pura X Max advertises and constructs
`OMX.hisi.video.decoder.vvc` for the supplied 1280x720/60 `vvc1` stream, so a
version-only FFmpeg upgrade cannot expose that hardware path to QtAVCore.

### Decision

Patch the OHCodec decoder family with `vvc_ohcodec`. Map `AV_CODEC_ID_VVC` to
`OH_AVCODEC_MIMETYPE_VIDEO_VVC`, select `vvc_mp4toannexb`, and reuse the same
surface-output lifetime contract as H.264/HEVC. Decoder selection must first
query `OH_AVCodec_GetCapabilityByCategory(mime, false, HARDWARE)`; the wrapper
does not fall through to an OHCodec software component. QtAVCore's existing
hardware-open policy reopens the same stream with FFmpeg's native software VVC
decoder when the wrapper is missing, the hardware capability is unavailable,
or hardware open fails.

The policy is implemented by
[`0058-ohcodec-vvc-decoder.patch`](ports/ffmpeg/0058-ohcodec-vvc-decoder.patch).
The installed-package verifier requires `ff_vvc_oh_decoder` and
`ff_vvc_mp4toannexb_bsf` in the OHOS `libavcodec.a` archive.

### Rejected alternatives

- Upgrading FFmpeg alone does not add the missing wrapper.
- Selecting an OHCodec software component would obscure which implementation
  ran and duplicate FFmpeg's already retained native software fallback.
- Feeding MP4 length-prefixed VVC packets directly is incompatible with the
  Annex-B input expected by the exercised hardware decoder.
- Adding an external VVC decoder or encoder would violate the native-decoder
  dependency policy and expand the distribution/licensing surface.

### Consequences and validation

The wrapper is registered only in OHOS builds with OHCodec available; other
targets and FFmpeg's native VVC decoder remain unchanged. On 2026-08-08 the
Windows-hosted OHOS dependency build completed and `verify-install.cmake`
passed. A signed Pura X Max HAP then presented all 600 frames of the supplied
1280x720/60 sample through `OMX.hisi.video.decoder.vvc`, with 694 hardware
frames across EOS, pause/resume, seek/flush, explicit stop, and surface
recreation. Maximum pending output was one and the core video queue remained
zero. A forced missing-device run emitted the hardware-fallback event and
decoded 30 frames from the same media in software without a stale hardware
frame.

### Retirement condition

Remove the patch when a pinned upstream FFmpeg release provides an equivalent
hardware-capability-gated OHCodec VVC wrapper with MP4-to-Annex-B conversion
and the same surface-output contract. Rebuild and verify the dependency
package, rebuild QtAVCore, and repeat the complete connected 600-frame plus
forced-software-fallback lifecycle before adopting it.

## FD-007: Resolve target dependencies through verified local builds

- Date: 2026-08-10
- Status: Accepted
- Scope: Windows, Android, and OHOS dependency acquisition and Android Windows host support

### Context

QtAVCore previously allowed a missing target prefix to be restored from the
newest successful GitHub Actions artifact before attempting a local build.
That made local validation depend on workflow retention, transfer reliability,
and a second package provenance path. Android dependency production was also
documented as macOS-only even though the pinned vcpkg model and Android NDK
toolchain can cross-compile the same arm64/API 28 triplet from Windows.

### Decision

Dependency resolution is local-first and local-only: consume a matching prefix
only when its sibling vcpkg database and installed-package contract are valid;
otherwise run the target's repository entry point and pass
`cmake/verify-install.cmake`. Do not download an Actions artifact as a local
dependency fallback. CI may still upload artifacts for diagnostics or archival
inspection, but parent builds do not consume them.

Add `scripts/build-android.ps1` as the Windows Android entry point. It discovers
the SDK, an already installed NDK, and CMake from Android Studio or explicit
arguments/environment variables. It prints the exact NDK revision and never
installs, upgrades, or replaces SDK components. Android remains arm64-v8a/API
28 with static dependencies and `c++_static`; the target prefix and vcpkg
triplet do not change.

### Rejected alternatives

- Retaining artifact fallback would preserve two dependency-resolution paths
  and would not prove that the current checkout can reproduce its package.
- Automatically invoking `sdkmanager` would mutate the developer machine and
  obscure which NDK the user selected in Android Studio.
- Treating an incomplete artifact download as a local prefix would bypass the
  vcpkg status database and installed-package verifier.
- Reusing OHOS or Windows libraries for Android would violate the target ABI
  and prefix-isolation contract.

### Consequences and validation

The first local build can take longer and may download upstream source archives
through vcpkg, but the produced binaries always come from the current checkout,
overlay set, triplet, and local toolchain. Android package provenance includes
the printed NDK revision. Windows, OHOS, and Android keep independent install
databases and work roots.

Validation requires the directly affected local entry point and
`cmake/verify-install.cmake`. A Windows-hosted Android policy change additionally
requires QtAVCore static and shared arm64/API 28 cross-builds using that local
prefix. No physical-device installation is part of this dependency contract.

The Windows entry point was exercised with Android Studio CMake 4.1.2 and the
already installed NDK `30.0.15729638-beta2`. It locally produced and verified
the FFmpeg 8.1.2 arm64/API 28 prefix, after which QtAVCore static and shared
cross-builds completed against that prefix. No Actions artifact and no SDK/NDK
installation or replacement was used.

### Review condition

Reconsider this policy only if the project adopts a signed, content-addressed
dependency distribution with equivalent checkout provenance and verification.
Until then, workflow artifacts remain outputs rather than build inputs.

## FD-008: Prefer decode throughput over size optimization on OHOS arm64

- Date: 2026-08-11
- Status: Accepted
- Scope: OHOS arm64 FFmpeg native software decoding

### Context

The player dependency feature retained `--enable-small` together with LTO and
the target toolchain's release `-O2`. FFmpeg interprets `--enable-small` as a
request to optimize for size and appends `-Oz`, overriding the earlier `-O2`.
On the connected OHOS arm64 device, a freshly opened low-complexity segment of
4K HEVC Main10 could initially reach its 25 FPS source cadence, but higher
bitrate/complexity segments lost real-time throughput while HEVC residual,
deblocking, and loop-filter functions dominated sampled CPU cycles. ARM64 ASM,
NEON, runtime CPU detection, and LTO were already active, so this was not a
Debug, scalar, or accidentally single-threaded build.

### Decision

Keep the shared `qtav-player` feature policy, including `--enable-small` and
LTO, but append `--optflags=-O3` for the OHOS arm64 triplet. This uses FFmpeg's
supported explicit optimization override and leaves Android and Windows
packages unchanged. Keep runtime CPU detection and generic ARM64 code
generation; do not select one device-specific `-mcpu` or instruction-set
extension for a distributable package.

QtAVCore separately exposes the string property
`avcodec.video.threads` for software-video decoder experiments. Thread-count
selection belongs to the player session, not the dependency package.

### Rejected alternatives

- Retaining effective `-Oz` prioritizes binary size over the measured 4K
  software-decode workload.
- Removing ASM, NEON, or LTO would reduce decode throughput and would not
  address the observed optimization-level mismatch.
- Selecting a device-specific CPU target would make the package unsafe for
  other supported OHOS arm64 devices.
- Applying `-O3` to every target without target evidence would broaden the
  change beyond the measured regression.

### Consequences and validation

Any change to this policy must run `scripts/build-ohos.ps1` locally and pass
`cmake/verify-install.cmake`. The resulting FFmpeg configure record must show
both the retained feature flags and effective `--optflags=-O3`. QtAVCore and
the signed OHOS player HAP must then be rebuilt against that verified prefix,
and the connected-device result must report the requested software-decoder
thread count before comparing `legend.mkv` presentation cadence, CPU load, and
thermal behavior.

The 2026-08-11 validation completed that sequence. The installed configuration
contained `-O3`, LTO, AArch64, and NEON without `-Oz`; the signed player reported
four active frame threads and decoded 4,204 `legend.mkv` frames by position
2:49 (24.9 FPS average). A ten-second device profile attributed 91.6% of cycles
to the four HEVC workers and less than 1% to ArkUI. Presentation totals remained
below decoded totals, so this validates the compiler/thread experiment without
claiming zero end-to-end drops. Detailed counters are retained in
`modern/PLAN_HISTORY_2026-08-11_OHOS_SOFTWARE_DECODE.md`.

### Review condition

Revisit the target-specific override when FFmpeg changes the semantics of
`--enable-small`, when binary-size limits require a separate dependency
profile, or when repeatable device measurements show that `-O2` or another
portable optimization level provides better sustained throughput.

## FD-009: Report a suppressed Android MediaCodec output release as stale

- Date: 2026-08-13
- Status: Accepted
- Scope: Android FFmpeg MediaCodec output ownership and the installed public
  release-result contract

### Context

FFmpeg 8.1.2 shares one atomic release flag across all retained
`AVMediaCodecBuffer` views. Its MediaCodec helpers correctly invoke the native
release only once, and also suppress a release after decoder flush changes the
buffer serial. Both suppressed cases nevertheless returned success. A caller
could therefore register an AImageReader timestamp association and wait for a
frame-available callback even though no native output had been queued.

This becomes reachable when Android republishes the same Surface during a
size/fullscreen refresh. QtAVCore invalidates the old presentation epoch, then
may redraw the retained latest frame in the new epoch. Epoch/timestamp tracking
cannot distinguish a real new producer release from FFmpeg's successful no-op;
the missing callback is an ownership fact, not a slow-producer timeout or an
unknown coded-size problem.

### Decision

Patch both `av_mediacodec_release_buffer()` and
`av_mediacodec_render_buffer_at_time()` so zero means the current call actually
invoked the native output release. Return `AVERROR(EALREADY)` when another view
already released the shared buffer or decoder flush retired it. Keep all other
native error values unchanged.

Document the result in installed `libavcodec/mediacodec.h` and require the
contract from the Android branch of `cmake/verify-install.cmake`. QtAVCore owns
the higher-level mapping to applied, stale, and failed; the dependency layer
does not know about renderer epochs or presentation policy.

### Rejected alternatives

- Suppressing redraws by timestamp/frame identity would be an application
  workaround and could discard a valid frame after a real generation change.
- Waiting less or treating a timeout as success would retain the false pending
  association and hide the ownership violation.
- Reopening MediaCodec or probing the stream dimensions on every surface-size
  change does not make an already-released output emit another callback.
- Keeping the result only inside QtAVCore is impossible because the public
  FFmpeg helper previously erased whether it performed the native release.

### Consequences and validation

Existing callers that check only for negative failure now observe a truthful
stale result. QtAVCore's bool convenience functions remain source compatible
but return true only for a real applied release; result-returning APIs preserve
the three-way distinction.

Any change to this contract must run the directly affected Android dependency
build and `cmake/verify-install.cmake`, followed by QtAVCore Android
shared/static builds and installed-package consumers. Connected-device
validation must exercise a Surface/fullscreen refresh while MediaCodec
hardware output remains active and confirm that presentation continues without
an unbounded pending image wait.
