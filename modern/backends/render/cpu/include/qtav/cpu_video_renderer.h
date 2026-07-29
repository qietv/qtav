// SPDX-License-Identifier: LGPL-2.1-or-later
#pragma once

#include <cstdint>
#include <memory>

#include <qtav/cpu_export.h>
#include <qtav/video_render_api.h>

namespace qtav {

struct QTAV_RENDER_CPU_EXPORT CpuImageBuffer {
    std::uint8_t* data = nullptr;
    int width = 0;
    int height = 0;
    int lineSize = 0;
    PixelFormat format = PixelFormat::Unknown;

    bool isValid() const noexcept;
};

QTAV_RENDER_CPU_EXPORT int cpuImageBytesPerPixel(
    PixelFormat format) noexcept;

class QTAV_RENDER_CPU_EXPORT CpuVideoRenderer final : public VideoRenderAPI {
public:
    CpuVideoRenderer();
    ~CpuVideoRenderer() override;

    CpuVideoRenderer(CpuVideoRenderer&&) noexcept;
    CpuVideoRenderer& operator=(CpuVideoRenderer&&) noexcept;
    CpuVideoRenderer(const CpuVideoRenderer&) = delete;
    CpuVideoRenderer& operator=(const CpuVideoRenderer&) = delete;

    VideoRenderCapabilities capabilities() const override;
    void setEventCallback(EventCallback callback) override;
    bool open(const VideoRenderConfig& config) override;
    bool configure(const VideoRenderConfig& config) override;
    bool render(const VideoFrame& frame) override;
    void close() noexcept override;

    bool setTarget(CpuImageBuffer target);
    CpuImageBuffer target() const;

private:
    class Impl;
    std::unique_ptr<Impl> impl_;
};

} // namespace qtav
