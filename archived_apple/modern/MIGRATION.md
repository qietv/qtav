# Archived Apple migration notes

These notes were removed from the active QtAV-to-QtAVCore migration guide on
2026-08-02. They are no longer supported or maintained.

The former Apple migration path replaced legacy Qt renderer/audio concepts
with four optional backend targets:

- `QtAV::RenderMetal` for application-owned Metal presentation;
- `QtAV::AudioCoreAudio` for macOS device output and playback timing;
- `QtAV::HWVideoToolbox` for retained hardware decode frames;
- `QtAV::InteropCVMetal` for zero-copy pixel-buffer plane import.

Applications supplied a VideoToolbox hardware-decode configuration before
opening media. Hardware frames exposed a backend-specific borrowed
`CVPixelBufferRef` and retained decoder resources through copied-frame
lifetime. CVMetal could import supported NV12/P010 planes without invoking the
generic CPU mapping path.

The Metal renderer consumed structured range, primaries, transfer, matrix,
chroma-location, mastering-display, and content-light metadata. The complete
EDR path used an application-owned `CAMetalLayer` and active screen, with
extended-linear BT.2020 output and explicit tone-mapping policy.

On macOS, the CoreAudio sink followed the default device or an explicit
backend-specific device identifier. It negotiated Float32 PCM, used the
portable resampler when needed, and exposed device-master clock and latency
through `AudioSink`.

None of these target names, headers, option variables, or native handle types
remain in the active install surface. See the parent archive README before
using this snapshot for historical comparison.
