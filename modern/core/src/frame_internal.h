// SPDX-License-Identifier: LGPL-2.1-or-later
#pragma once

#include <cstdint>

#include <qtav/frame.h>

struct AVFrame;

namespace qtav::detail {

struct QTAV_CORE_EXPORT FrameFactory {
    static VideoFrame video(
        const AVFrame* frame,
        std::int64_t timestampMs,
        std::int64_t durationMs,
        HardwareDeviceType hardwareDeviceType =
            HardwareDeviceType::Unknown);
    static AudioFrame audio(
        const AVFrame* frame,
        std::int64_t timestampMs,
        std::int64_t durationMs);
    static VideoFrame hardware(
        HardwareFrame frame,
        std::int64_t timestampMs = 0,
        std::int64_t durationMs = 0);
};

} // namespace qtav::detail
