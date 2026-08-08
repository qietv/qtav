// SPDX-License-Identifier: LGPL-2.1-or-later
#pragma once

#include <cstdint>
#include <memory>
#include <string>

#include <qtav/frame.h>

struct AVFrame;

namespace qtav::detail {

struct QTAV_CORE_EXPORT FrameFactory {
    static VideoFrame video(
        const AVFrame* frame,
        std::int64_t timestampMs,
        std::int64_t durationMs,
        HardwareDeviceType hardwareDeviceType =
            HardwareDeviceType::Unknown,
        std::uintptr_t hardwareNativeIdentity = 0,
        std::uint32_t hardwareSurfaceGeneration = 0,
        std::shared_ptr<void> decoderLifetime = {});
    static AudioFrame audio(
        const AVFrame* frame,
        std::int64_t timestampMs,
        std::int64_t durationMs);
    static SubtitleFrame subtitle(
        std::string text,
        std::int64_t timestampMs,
        std::int64_t durationMs,
        bool forced);
    static VideoFrame hardware(
        HardwareFrame frame,
        std::int64_t timestampMs = 0,
        std::int64_t durationMs = 0);
    // Internal backend bridge. The returned reference is owned by frame and
    // remains valid only while a copy of frame remains alive.
    static const AVFrame* nativeVideoFrame(
        const VideoFrame& frame) noexcept;
};

} // namespace qtav::detail
