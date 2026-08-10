# QtAVCore Android player demo

This is the user-facing Android example for manual final playback checks. It
is deliberately separate from `examples/android/`, which remains the
automated connected-device regression harness.

The UI contains:

- a `SurfaceView` video area;
- current time, seek slider, and total duration;
- local-file, remote-URL, play/pause, stop, and full-screen controls;
- live Vulkan, HDR, ZeroCopy, hardware-decode, and Debug switches, with
  rendering controls on the first row and ZeroCopy/hardware decode on a
  separate second row so every switch remains touchable;
- a Debug-controlled top-left status window showing the active native path,
  current presentation FPS, or the exact failure.

The full-screen button or a rotation into landscape moves the complete control
panel onto the bottom of the video. It remains visible for five seconds after
the last touch and then disappears without hiding the independent Debug
window. Tap the video once to reveal the controls again; that reveal gesture
does not activate a newly exposed button. Rotating back to portrait exits
full-screen mode.

## Build

The build is Gradle-free and reproducible from the recorded Darwin
cross-compilation host. macOS is not a supported QtAVCore target; its former
native backends are archived and unmaintained. By default this script uses the
same NDK/API/tool versions as the Android regression harness and consumes the
repository-local arm64/API 28 vcpkg package containing FFmpeg 8.1.2, OpenSSL,
libplacebo 7.351.0, and their transitive dependencies. The FFmpeg
configuration has networking and its OpenSSL TLS backend enabled, together
with common MP4/MOV,
Matroska, AVI, MPEG-TS, FLV, Ogg, MP3, AAC, AC-3, FLAC, and WAV
demux/decode support.

Vulkan application-rendered playback delegates color conversion, tone
mapping, output encoding, and FFmpeg-parsed Dolby Vision RPU reshaping to
libplacebo. The optional libdovi parser is intentionally disabled; FFmpeg's
per-frame `AVDOVIMetadata` is used directly. MediaCodec/AImageReader keeps
decoded pixels off the CPU, with only an external-format-to-FP16 GPU
normalization pass before libplacebo. Dolby Vision frames use an identity
sampler in that pass to preserve raw Y/Cb/Cr for RPU reshaping.

```sh
modern/examples/android_player/build-android-player.sh
```

The signed debug APK is written to:

```text
build/android-player/qtav-core-player.apk
```

No device installation occurs during the build.

The APK assets include third-party notices plus the FFmpeg package's GPLv3,
OpenSSL Apache-2.0, and libplacebo LGPL-2.1-or-later license texts. QtAVCore
source files remain LGPL-2.1-or-later; the selected repository FFmpeg feature
package is a GPLv3 build.

The usual Android tool overrides are supported:

```text
ANDROID_SDK_ROOT
QTAV_ANDROID_NDK_VERSION
QTAV_ANDROID_API
QTAV_ANDROID_COMPILE_SDK
QTAV_ANDROID_BUILD_TOOLS
QTAV_ANDROID_CMAKE_VERSION
QTAV_BUILD_JOBS
QTAV_HOST_PKG_CONFIG
```

Before running the script, build the local dependency package with
`ffmpeg/scripts/build-android.sh` on macOS or
`ffmpeg/scripts/build-android.ps1` on Windows. The package must exist under
`ffmpeg/build/arm64-android-28-static/vcpkg_installed/`; workflow artifacts are
not a supported fallback. The player build does not download or independently
compile target FFmpeg, OpenSSL, or libplacebo.

## Install gate

Installation is intentionally gated because recent Android versions can
require a physical confirmation before installing or replacing a debug app.
First run:

```sh
modern/examples/android_player/run-connected-device.sh
```

It stops before `adb install`. Unlock the one connected device and prepare to
approve its prompt, then explicitly continue:

```sh
QTAV_ANDROID_INSTALL_CONFIRMED=1 \
  modern/examples/android_player/run-connected-device.sh
```

The script makes one install attempt. On a device-side authorization failure
it stops instead of retrying or bypassing the prompt.

## Option behavior

| Hardware | ZeroCopy | Vulkan | Active video path |
| --- | --- | --- | --- |
| off | either | on | software decode → Vulkan upload/render |
| off | either | off | software decode → OpenGL ES upload/render |
| on | on | on | MediaCodec → private AImageReader/AHardwareBuffer → Vulkan |
| on | on | off | MediaCodec → AImageReader/AHardwareBuffer → EGLImage raw YCbCr → OpenGL ES |
| on | off | either | MediaCodec direct Surface presentation |

