// SPDX-License-Identifier: LGPL-2.1-or-later

#include <qtav/cpu_video_renderer.h>

#include <array>
#include <limits>
#include <mutex>
#include <string>
#include <utility>

extern "C" {
#include <libavutil/pixfmt.h>
#include <libswscale/swscale.h>
}

namespace qtav {
namespace {

AVPixelFormat outputFormat(PixelFormat format) noexcept
{
    switch (format) {
    case PixelFormat::RGB24:
        return AV_PIX_FMT_RGB24;
    case PixelFormat::BGR24:
        return AV_PIX_FMT_BGR24;
    case PixelFormat::RGBA:
        return AV_PIX_FMT_RGBA;
    case PixelFormat::BGRA:
        return AV_PIX_FMT_BGRA;
    case PixelFormat::ARGB:
        return AV_PIX_FMT_ARGB;
    case PixelFormat::Gray8:
        return AV_PIX_FMT_GRAY8;
    default:
        return AV_PIX_FMT_NONE;
    }
}

bool isEmpty(const CpuImageBuffer& target) noexcept
{
    return !target.data && target.width == 0 && target.height == 0
        && target.lineSize == 0 && target.format == PixelFormat::Unknown;
}

bool isFullViewport(
    const VideoViewport& viewport,
    const VideoSize& surface) noexcept
{
    return !viewport.isValid()
        || (viewport.x == 0 && viewport.y == 0
            && viewport.width == surface.width
            && viewport.height == surface.height);
}

bool isSupportedConfig(const VideoRenderConfig& config) noexcept
{
    return config.surfaceSize.isValid()
        && isFullViewport(config.viewport, config.surfaceSize)
        && config.aspectRatio == VideoAspectRatioMode::Stretch
        && config.rotation == VideoRotation::Rotate0
        && config.deviceOwnership == NativeResourceOwnership::Borrowed
        && config.contextOwnership == NativeResourceOwnership::Borrowed
        && config.surfaceOwnership == NativeResourceOwnership::Borrowed;
}

bool matches(
    const CpuImageBuffer& target,
    const VideoSize& surface) noexcept
{
    return !target.isValid()
        || (target.width == surface.width && target.height == surface.height);
}

} // namespace

int cpuImageBytesPerPixel(PixelFormat format) noexcept
{
    switch (format) {
    case PixelFormat::RGB24:
    case PixelFormat::BGR24:
        return 3;
    case PixelFormat::RGBA:
    case PixelFormat::BGRA:
    case PixelFormat::ARGB:
        return 4;
    case PixelFormat::Gray8:
        return 1;
    default:
        return 0;
    }
}

bool CpuImageBuffer::isValid() const noexcept
{
    const int bytesPerPixel = cpuImageBytesPerPixel(format);
    return data && width > 0 && height > 0 && bytesPerPixel > 0
        && width <= std::numeric_limits<int>::max() / bytesPerPixel
        && lineSize >= width * bytesPerPixel;
}

class CpuVideoRenderer::Impl {
public:
    ~Impl()
    {
        sws_freeContext(context_);
    }

    void notifyError(std::string detail)
    {
        EventCallback callback;
        {
            std::lock_guard<std::mutex> lock(mutex_);
            callback = callback_;
        }
        if (callback) {
            callback({ VideoRenderEventType::Error, std::move(detail) });
        }
    }

