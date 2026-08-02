# Archived Apple production milestone

Status: completed historically, archived 2026-08-02, no longer maintained.

## Metal

- [x] Objective-C++ backend isolated from core headers.
- [x] Borrowed device, command queue, and current-target callback.
- [x] NV12/P010/YUV/RGB upload and shader conversion.
- [x] Resize, viewport, aspect ratio, rotation, and redraw.
- [x] Extended-linear BT.2020 EDR layer configuration and HDR10/HLG metadata.
- [x] Live macOS/iOS display-headroom adaptation and FP16 validation.

## Audio and hardware decode

- [x] CoreAudio device output with format negotiation, latency, and clock.
- [x] VideoToolbox hardware decode with retained pixel-buffer lifetime.
- [x] CVMetal zero-copy NV12/P010 plane import.
- [x] Explicit software fallback and lifecycle coverage.

## Historical acceptance

- [x] Native macOS A/V playback without Qt.
- [x] Software and VideoToolbox decode paths.
- [x] Resize, pause, seek, media replacement, and shutdown coverage.
- [x] No Apple SDK type in core public headers.
- [x] Static/shared build, sanitizer, install-consumer, and CTest coverage.
- [x] iOS 16 arm64 Objective-C++ syntax build.

These checks are frozen historical results and must not be used as current
support claims. The active implementation plan contains no Apple development
work.
