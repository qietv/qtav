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
is decided at most once. If the last frame reference is released without an
explicit decision, the fallback unconditionally frees/drops the output rather
than rendering it. The opaque token retains FFmpeg's decoder reference until
the frame buffer is released.

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
packages are unchanged.

Validation requires `scripts/build-ohos.ps1`, `cmake/verify-install.cmake`,
QtAVCore OHOS shared/static target compilation, an installed
`QtAV::HWOHCodec` consumer, and connected-device HAP coverage that observes
successful timed presentations and explicit drops. The 2026-08-05 Mate 60 Pro
run completed 30 timed H.264 presentations and three explicit drops.

### Retirement condition

Remove the patch when the pinned upstream FFmpeg OHCodec decoder exposes an
equivalent retained, single-decision surface-output API. Re-run the dependency
build and verifier, QtAVCore shared/static consumption, and the connected
present/drop lifecycle matrix before adopting that replacement.
