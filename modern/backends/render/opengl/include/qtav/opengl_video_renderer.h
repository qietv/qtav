// SPDX-License-Identifier: LGPL-2.1-or-later
#pragma once

#include <cstdint>
#include <functional>
#include <memory>

#include <qtav/opengl_export.h>
#include <qtav/video_render_api.h>

namespace qtav {

// OpenGLVideoRenderer renders into the application-provided framebuffer of
// the OpenGL ES 3.x context that is current on the calling thread. Framebuffer
// zero is valid and represents the current default framebuffer.
struct QTAV_RENDER_OPENGL_EXPORT OpenGLRenderTarget {
    std::uint32_t framebuffer = 0;
    VideoSize size;
    std::uint64_t generation = 0;

    bool isValid() const noexcept
    {
        return size.isValid();
    }
};

using OpenGLCurrentTargetCallback =
    std::function<OpenGLRenderTarget()>;

class QTAV_RENDER_OPENGL_EXPORT OpenGLVideoRenderer final
    : public VideoRenderAPI {
public:
    explicit OpenGLVideoRenderer(
        OpenGLCurrentTargetCallback currentTarget);
    ~OpenGLVideoRenderer() override;

    OpenGLVideoRenderer(OpenGLVideoRenderer&&) noexcept;
    OpenGLVideoRenderer& operator=(OpenGLVideoRenderer&&) noexcept;
    OpenGLVideoRenderer(const OpenGLVideoRenderer&) = delete;
    OpenGLVideoRenderer& operator=(const OpenGLVideoRenderer&) = delete;

    VideoRenderCapabilities capabilities() const override;
    void setEventCallback(EventCallback callback) override;
    bool open(const VideoRenderConfig& config) override;
    bool configure(const VideoRenderConfig& config) override;
    bool render(const VideoFrame& frame) override;
    void close() noexcept override;

    void setCurrentTargetCallback(
        OpenGLCurrentTargetCallback callback);

private:
    class Impl;
    std::unique_ptr<Impl> impl_;
};

} // namespace qtav