    mutable std::mutex mutex_;
    EventCallback callback_;
    VideoRenderConfig config_;
    CpuImageBuffer target_;
    SwsContext* context_ = nullptr;
    bool open_ = false;
};

CpuVideoRenderer::CpuVideoRenderer()
    : impl_(std::make_unique<Impl>())
{
}

CpuVideoRenderer::~CpuVideoRenderer() = default;
CpuVideoRenderer::CpuVideoRenderer(CpuVideoRenderer&&) noexcept = default;
CpuVideoRenderer& CpuVideoRenderer::operator=(CpuVideoRenderer&&) noexcept =
    default;

VideoRenderCapabilities CpuVideoRenderer::capabilities() const
{
    VideoRenderCapabilities result;
    result.softwareFormats = {
        PixelFormat::YUV420P,
        PixelFormat::YUV422P,
        PixelFormat::YUV444P,
        PixelFormat::NV12,
        PixelFormat::NV21,
        PixelFormat::P010,
        PixelFormat::RGB24,
        PixelFormat::BGR24,
        PixelFormat::RGBA,
        PixelFormat::BGRA,
        PixelFormat::ARGB,
        PixelFormat::Gray8,
        PixelFormat::Native,
    };
    result.aspectRatioModes = { VideoAspectRatioMode::Stretch };
    return result;
}

void CpuVideoRenderer::setEventCallback(EventCallback callback)
{
    if (!impl_) {
        return;
    }
    std::lock_guard<std::mutex> lock(impl_->mutex_);
    impl_->callback_ = std::move(callback);
}

bool CpuVideoRenderer::open(const VideoRenderConfig& config)
{
    if (!impl_) {
        return false;
    }
    if (!isSupportedConfig(config)) {
        impl_->notifyError(
            "The CPU renderer requires a borrowed full-surface target, "
            "Stretch aspect mode, and Rotate0");
        return false;
    }

    bool targetMatches = false;
    {
        std::lock_guard<std::mutex> lock(impl_->mutex_);
        targetMatches = matches(impl_->target_, config.surfaceSize);
        if (targetMatches) {
            impl_->config_ = config;
            impl_->open_ = true;
        }
    }
    if (!targetMatches) {
        impl_->notifyError(
            "The CPU image target size does not match the configured surface");
    }
    return targetMatches;
}

bool CpuVideoRenderer::configure(const VideoRenderConfig& config)
{
    if (!impl_) {
        return false;
    }
    if (!isSupportedConfig(config)) {
        impl_->notifyError(
            "The CPU renderer requires a borrowed full-surface target, "
            "Stretch aspect mode, and Rotate0");
        return false;
    }

    bool configured = false;
    {
        std::lock_guard<std::mutex> lock(impl_->mutex_);
        configured = impl_->open_
            && matches(impl_->target_, config.surfaceSize);
        if (configured) {
            impl_->config_ = config;
        }
    }
    if (!configured) {
        impl_->notifyError(
            "The CPU renderer is closed or its target size does not match "
            "the configured surface");
    }
    return configured;
}

bool CpuVideoRenderer::render(const VideoFrame& frame)
{
    if (!impl_) {
        return false;
    }

    bool rendered = false;
    std::string error;
    {
        std::lock_guard<std::mutex> lock(impl_->mutex_);
        const CpuImageBuffer target = impl_->target_;
        const AVPixelFormat sourceFormat =
            static_cast<AVPixelFormat>(frame.nativeFormat());
        const AVPixelFormat destinationFormat = outputFormat(target.format);

        if (!impl_->open_) {
            error = "The CPU renderer is not open";
        } else if (!frame || !target.isValid()) {
            error = "The CPU renderer requires a valid frame and image target";
        } else if (!matches(target, impl_->config_.surfaceSize)) {
            error =
                "The CPU image target size does not match the configured surface";
        } else if (sourceFormat == AV_PIX_FMT_NONE
            || !sws_isSupportedInput(sourceFormat)
            || destinationFormat == AV_PIX_FMT_NONE
            || !sws_isSupportedOutput(destinationFormat)) {
            error = "libswscale does not support the requested pixel conversion";
        } else {
            impl_->context_ = sws_getCachedContext(
                impl_->context_,
                frame.width(),
                frame.height(),
                sourceFormat,
                target.width,
                target.height,
                destinationFormat,
                SWS_BILINEAR,
                nullptr,
                nullptr,
                nullptr);
            if (!impl_->context_) {
                error = "libswscale could not create a conversion context";
            } else {
                std::array<const std::uint8_t*, 4> sourceData {};
                std::array<int, 4> sourceLineSizes {};
                for (int plane = 0;
                     plane < static_cast<int>(sourceData.size());
                     ++plane) {
                    sourceData[static_cast<std::size_t>(plane)] =
                        frame.data(plane);
                    sourceLineSizes[static_cast<std::size_t>(plane)] =
                        frame.lineSize(plane);
                }
                std::array<std::uint8_t*, 4> destinationData {
                    target.data,
                    nullptr,
                    nullptr,
                    nullptr,
                };
                std::array<int, 4> destinationLineSizes {
                    target.lineSize,
                    0,
                    0,
                    0,
                };
                rendered = sws_scale(
                               impl_->context_,
                               sourceData.data(),
                               sourceLineSizes.data(),
                               0,
                               frame.height(),
                               destinationData.data(),
                               destinationLineSizes.data())
                    == target.height;
                if (!rendered) {
                    error = "libswscale did not produce the complete image";
                }
            }
        }
    }

    if (!rendered) {
        impl_->notifyError(std::move(error));
    }
    return rendered;
}

void CpuVideoRenderer::close() noexcept
{
    if (!impl_) {
        return;
    }
    std::lock_guard<std::mutex> lock(impl_->mutex_);
    impl_->open_ = false;
    impl_->target_ = {};
    sws_freeContext(impl_->context_);
    impl_->context_ = nullptr;
}

bool CpuVideoRenderer::setTarget(CpuImageBuffer target)
{
    if (!impl_) {
        return false;
    }
    if (!isEmpty(target) && !target.isValid()) {
        return false;
    }

    std::lock_guard<std::mutex> lock(impl_->mutex_);
    if (impl_->open_ && target.isValid()
        && !matches(target, impl_->config_.surfaceSize)) {
        return false;
    }
    impl_->target_ = target;
    return true;
}

CpuImageBuffer CpuVideoRenderer::target() const
{
    if (!impl_) {
        return {};
    }
    std::lock_guard<std::mutex> lock(impl_->mutex_);
    return impl_->target_;
}

} // namespace qtav
