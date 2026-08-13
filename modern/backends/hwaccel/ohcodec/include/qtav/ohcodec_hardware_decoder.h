// SPDX-License-Identifier: LGPL-2.1-or-later
#pragma once

#if !defined(__OHOS__)
#  error "qtav/ohcodec_hardware_decoder.h is OHOS-only"
#endif

#include <native_window/external_window.h>

#include <cstdint>
#include <memory>

#include <qtav/frame.h>
#include <qtav/hardware_decoder.h>
#include <qtav/hw_ohcodec_export.h>

namespace qtav {

// Reference-counted versioned direct-output surface token. Construction
// retains the OHNativeWindow; copies share that reference and generation.
// Construct a new token whenever ArkUI publishes a replacement window.
class QTAV_HW_OHCODEC_EXPORT OHCodecSurface final {
public:
    OHCodecSurface() noexcept;
    explicit OHCodecSurface(OHNativeWindow* window);
    ~OHCodecSurface();

    OHCodecSurface(const OHCodecSurface&) noexcept;
    OHCodecSurface(OHCodecSurface&&) noexcept;
    OHCodecSurface& operator=(const OHCodecSurface&) noexcept;
    OHCodecSurface& operator=(OHCodecSurface&&) noexcept;

    explicit operator bool() const noexcept;
    bool isValid() const noexcept;
    OHNativeWindow* nativeWindow() const noexcept;
    std::uint32_t generation() const noexcept;

private:
    class Impl;
    std::shared_ptr<const Impl> impl_;
};

struct QTAV_HW_OHCODEC_EXPORT OHCodecHardwareDecodeOptions {
    // If wrapper selection, codec configuration, or surface setup fails,
    // reopen the same stream with its ordinary FFmpeg software decoder.
    bool allowSoftwareFallback = true;
    // Bounds extra decoder outputs retained by QtAVCore and application
    // callbacks. The core clamps the value to its supported range.
    int extraHardwareFrames = 4;
};

// Selects FFmpeg's explicit "*_ohcodec" wrapper decoder and binds it to the
// supplied OHNativeWindow generation. An invalid surface returns a
// required-but-empty device token so Player's fallback policy remains
// observable and deterministic.
QTAV_HW_OHCODEC_EXPORT HardwareDecodeConfig
ohCodecHardwareDecodeConfig(
    const OHCodecSurface& surface,
    OHCodecHardwareDecodeOptions options = {});

class OHCodecFrame;

// Result of the one permitted decision for an OHCodec surface output.
// AlreadyDecided means another view of the same underlying VideoFrame made
// the decision first; no new surface buffer will be produced.
enum class OHCodecFrameDecisionResult {
    Applied,
    AlreadyDecided,
    Failed,
};

// Validates that frame is a pending OHCodec direct-surface output for the
// exact supplied window generation. Stale or foreign surface outputs return
// an empty value.
QTAV_HW_OHCODEC_EXPORT OHCodecFrame
ohCodecFrame(
    const VideoFrame& frame,
    const OHCodecSurface& surface) noexcept;

// Move-only decision token for one FFmpeg OHCodec output buffer. The source
// VideoFrame keeps the native output and decoder alive. Exactly one of
// present(), presentAt(), or drop() may be called. Destroying an undecided
// token drops the output.
class QTAV_HW_OHCODEC_EXPORT OHCodecFrame final {
public:
    OHCodecFrame() noexcept;
    ~OHCodecFrame();

    OHCodecFrame(OHCodecFrame&& other) noexcept;
    OHCodecFrame& operator=(OHCodecFrame&& other) noexcept;
    OHCodecFrame(const OHCodecFrame&) = delete;
    OHCodecFrame& operator=(const OHCodecFrame&) = delete;

    explicit operator bool() const noexcept;
    bool isValid() const noexcept;
    bool isPending() const noexcept;
    std::uint32_t surfaceGeneration() const noexcept;
    std::int64_t timestamp() const noexcept;
    const HardwareFrame& sourceFrame() const noexcept;

    // Releases immediately to the codec's configured surface.
    bool present() noexcept;
    OHCodecFrameDecisionResult presentResult() noexcept;
    // Releases for presentation at a CLOCK_MONOTONIC timestamp in
    // nanoseconds. The timestamp must be close to the current time.
    bool presentAt(std::int64_t monotonicNanoseconds) noexcept;
    OHCodecFrameDecisionResult presentAtResult(
        std::int64_t monotonicNanoseconds) noexcept;
    // Releases without presenting.
    bool drop() noexcept;
    OHCodecFrameDecisionResult dropResult() noexcept;

private:
    OHCodecFrame(
        HardwareFrame source,
        std::uintptr_t buffer,
        std::uint32_t generation,
        std::int64_t timestamp) noexcept;

    void abandon() noexcept;

    HardwareFrame source_;
    std::uintptr_t buffer_ = 0;
    std::uint32_t generation_ = 0;
    std::int64_t timestamp_ = 0;
    bool decided_ = false;

    friend QTAV_HW_OHCODEC_EXPORT OHCodecFrame
    ohCodecFrame(
        const VideoFrame& frame,
        const OHCodecSurface& surface) noexcept;
};

} // namespace qtav
