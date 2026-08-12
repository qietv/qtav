# OHOS four-thread software-decode and FFmpeg optimization record

Date: 2026-08-11

This record covers the user-directed four-thread FFmpeg software-decode test
for `legend.mkv`. It is separate from the OHCodec production path and from the
strict explicit-plane Vulkan gate.

## Change boundary

- QtAVCore accepts `avcodec.video.threads` as a software-video decoder option
  on the next decoder open and emits `decoder.software.configuration` with the
  actual thread count and active FFmpeg thread type.
- The OHOS player demo requests four threads for its diagnostic software path.
- The repository OHOS arm64 FFmpeg package retains `--enable-small`, LTO,
  runtime CPU detection, AArch64 assembly, and NEON, but appends
  `--optflags=-O3`. Android and Windows policy is unchanged.
- No device-specific `-mcpu` is used, so the distributable arm64 package does
  not assume one Huawei CPU implementation.

## Build and package verification

The directly affected dependency build ran locally:

```powershell
ffmpeg/scripts/build-ohos.ps1
```

It completed and invoked `cmake/verify-install.cmake` successfully for
`ffmpeg/build/arm64-ohos-23-static/vcpkg_installed/arm64-ohos-23-static`.
The generated FFmpeg configuration recorded `--enable-lto`, `--enable-small`,
`--optflags=-O3`, and `--enable-ohcodec`. Its effective compiler flags
contained `-O3` and `-flto`, did not contain `-Oz`, and recorded AArch64 plus
NEON support.

The player package was then rebuilt against that verified prefix:

```powershell
modern/examples/ohos/build-ohos-player-hap.ps1 -Parallel 8
```

The native build linked all required targets, Hvigor produced a signed HAP,
and replacement installation succeeded for `org.qtavcore.playerdemo` on the
connected device.

## `legend.mkv` device result

The player selected FFmpeg software decode, Vulkan, native BT.2020/PQ HDR, and
the `XComponentType.SURFACE` carrier. The media was 3840x2160 HEVC Main10/PQ at
25 FPS with E-AC-3 audio.

The open event reported:

```text
decoder.software.configuration: Software video decoder threads=4 activeThreadType=1
```

`activeThreadType=1` is FFmpeg frame threading. `top -H` exposed exactly four
workers, `av:hevc:df0` through `av:hevc:df3`. In a loaded segment all four were
near saturation; no fifth through ninth HEVC worker existed.

At playback position 2:29, the overlay reported a rolling successful-
presentation rate of 25.0 FPS. At 2:49 the paused snapshot recorded:

| Counter | Value |
| --- | ---: |
| Software-decoded frames | 4,204 |
| Average decoded cadence | 24.9 FPS |
| Successful presentations | 3,671 |
| Player queue/late drops | 28 |
| Hardware-decoded frames | 0 |

The decoded cadence therefore held the 25 FPS source rate through the sustained
sample. The 533-frame decoded/presented total difference is not explained by
the Player queue/late-drop counter and is retained as a separate presentation-
scheduling/coalescing observation. This run validates the software decoder
throughput change but does not redefine a decoded frame as a presented frame.

The device began near 32.7 degrees C at the `system_h` sensor. After the
sustained run it reported 37.2 degrees C, with a 35.0 degrees C battery sensor.
Playback was paused after collection.

## CPU profile

A ten-second 199 Hz `hiperf` cycle profile captured 13,085 samples with zero
lost samples. Thread aggregation was:

| Thread/group | CPU cycles |
| --- | ---: |
| `av:hevc:df0` | 23.92% |
| `av:hevc:df3` | 23.14% |
| `av:hevc:df1` | 22.52% |
| `av:hevc:df2` | 22.04% |
| Native player/render work | 6.51% |
| GPU worker | 0.78% |
| ArkUI process | 0.56% |
| Audio callback | 0.43% |

The four HEVC workers totaled 91.62%. The largest named functions were
`ff_hevc_hls_residual_coding` at 13.62%,
`ff_hevc_v_loop_filter_luma_10_neon` at 6.21%, `memcpy` at 3.90%,
`ff_hevc_hls_filter` at 2.63%, and the 10-bit NEON chroma loop filter at 1.52%.
The Vulkan driver accounted for 0.96% and the QtAVCore Vulkan renderer for
0.26% at DSO level.

Ace layout code accounted for 0.12%, the ArkUI slider library for 0.02%, and
Skia for 0.01%. This objective profile rejects the information panel, progress
bar, and subtitle/bottom text as the primary cause of software decode stalls.

## Comparison boundary

The earlier installed package used effective `-Oz` and FFmpeg automatic thread
selection, which opened nine HEVC workers. On the same media it fell to about
10.7 FPS near position 0:26 while the process used roughly 742% CPU and the
`system_h` sensor approached 43 degrees C. The new `-O3` plus four-thread run
maintained decode cadence with less parallel CPU demand and lower temperature.

Because optimization level and thread count changed together, this comparison
does not claim that four threads alone are faster than nine, or quantify the
isolated gain from `-O3`. A future tuning matrix may compare automatic, four,
six, and eight threads using the same optimized package and fixed media
segments. The current production conclusion is narrower: the old `-Oz` package
was unsuitable for this sustained 4K HEVC software workload, four optimized
frame threads are sufficient to decode this sample in real time on the tested
device, and application UI redraw is not the dominant CPU consumer.