Hardware decode, ZeroCopy, Vulkan, and HDR are enabled by default, selecting
the MediaCodec/AImageReader/Vulkan/libplacebo path. This keeps Dolby Vision
Profile 5 base-layer Y/Cb/Cr on the GPU while ensuring that FFmpeg's parsed
RPU metadata reaches libplacebo; direct-Surface presentation would bypass
that application-side reshaping and tone mapping. Disable ZeroCopy explicitly
to test codec direct-Surface presentation. While direct-Surface presentation
is selected, changing the Vulkan switch only preserves the renderer preference
for the next application-rendered path: enabled selects Vulkan and disabled
selects OpenGL
ES. The Debug control remains available because it only shows or hides the
top-left diagnostics. HDR remains independent: MediaCodec/Android supplies
the decoded HDR surface and, on Android 15 or newer, the switch controls that
`SurfaceView` layer's desired HDR headroom directly. This headroom request
does not convert MediaCodec's direct-Surface PQ/HLG buffers to SDR.

Changing an option that affects the active path preserves the current
position, rebuilds the affected decoder/renderer resources, and resumes only
if playback was active. Renderer preferences changed while direct-Surface
presentation is active are retained without interrupting playback.

All non-direct video rendering runs on one native render thread. The player
hands MediaCodec frames to the application inside the bounded decode window,
together with their monotonic presentation deadlines. Direct Surface output
uses the core's independent video-decode and presentation workers: encoded
packets are paced before decode and the application releases each small-window
output when its ordinary presentation callback arrives. Both private
AImageReader ZeroCopy paths reserve one of four application pipeline slots,
including the frame currently in the graphics-thread attempt, before releasing
another output. Vulkan waits up to 100 ms for image ownership. OpenGL releases
non-blockingly, retains the exact frame/deadline, and lets the AImageReader
callback wake the native render thread; the reader has two acquisition slots
outside the four-image correlation window so callback coalescing cannot strand
an available image at `MAX_IMAGES_ACQUIRED`. Audio decode/output remains on
separate workers. A pending frame older than the 250 ms timestamp-correlation
window is retired instead of being presented in a recovery burst.

The Debug switch shows or hides the top-left status window without rebuilding
the playback pipeline. Its first lines report the actual Vulkan swapchain or
EGL surface color space; direct-Surface mode instead reports the color metadata
of the buffer being passed through MediaCodec because Android's public
`ANativeWindow` getter does not expose the producer's final per-buffer
dataspace. They also report the effective HDR-switch policy, Android's current
HDR/SDR headroom ratio when available, and the display's desired maximum HDR
content luminance. The remaining status reports application
callbacks/presented, the rolling rate of successful application presents
(`present fps`), render-queue drops,
core decoded/delivered, queue/late drops, maximum queue depth, presentation
starvations/maximum starvation time, render timing, AHardwareBuffer cache
imports/hits, and interop queued/acquired/imported/stale counts. During active
Vulkan playback up to three callbacks may be in the renderer's frames-in-flight
ring; this bounded trailing difference is not a dropped frame. A stable run has
core queue/late drops at `0/0`, no recurring presentation starvation, render
queue drops at zero, and equal interop queued/acquired counts.

After enough valid video timestamps are observed, the demo requests the
inferred fixed source frame rate from its `ANativeWindow` and selects the best
matching advertised display mode: the lowest exact multiple when available,
otherwise the fastest mode. Android remains free to keep another physical
mode; the status line reports the source-rate hint and requested display
target. Playback has no per-frame application logcat output; normal logging is
limited to setup, the first Dolby Vision RPU frame, first presentation, and
errors. The diagnostics count Dolby Vision RPU frames and raw YCbCr imports so
the Profile 5 libplacebo path can be distinguished from codec passthrough.

`HDR` selects `PreferHdr`; disabling it selects deterministic SDR output on an
application renderer. The status line reports whether the actual Vulkan/EGL
surface is HDR or SDR. In direct-Surface mode Android/MediaCodec owns the
decoded surface, so the Vulkan/OpenGL selection does not affect current frames,
but it remains selectable for the next application-rendered path. HDR remains
available. On Android 15 or newer, enabling HDR lets Android choose suitable
headroom for the `SurfaceView`; disabling it requests no headroom above SDR
white. Neither request retags or tone maps direct MediaCodec output, so a PQ
source can remain a BT.2020/PQ HDR compositor layer with this switch disabled;
the Debug window labels this behavior as codec dataspace passthrough. Earlier
Android releases still pass through direct-surface HDR automatically but do
not expose this per-`SurfaceView` headroom control.

`present fps` is measured from successful direct-Surface releases or successful
application-renderer presents over a rolling wall-clock window. It is distinct
from the inferred source `rate hint` and Android's requested `display target`;
it falls to `0.0` when presentation stops.

