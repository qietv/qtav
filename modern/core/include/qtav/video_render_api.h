// SPDX-License-Identifier: LGPL-2.1-or-later
#pragma once

#include <cstdint>
#include <functional>
#include <string>
#include <vector>

#include <qtav/export.h>
#include <qtav/frame.h>
#include <qtav/hardware_frame.h>

namespace qtav {

struct QTAV_CORE_EXPORT VideoSize {
    int width = 0;
    int height = 0;

    bool isValid() const noexcept
    {
        return width > 0 && height > 0;
    }
};

struct QTAV_CORE_EXPORT VideoViewport {
    int x = 0;
    int y = 0;
    int width = 0;
    int height = 0;

    bool isValid() const noexcept
    {
        return width > 0 && height > 0;
    }
};

enum class VideoAspectRatioMode {
    Fit,
    Fill,
    Stretch,
};

enum class VideoRotation {
    Rotate0,
    Rotate90,
    Rotate180,
    Rotate270,
};

enum class NativeResourceOwnership {
    Borrowed,
    Owned,
};

struct QTAV_CORE_EXPORT VideoRenderConfig {
    VideoSize surfaceSize;
    VideoViewport viewport;
    VideoAspectRatioMode aspectRatio = VideoAspectRatioMode::Fit;
    VideoRotation rotation = VideoRotation::Rotate0;
    NativeResourceOwnership deviceOwnership =
        NativeResourceOwnership::Borrowed;
    NativeResourceOwnership contextOwnership =
        NativeResourceOwnership::Borrowed;
    NativeResourceOwnership surfaceOwnership =
        NativeResourceOwnership::Borrowed;
};

struct QTAV_CORE_EXPORT VideoRenderCapabilities {
    std::vector<PixelFormat> softwareFormats;
    std::vector<HardwareDeviceType> hardwareDevices;
    bool customViewport = false;
    bool rotation = false;
    bool ownedDevice = false;
    bool ownedContext = false;
    bool ownedSurface = false;
    std::vector<VideoAspectRatioMode> aspectRatioModes {
        VideoAspectRatioMode::Fit,
        VideoAspectRatioMode::Fill,
        VideoAspectRatioMode::Stretch,
    };
};

enum class VideoRenderEventType {
    RedrawRequested,
    SurfaceLost,
    Error,
};

struct QTAV_CORE_EXPORT VideoRenderEvent {
    VideoRenderEventType type = VideoRenderEventType::RedrawRequested;
    std::string detail;
};

// Result of one backend render attempt. This is deliberately independent of
// Player's frame sequence and presentation generation: adapters classify only
// what happened to the supplied frame and Player adds snapshot identity.
enum class VideoRenderAttemptStatus {
    // The frame was submitted to the current presentation target.
    Presented,
    // The backend is waiting for an asynchronous producer or GPU resource and
    // will emit RedrawRequested when the same retained frame can be retried.
    DeferredUntilRedraw,
    // Transient pressure requires a bounded timer retry. A zero delay leaves
    // the concrete backoff policy to the application.
    RetryAfterBackoff,
    // This frame is terminally stale, superseded, or deliberately consumed by
    // a non-renderer route. It must not be retried.
    Discarded,
    // The current native surface generation is invalid and must be recreated.
    SurfaceLost,
    // The active renderer cannot continue without replacement or fallback.
    FatalError,
};

struct QTAV_CORE_EXPORT VideoRenderAttemptResult {
    VideoRenderAttemptStatus status =
        VideoRenderAttemptStatus::RetryAfterBackoff;
    std::uint32_t retryAfterMilliseconds = 0;
    std::string detail;

    bool presented() const noexcept
    {
        return status == VideoRenderAttemptStatus::Presented;
    }

    bool frameConsumed() const noexcept
    {
        return presented()
            || status == VideoRenderAttemptStatus::Discarded;
    }

    bool retryable() const noexcept
    {
        return status == VideoRenderAttemptStatus::DeferredUntilRedraw
            || status == VideoRenderAttemptStatus::RetryAfterBackoff;
    }
};

class QTAV_CORE_EXPORT VideoRenderAPI {
public:
    using EventCallback = std::function<void(const VideoRenderEvent&)>;

    virtual ~VideoRenderAPI();

    virtual VideoRenderCapabilities capabilities() const = 0;
    virtual void setEventCallback(EventCallback callback) = 0;
    virtual bool open(const VideoRenderConfig& config) = 0;
    virtual bool configure(const VideoRenderConfig& config) = 0;
    virtual bool render(const VideoFrame& frame) = 0;
    virtual void close() noexcept = 0;
    // New backends should override this method and classify every attempt.
    // The default preserves source compatibility for existing bool renderers
    // by treating false as a short timer-backoff request. It is appended after
    // the original virtual surface so existing method slots stay stable while
    // the 2.x API is still being integrated.
    virtual VideoRenderAttemptResult renderDetailed(
        const VideoFrame& frame);
};

} // namespace qtav
