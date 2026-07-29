// SPDX-License-Identifier: LGPL-2.1-or-later
#pragma once

#include <cstdint>

#include <qtav/frame.h>

struct AVFrame;

namespace qtav::detail {

struct FrameFactory {
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
};

} // namespace qtav::detail