The OpenGL ES MediaCodec ZeroCopy path creates a private GPU-sampled
`AImageReader`, imports each AHardwareBuffer as an EGLImage, and requires
`GL_OES_EGL_image_external_essl3`, `GL_EXT_YUV_target`, and native-fence EGL
sync. The external sampler exposes normalized Y/Cb/Cr in R/G/B. A crop-aware
RGBA16F normalization pass preserves those components without performing
color conversion; libplacebo then applies Dolby Vision reshaping, color
conversion, tone mapping, and output encoding. A device that cannot provide
this raw contract rejects the hardware frame rather than interpreting Profile
5 samples through an implicit RGB conversion.
Android window submission occurs through the renderer's present callback
before the AImage is returned. The exported EGL native fence therefore covers
`eglSwapBuffers()` as well as sampling; presenting only after `render()`
returned could accumulate unsignalled release fences and stall both video and
the shared demux feed after the AImageReader acquisition quota was exhausted.
For SDR output, the renderer also detects the Android EGL window's sRGB
framebuffer encoding and leaves the final linear-to-sRGB conversion to GL.
This prevents the HDR-off OpenGL ES path from applying sRGB encoding twice and
lifting midtones compared with Vulkan.

Both Vulkan and OpenGL ES AImageReader paths accept and correlate a valid zero
timestamp.

FFmpeg's planar 10-bit 4:2:0 software frames map to
`PixelFormat::YUV420P10` and are supported by both Vulkan and OpenGL ES.
Turning hardware decode off therefore produces a picture for HEVC Main10
media, although real-time 4K software decode still depends on CPU throughput
and may not keep pace on a phone.

## Opening media

`Open local` uses Android's Storage Access Framework and retains the detached
read-only file descriptor for the complete QtAVCore playback session. It is
passed through FFmpeg's seekable `fd:` protocol; the app never attempts to
reopen the provider path or its `/proc/self/fd` symlink.

`Open URL` accepts HTTP or HTTPS and passes the URL directly to QtAVCore.
FFmpeg performs HTTP reads and uses the OpenSSL backend for HTTPS; the Java
shell neither downloads the file nor opens a network connection. Peer and
host verification are enabled explicitly. At startup the Java shell reads the
public Android system trust files, concatenates them into an app-private PEM
bundle, and passes that file to FFmpeg's OpenSSL backend; Java still performs
no network I/O. This remains a remote-file player rather than an
adaptive-streaming example. The demo manifest explicitly allows cleartext
HTTP for manual test servers; production applications should remove that
allowance unless their own network-security policy requires it.

Hardware ZeroCopy does not pre-probe the coded video dimensions. Each private
`AImageReader` starts with a minimal default buffer size; MediaCodec overrides
that default with its decoded output size, and the interop reads each produced
image's actual dimensions and crop. Opening a
remote URL therefore performs one FFmpeg open for playback instead of a
separate metadata request followed by playback.

The decoded image size is independent of the presentation Surface size.
Resizing the video area or changing orientation rebuilds or reconfigures the
Vulkan/EGL presentation target without using the window size as a decoder
buffer size. Republishing the same Android Surface after `surfaceChanged()` is
enough to refresh an in-place Vulkan swapchain or EGL-surface size change. In
the Java `SurfaceView` shell, the settled Android display rotation is also
forwarded to the Vulkan render configuration so View-system buffer transforms
do not change the picture aspect after an orientation change. In
direct-Surface mode the demo fits and centers the `SurfaceView` from the
already-open track or decoded-frame dimensions so MediaCodec does not stretch
the picture to the control area's aspect ratio.

The long-form 4K performance samples used for connected-device checks are
`Download/legend.mkv` and `Download/suzume.mkv`. The matching direct-streaming
URL for `legend.mkv` is
`https://2dland.cn/test/legend_of_the_magnate.mkv`.

Native diagnostics:

```sh
adb logcat -s QtAVCorePlayer
```

## Suggested manual final-test matrix

Use one ordinary H.264/AAC SDR file, one HEVC Main10 HDR10 file, and one
HTTP/HTTPS URL. For each supported device path, verify picture, audible
AAudio output, pause/resume, one slider seek, stop/play, and a
background/foreground Surface recreation:

1. software decode + Vulkan + SDR;
2. software decode + OpenGL ES + SDR;
3. software decode + Vulkan + HDR;
4. hardware decode + ZeroCopy + Vulkan;
5. hardware decode + ZeroCopy + OpenGL ES;
6. hardware decode + ZeroCopy off (direct Surface);
7. Debug off hides the top-left status window without interrupting playback;
8. Debug on restores the status window and reports a stable `present fps`;
9. the full-screen button and landscape rotation both overlay the controls,
   auto-hide them after five seconds, and leave Debug visible.

Also open one file through the Storage Access Framework and one remote file
through both HTTP and HTTPS where test hosting permits. Treat unsupported
device HDR/codec capabilities as explicit unavailable results, not
as successful coverage. For long-form 4K Vulkan or OpenGL ZeroCopy playback, let the
sample run for at least one minute and verify core queue/late drops `0/0`, zero
recurring render-queue drops, bounded interop queued/acquired/imported trailing
counts, and no unbounded callback/presented growth instead of merely checking
that a picture appeared. Repeat OpenGL after fully closing and reopening the
application because surface/buffer-pool reuse is part of this regression gate.
