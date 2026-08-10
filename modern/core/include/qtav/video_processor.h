// SPDX-License-Identifier: LGPL-2.1-or-later
#pragma once

#include <string>

#include <qtav/export.h>
#include <qtav/frame.h>

namespace qtav {

struct QTAV_CORE_EXPORT VideoProcessorFormat {
    int width = 0;
    int height = 0;
    PixelFormat pixelFormat = PixelFormat::Unknown;
    bool hardwareFrame = false;
    HardwareDeviceType hardwareDevice = HardwareDeviceType::Unknown;

    bool isValid() const noexcept
    {
        return width > 0 && height > 0
            && pixelFormat != PixelFormat::Unknown;
    }
};

QTAV_CORE_EXPORT VideoProcessorFormat videoProcessorFormat(
    const VideoFrame& frame) noexcept;

struct QTAV_CORE_EXPORT VideoProcessorOpenResult {
    bool success = false;
    // An explicit format-level bypass. Player forwards frames unchanged and
    // does not call process(), reset(), or drain() for this format; close() is
    // still called when the format, processor, media, or playback ends.
    bool bypass = false;
    std::string error;
};

struct QTAV_CORE_EXPORT VideoProcessingResult {
    bool success = false;
    // An explicit per-frame bypass. frame may be empty when this is true.
    bool bypass = false;
    // A successful non-bypass result must contain exactly one frame with the
    // same timestamp and duration as the input. Geometry, software pixel
    // format, pixels, and color metadata may change.
    VideoFrame frame;
    std::string error;
};

// Optional synchronous one-input/one-output video transform. Player calls it
// on the video-decode worker only after VideoFrameScheduler declines the exact
// frame, and before ordinary presentation callbacks/rendering. Queued cadence
// conversion and graphics-context effects are deliberately separate future
// contracts; GPU work remains owned by VideoRenderAPI on the graphics thread.
class QTAV_CORE_EXPORT VideoFrameProcessor {
public:
    virtual ~VideoFrameProcessor();

    virtual VideoProcessorOpenResult open(
        const VideoProcessorFormat& format) = 0;
    virtual VideoProcessingResult process(const VideoFrame& frame) = 0;
    // Finalizes a naturally completed segment. This first synchronous
    // contract cannot emit delayed frames; return false if buffered output
    // would be lost instead of silently discarding it.
    virtual bool drain() = 0;
    // Discards state at a timeline discontinuity while retaining the format.
    virtual bool reset() = 0;
    virtual void close() noexcept = 0;
};

} // namespace qtav
