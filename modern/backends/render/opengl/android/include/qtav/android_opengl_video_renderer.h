// SPDX-License-Identifier: LGPL-2.1-or-later
#pragma once

#if !defined(__ANDROID__)
#  error "qtav/android_opengl_video_renderer.h is Android-only"
#endif

#include <android/native_window.h>

#include <memory>

#include <qtav/android_opengl_export.h>
#include <qtav/opengl_video_renderer.h>

namespace qtav {

// Owns the Android EGL display, OpenGL ES 3.x context, window surface, and
// surface-generation state. The NativeActivity remains application-owned;
// the adapter retains only its active ANativeWindow generation.
class QTAV_RENDER_OPENGL_ANDROID_EXPORT
AndroidOpenGLVideoRenderer final : public VideoRenderAPI {
public:
    explicit AndroidOpenGLVideoRenderer(
        OpenGLOutputPreference outputPreference =
            OpenGLOutputPreference::PreferHdr);
    ~AndroidOpenGLVideoRenderer() override;

    AndroidOpenGLVideoRenderer(AndroidOpenGLVideoRenderer&&) noexcept;
    AndroidOpenGLVideoRenderer& operator=(
        AndroidOpenGLVideoRenderer&&) noexcept;
    AndroidOpenGLVideoRenderer(
        const AndroidOpenGLVideoRenderer&) = delete;
    AndroidOpenGLVideoRenderer& operator=(
        const AndroidOpenGLVideoRenderer&) = delete;

    VideoRenderCapabilities capabilities() const override;
    void setEventCallback(EventCallback callback) override;
    bool open(const VideoRenderConfig& config) override;
    bool configure(const VideoRenderConfig& config) override;
    bool render(const VideoFrame& frame) override;
    void close() noexcept override;

    // The adapter acquires its own window reference. Replacing or removing the
    // window invalidates the old EGL surface generation.
    bool setWindow(ANativeWindow* window);
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
