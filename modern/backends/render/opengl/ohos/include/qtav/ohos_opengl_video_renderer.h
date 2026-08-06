// SPDX-License-Identifier: LGPL-2.1-or-later
#pragma once

#if !defined(__OHOS__)
#  error "qtav/ohos_opengl_video_renderer.h is OHOS-only"
#endif

#include <native_window/external_window.h>

#include <memory>

#include <qtav/ohos_opengl_export.h>
#include <qtav/opengl_video_renderer.h>

namespace qtav {

// Owns the OHOS EGL display, OpenGL ES 3.x context, window surface, and
// surface-generation state. ArkUI owns the XComponent; this adapter retains
// only its active OHNativeWindow generation.
class QTAV_RENDER_OPENGL_OHOS_EXPORT OHOSOpenGLVideoRenderer final
    : public VideoRenderAPI {
public:
    explicit OHOSOpenGLVideoRenderer(
        OpenGLOutputPreference outputPreference =
            OpenGLOutputPreference::PreferHdr);
    ~OHOSOpenGLVideoRenderer() override;

    OHOSOpenGLVideoRenderer(OHOSOpenGLVideoRenderer&&) noexcept;
    OHOSOpenGLVideoRenderer& operator=(
        OHOSOpenGLVideoRenderer&&) noexcept;
    OHOSOpenGLVideoRenderer(const OHOSOpenGLVideoRenderer&) = delete;
    OHOSOpenGLVideoRenderer& operator=(
        const OHOSOpenGLVideoRenderer&) = delete;

    VideoRenderCapabilities capabilities() const override;
    void setEventCallback(EventCallback callback) override;
    bool open(const VideoRenderConfig& config) override;
    bool configure(const VideoRenderConfig& config) override;
    VideoRenderAttemptResult renderDetailed(
        const VideoFrame& frame) override;
    bool render(const VideoFrame& frame) override;
    void close() noexcept override;

    // The adapter retains the supplied native window. Republishing the same
    // pointer after an XComponent size change recreates the EGL surface;
    // passing nullptr invalidates the current surface generation.
    bool setWindow(OHNativeWindow* window);
    VideoSize surfaceSize() const noexcept;
    std::uint64_t surfaceGeneration() const noexcept;
    OpenGLOutputColorSpace outputColorSpace() const noexcept;
    bool hdrOutputActive() const noexcept;
    int colorComponentBits() const noexcept;
    void setHardwareFrameInterop(
        std::shared_ptr<OpenGLHardwareFrameInterop> hardwareInterop);
    std::shared_ptr<OpenGLHardwareFrameInterop>
    hardwareFrameInterop() const noexcept;

private:
    class Impl;
    std::unique_ptr<Impl> impl_;
};

} // namespace qtav
