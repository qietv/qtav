// SPDX-License-Identifier: LGPL-2.1-or-later
#pragma once

#if !defined(__ANDROID__)
#  error "qtav/mediacodec_hardware_decoder.h is available only on Android"
#endif

#include <android/native_window.h>

#include <cstdint>
#include <memory>

#include <qtav/frame.h>
#include <qtav/hardware_decoder.h>
#include <qtav/hw_mediacodec_export.h>

namespace qtav {

// Reference-counted versioned direct-output surface token. Construction
// acquires the ANativeWindow; copies share that reference and generation.
// Construct a new token whenever the application publishes a replacement
// native-window generation.
class QTAV_HW_MEDIACODEC_EXPORT MediaCodecSurface final {
public:
    MediaCodecSurface() noexcept;
    explicit MediaCodecSurface(ANativeWindow* window);
    ~MediaCodecSurface();

    MediaCodecSurface(const MediaCodecSurface&) noexcept;
    MediaCodecSurface(MediaCodecSurface&&) noexcept;
    MediaCodecSurface& operator=(const MediaCodecSurface&) noexcept;
    MediaCodecSurface& operator=(MediaCodecSurface&&) noexcept;

    explicit operator bool() const noexcept;
    bool isValid() const noexcept;
    ANativeWindow* nativeWindow() const noexcept;
    std::uint32_t generation() const noexcept;

private:
    class Impl;
    std::shared_ptr<const Impl> impl_;
};

struct QTAV_HW_MEDIACODEC_EXPORT MediaCodecHardwareDecodeOptions {
    // If wrapper selection, codec configuration, or surface setup fails,
    // reopen the same stream with its ordinary FFmpeg software decoder.
    bool allowSoftwareFallback = true;
    // Bounds extra decoder outputs retained by QtAVCore and application
    // callbacks. The core clamps the value to its supported range.
    int extraHardwareFrames = 4;
};

// Selects FFmpeg's explicit "*_mediacodec" wrapper decoder and binds it to
// the supplied ANativeWindow generation. An invalid surface returns a
// required-but-empty device token so Player's fallback policy remains
// observable and deterministic.
QTAV_HW_MEDIACODEC_EXPORT HardwareDecodeConfig
mediaCodecHardwareDecodeConfig(
    const MediaCodecSurface& surface,
    MediaCodecHardwareDecodeOptions options = {});

class MediaCodecFrame;

// Result of the one permitted decision for a MediaCodec surface output.
// AlreadyReleased means the shared FFmpeg output was previously decided or
// retired by a decoder flush; no new Surface buffer will be produced.
enum class MediaCodecFrameDecisionResult {
    Applied,
    AlreadyReleased,
    Failed,
};

// Validates that frame is a pending MediaCodec direct-surface output for the
// exact supplied window generation. Stale or foreign surface outputs return
// an empty value.
QTAV_HW_MEDIACODEC_EXPORT MediaCodecFrame
mediaCodecFrame(
    const VideoFrame& frame,
    const MediaCodecSurface& surface) noexcept;

// Move-only decision token for one FFmpeg MediaCodec output buffer. The
// source VideoFrame keeps the native output alive. Exactly one of present(),
// presentAt(), or drop() should be called. If no decision is made, releasing
// the last source-frame copy drops the output through FFmpeg's buffer
// destructor.
class QTAV_HW_MEDIACODEC_EXPORT MediaCodecFrame final {
public:
    MediaCodecFrame() noexcept;
    ~MediaCodecFrame();

    MediaCodecFrame(MediaCodecFrame&&) noexcept;
    MediaCodecFrame& operator=(MediaCodecFrame&&) noexcept;
    MediaCodecFrame(const MediaCodecFrame&) = delete;
    MediaCodecFrame& operator=(const MediaCodecFrame&) = delete;

    explicit operator bool() const noexcept;
    bool isValid() const noexcept;
    bool isPending() const noexcept;
    std::uint32_t surfaceGeneration() const noexcept;
    std::int64_t timestamp() const noexcept;
    const HardwareFrame& sourceFrame() const noexcept;

    // Releases immediately to the codec's configured Surface.
    bool present() noexcept;
    MediaCodecFrameDecisionResult presentResult() noexcept;
    // Releases for presentation at a CLOCK_MONOTONIC timestamp in
    // nanoseconds. FFmpeg/Android require this value to be near "now".
    bool presentAt(std::int64_t monotonicNanoseconds) noexcept;
    MediaCodecFrameDecisionResult presentAtResult(
        std::int64_t monotonicNanoseconds) noexcept;
    // Releases without presenting.
    bool drop() noexcept;
    MediaCodecFrameDecisionResult dropResult() noexcept;

private:
    MediaCodecFrame(
        HardwareFrame source,
        std::uintptr_t buffer,
        std::uint32_t generation,
        std::int64_t timestamp) noexcept;

    HardwareFrame source_;
    std::uintptr_t buffer_ = 0;
    std::uint32_t generation_ = 0;
    std::int64_t timestamp_ = 0;
    bool decided_ = false;

    friend QTAV_HW_MEDIACODEC_EXPORT MediaCodecFrame
    mediaCodecFrame(
        const VideoFrame& frame,
        const MediaCodecSurface& surface) noexcept;
};

} // namespace qtav
