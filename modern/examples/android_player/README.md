# QtAVCore Android player demo

This is the user-facing Android example for manual final playback checks. It
is deliberately separate from `examples/android/`, which remains the
automated connected-device regression harness.

The UI contains:

- a `SurfaceView` video area;
- current time, seek slider, and total duration;
- local-file, remote-URL, play/pause, and stop controls;
- live Vulkan, HDR, ZeroCopy, hardware-decode, and Vulkan debug-layer
  switches, with rendering controls on the first row and ZeroCopy/hardware
  decode on a separate second row so every switch remains touchable;
- a status line showing the active native path or the exact failure.

## Build

The build is Gradle-free and reproducible from macOS. By default it uses the
same NDK/API/tool versions as the Android regression harness and builds
checksum-pinned OpenSSL 3.5.7 plus FFmpeg 8.1.2. The FFmpeg configuration has
networking and its OpenSSL TLS backend enabled, together with common MP4/MOV,
Matroska, AVI, MPEG-TS, FLV, Ogg, MP3, AAC, AC-3, FLAC, and WAV
demux/decode support.

```sh
modern/examples/android_player/build-android-player.sh
```

The signed debug APK is written to:

```text
build/android-player/qtav-core-player.apk
```

No device installation occurs during the build.

The APK assets include third-party notices plus the LGPL-2.1 and Apache-2.0
license texts for the statically packaged QtAVCore/FFmpeg and OpenSSL code.

The usual Android tool overrides are supported:

```text
ANDROID_SDK_ROOT
QTAV_ANDROID_NDK_VERSION
QTAV_ANDROID_API
QTAV_ANDROID_COMPILE_SDK
QTAV_ANDROID_BUILD_TOOLS
QTAV_ANDROID_CMAKE_VERSION
QTAV_BUILD_JOBS
```

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
| on | on | off | MediaCodec → SurfaceTexture/external-OES → OpenGL ES |
| on | off | either | MediaCodec direct Surface presentation |

Hardware decode is enabled and ZeroCopy is disabled by default, selecting
MediaCodec direct-Surface presentation. On the connected 4K test device this
path keeps every player callback (`callbacks/presented N/N`); enable ZeroCopy
explicitly when testing GPU texture interop or application-side video
processing. While direct-Surface presentation is selected, the Vulkan, HDR,
and debug-layer controls are disabled because no application renderer exists;
Android controls the surface color/HDR presentation.

Changing an option preserves the current position, rebuilds the affected
decoder/renderer resources, and resumes only if playback was active.

All non-direct video rendering runs on one native render thread. The player
hands MediaCodec frames to the application inside the bounded decode window,
together with their monotonic presentation deadlines. Direct Surface output
uses the core's independent video-decode and presentation workers: encoded
packets are paced before decode and the application releases each small-window
output when its ordinary presentation callback arrives. The Vulkan ZeroCopy
path reserves one of four application pipeline slots before releasing an output,
waits up to 100 ms for the private AImageReader to acquire it, and only then
allows the video-decode worker to advance. Audio packets are decoded on a
separate worker, so E-AC-3 conversion/output backpressure cannot periodically
starve video packet delivery. This prevents producer
bursts from silently coalescing AImages without waiting for the later Vulkan
render deadline. Renderer `RedrawRequested` events are routed back to the
native render thread. A pending frame older than the 250 ms timestamp
correlation window is retired instead of being presented in a recovery burst.

The status line reports application callbacks/presented, render-queue drops,
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
limited to setup, first presentation, and errors.

`HDR` selects `PreferHdr`; disabling it selects deterministic SDR output.
The status line reports whether the actual Vulkan/EGL surface is HDR or SDR.
In direct-Surface mode Android/MediaCodec owns composition, so the application
renderer selection and explicit HDR output switch are inactive. Disable
hardware decode or enable ZeroCopy to activate those renderer controls.

`Debug layer` is an exact Vulkan validation request. It enables
`VK_LAYER_KHRONOS_validation` plus `VK_EXT_debug_utils` and sends validation
messages to the `QtAVCorePlayer` logcat tag. If the device has no validation
layer installed or enabled, pipeline creation fails visibly; turn the switch
off to continue. It is inactive on the OpenGL ES path.

The OpenGL ES MediaCodec ZeroCopy path keeps HDR external-OES source sampling
disabled because that capability requires separate device/codec/driver color
validation. HDR output selection still applies to supported sources and the
software path. A P010/HDR source therefore reports this path as unavailable
instead of continuing with a black surface; use Vulkan ZeroCopy or turn
ZeroCopy off for MediaCodec direct-Surface presentation.

SurfaceTexture cannot distinguish a legitimate zero presentation timestamp
from its initial no-image timestamp. That path therefore drops only a
zero-PTS first codec output and begins correlation at the first positive
timestamp; the counter can consequently end at `callbacks/presented N/N-1`.
The Vulkan AImageReader path accepts and correlates a valid zero timestamp.

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

Hardware ZeroCopy needs the coded video dimensions before its private
MediaCodec target is created. Local document dimensions come from
`MediaMetadataRetriever`; remote dimensions are probed asynchronously with
`libavformat`, using the same FFmpeg/OpenSSL libraries and verified CA bundle
as playback. If the container cannot report dimensions, the status line asks
the user to disable ZeroCopy or hardware decode for that file.

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
7. Vulkan debug layer off;
8. Vulkan debug layer on, either with validation messages or the documented
   unavailable-layer error followed by a successful off-state recovery.

Also open one file through the Storage Access Framework and one remote file
through both HTTP and HTTPS where test hosting permits. Treat unsupported
device HDR/validation/codec capabilities as explicit unavailable results, not
as successful coverage. For long-form 4K Vulkan ZeroCopy playback, let the
sample run for at least one minute and verify core queue/late drops `0/0`, zero
render-queue drops, equal interop queued/acquired counts, and no unbounded
growth in the at-most-three callback/presented frames in flight instead of
merely checking that a picture appeared.
