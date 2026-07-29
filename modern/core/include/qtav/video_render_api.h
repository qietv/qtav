// SPDX-License-Identifier: LGPL-2.1-or-later
#pragma once

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
};

} // namespace qtav
