// SPDX-License-Identifier: LGPL-2.1-or-later
#pragma once

#include <cstdint>
#include <functional>
#include <memory>

#include <qtav/opengl_export.h>
#include <qtav/video_render_api.h>

namespace qtav {

// PreferHdr selects native HDR when the platform adapter exposes it and falls
// back to SDR. RequireHdr rejects an SDR-only surface. SdrOnly always requests
// the deterministic HDR-to-SDR rendering path.
enum class OpenGLOutputPreference {
    PreferHdr,
    RequireHdr,
    SdrOnly,
};

// Defines the encoding written by OpenGLVideoRenderer. Platform adapters must
// ensure that the framebuffer and presentation surface use the same color
// space. HDR10PQ and HDR10HLG contain BT.2020 primaries with the named
// transfer function; SdrSrgb contains BT.709/sRGB output.
enum class OpenGLOutputColorSpace {
    SdrSrgb,
    HDR10PQ,
    HDR10HLG,
};

// OpenGLVideoRenderer renders into the application-provided framebuffer of
// the OpenGL ES 3.x context that is current on the calling thread. Framebuffer
// zero is valid and represents the current default framebuffer.
struct QTAV_RENDER_OPENGL_EXPORT OpenGLRenderTarget {
    std::uint32_t framebuffer = 0;
    VideoSize size;
    std::uint64_t generation = 0;
    OpenGLOutputColorSpace colorSpace =
        OpenGLOutputColorSpace::SdrSrgb;

    bool isValid() const noexcept
    {
        return size.isValid();
    }

    bool isHdr() const noexcept;
};

QTAV_RENDER_OPENGL_EXPORT bool openGLColorSpaceIsHdr(
    OpenGLOutputColorSpace colorSpace) noexcept;

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
