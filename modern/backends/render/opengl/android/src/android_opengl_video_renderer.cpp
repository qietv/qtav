// SPDX-License-Identifier: LGPL-2.1-or-later

#include <qtav/android_opengl_video_renderer.h>

#include <EGL/egl.h>

#include <cstdint>
#include <mutex>
#include <string>
#include <utility>

namespace qtav {
namespace {

const char* eglErrorName(EGLint error) noexcept
{
    switch (error) {
    case EGL_SUCCESS:
        return "EGL_SUCCESS";
    case EGL_NOT_INITIALIZED:
        return "EGL_NOT_INITIALIZED";
    case EGL_BAD_ACCESS:
        return "EGL_BAD_ACCESS";
    case EGL_BAD_ALLOC:
        return "EGL_BAD_ALLOC";
    case EGL_BAD_ATTRIBUTE:
        return "EGL_BAD_ATTRIBUTE";
    case EGL_BAD_CONTEXT:
        return "EGL_BAD_CONTEXT";
    case EGL_BAD_CONFIG:
        return "EGL_BAD_CONFIG";
    case EGL_BAD_CURRENT_SURFACE:
        return "EGL_BAD_CURRENT_SURFACE";
    case EGL_BAD_DISPLAY:
        return "EGL_BAD_DISPLAY";
    case EGL_BAD_SURFACE:
        return "EGL_BAD_SURFACE";
    case EGL_BAD_MATCH:
        return "EGL_BAD_MATCH";
    case EGL_BAD_PARAMETER:
        return "EGL_BAD_PARAMETER";
    case EGL_BAD_NATIVE_PIXMAP:
        return "EGL_BAD_NATIVE_PIXMAP";
    case EGL_BAD_NATIVE_WINDOW:
        return "EGL_BAD_NATIVE_WINDOW";
    case EGL_CONTEXT_LOST:
        return "EGL_CONTEXT_LOST";
    default:
        return "unknown EGL error";
    }
}

std::string eglError(const char* operation)
{
    const EGLint code = eglGetError();
    return std::string(operation) + " failed: " + eglErrorName(code)
        + " (" + std::to_string(code) + ')';
}

bool validConfig(const VideoRenderConfig& config) noexcept
{
    if (!config.surfaceSize.isValid()) {
        return false;
    }
    const VideoViewport& viewport = config.viewport;
    return !viewport.isValid()
        || (viewport.x >= 0 && viewport.y >= 0
            && viewport.x <= config.surfaceSize.width
            && viewport.y <= config.surfaceSize.height
            && viewport.width
                <= config.surfaceSize.width - viewport.x
            && viewport.height
                <= config.surfaceSize.height - viewport.y);
}

} // namespace

class AndroidOpenGLVideoRenderer::Impl {
public:
    Impl()
        : renderer_([this] {
              return OpenGLRenderTarget {
                  0,
                  surfaceSize_,
                  generation_,
              };
          })
    {
        renderer_.setEventCallback(
            [this](const VideoRenderEvent& event) {
                if (event.type == VideoRenderEventType::Error) {
                    std::lock_guard<std::recursive_mutex> lock(mutex_);
                    lastEngineError_ = event.detail;
                }
                notify(event.type, event.detail);
            });
    }

    ~Impl()
    {
        close();
    }

    void notify(VideoRenderEventType type, std::string detail)
    {
        EventCallback callback;
        {
            std::lock_guard<std::recursive_mutex> lock(mutex_);
            callback = eventCallback_;
        }
        if (callback) {
            callback({ type, std::move(detail) });
        }
    }

    bool createContext(std::string& error)
    {
        if (display_ != EGL_NO_DISPLAY
            && context_ != EGL_NO_CONTEXT) {
            return true;
        }
        display_ = eglGetDisplay(EGL_DEFAULT_DISPLAY);
        if (display_ == EGL_NO_DISPLAY) {
            error = eglError("eglGetDisplay");
            return false;
        }
        EGLint major = 0;
        EGLint minor = 0;
        if (eglInitialize(display_, &major, &minor) != EGL_TRUE) {
            error = eglError("eglInitialize");
            return false;
        }
        if (eglBindAPI(EGL_OPENGL_ES_API) != EGL_TRUE) {
            error = eglError("eglBindAPI");
            return false;
        }
        const EGLint attributes[] {
            EGL_SURFACE_TYPE, EGL_WINDOW_BIT,
            EGL_RENDERABLE_TYPE, EGL_OPENGL_ES3_BIT,
            EGL_RED_SIZE, 8,
            EGL_GREEN_SIZE, 8,
            EGL_BLUE_SIZE, 8,
            EGL_ALPHA_SIZE, 8,
            EGL_DEPTH_SIZE, 0,
            EGL_STENCIL_SIZE, 0,
            EGL_NONE,
        };
        EGLint count = 0;
        if (eglChooseConfig(
                display_,
                attributes,
                &eglConfig_,
                1,
                &count) != EGL_TRUE
            || count != 1) {
            error =
                "No Android EGLConfig supports an RGBA8 OpenGL ES 3 window";
            return false;
        }
        const EGLint contextAttributes[] {
            EGL_CONTEXT_CLIENT_VERSION, 3,
            EGL_NONE,
        };
        context_ = eglCreateContext(
            display_,
            eglConfig_,
            EGL_NO_CONTEXT,
            contextAttributes);
        if (context_ == EGL_NO_CONTEXT) {
            error = eglError("eglCreateContext");
            return false;
        }
        return true;
    }

    bool createSurface(std::string& error)
    {
        if (surface_ != EGL_NO_SURFACE) {
            return true;
        }
        if (!window_) {
            error = "The Android native window is unavailable";
            return false;
        }
        if (!createContext(error)) {
            return false;
        }
        surface_ = eglCreateWindowSurface(
            display_,
            eglConfig_,
            window_,
            nullptr);
        if (surface_ == EGL_NO_SURFACE) {
            error = eglError("eglCreateWindowSurface");
            return false;
        }
        EGLint width = 0;
        EGLint height = 0;
        if (eglQuerySurface(
                display_, surface_, EGL_WIDTH, &width) != EGL_TRUE
            || eglQuerySurface(
                display_, surface_, EGL_HEIGHT, &height) != EGL_TRUE
            || width <= 0 || height <= 0) {
            error = eglError("eglQuerySurface");
            destroySurface();
            return false;
        }
        surfaceSize_ = { width, height };
        ++generation_;
        return true;
    }

    void destroySurface() noexcept
    {
        const bool hadSurface = surface_ != EGL_NO_SURFACE
            || surfaceSize_.isValid();
        if (display_ != EGL_NO_DISPLAY
            && surface_ != EGL_NO_SURFACE) {
            if (eglGetCurrentSurface(EGL_DRAW) == surface_) {
                eglMakeCurrent(
                    display_,
                    EGL_NO_SURFACE,
                    EGL_NO_SURFACE,
                    EGL_NO_CONTEXT);
            }
            eglDestroySurface(display_, surface_);
        }
        surface_ = EGL_NO_SURFACE;
        surfaceSize_ = {};
        if (hadSurface) {
            ++generation_;
        }
    }

    bool makeCurrent(std::string& error)
    {
        if (display_ == EGL_NO_DISPLAY
            || context_ == EGL_NO_CONTEXT
            || surface_ == EGL_NO_SURFACE) {
            error =
                "The Android EGL context or window surface is unavailable";
            return false;
        }
        if (eglMakeCurrent(
                display_,
                surface_,
                surface_,
                context_) != EGL_TRUE) {
            error = eglError("eglMakeCurrent");
            return false;
        }
        return true;
    }

    void doneCurrent() noexcept
    {
        if (display_ != EGL_NO_DISPLAY) {
            eglMakeCurrent(
                display_,
                EGL_NO_SURFACE,
                EGL_NO_SURFACE,
                EGL_NO_CONTEXT);
        }
    }

    bool openEngine(
        const VideoRenderConfig& requested,
        std::string& error)
    {
        renderConfig_ = requested;
        renderConfig_.surfaceSize = surfaceSize_;
        renderConfig_.deviceOwnership = NativeResourceOwnership::Borrowed;
        renderConfig_.contextOwnership = NativeResourceOwnership::Borrowed;
        renderConfig_.surfaceOwnership = NativeResourceOwnership::Borrowed;
        if (!validConfig(renderConfig_)) {
            error =
                "The Android OpenGL ES surface configuration is invalid";
            return false;
        }
        if (!makeCurrent(error)) {
            return false;
        }
        lastEngineError_.clear();
        const bool succeeded = engineOpen_
            ? renderer_.configure(renderConfig_)
            : renderer_.open(renderConfig_);
        engineOpen_ = succeeded;
        doneCurrent();
        if (!succeeded && error.empty()) {
            error = lastEngineError_.empty()
                ? "The OpenGL ES engine could not open"
                : lastEngineError_;
        }
        return succeeded;
    }

    void destroyContext() noexcept
    {
        if (display_ != EGL_NO_DISPLAY
            && context_ != EGL_NO_CONTEXT) {
            eglDestroyContext(display_, context_);
        }
        context_ = EGL_NO_CONTEXT;
        renderConfig_ = {};
        if (display_ != EGL_NO_DISPLAY) {
            eglTerminate(display_);
        }
        display_ = EGL_NO_DISPLAY;
        eglConfig_ = nullptr;
    }

    void close() noexcept
    {
        std::lock_guard<std::recursive_mutex> lock(mutex_);
        std::string ignored;
        if (engineOpen_ && makeCurrent(ignored)) {
            renderer_.close();
            doneCurrent();
        }
        engineOpen_ = false;
        open_ = false;
        destroySurface();
        destroyContext();
        if (window_) {
            ANativeWindow_release(window_);
            window_ = nullptr;
        }
    }

    mutable std::recursive_mutex mutex_;
    EventCallback eventCallback_;
    EGLDisplay display_ = EGL_NO_DISPLAY;
    EGLConfig eglConfig_ = nullptr;
    EGLContext context_ = EGL_NO_CONTEXT;
    EGLSurface surface_ = EGL_NO_SURFACE;
    ANativeWindow* window_ = nullptr;
    VideoSize surfaceSize_;
    std::uint64_t generation_ = 0;
    VideoRenderConfig renderConfig_;
    bool open_ = false;
    bool engineOpen_ = false;
    std::string lastEngineError_;
    OpenGLVideoRenderer renderer_;
};

AndroidOpenGLVideoRenderer::AndroidOpenGLVideoRenderer()
    : impl_(std::make_unique<Impl>())
{
}

AndroidOpenGLVideoRenderer::~AndroidOpenGLVideoRenderer() = default;
AndroidOpenGLVideoRenderer::AndroidOpenGLVideoRenderer(
    AndroidOpenGLVideoRenderer&&) noexcept = default;
AndroidOpenGLVideoRenderer&
AndroidOpenGLVideoRenderer::operator=(
    AndroidOpenGLVideoRenderer&&) noexcept = default;

VideoRenderCapabilities
AndroidOpenGLVideoRenderer::capabilities() const
{
    if (!impl_) {
        return {};
    }
    VideoRenderCapabilities result =
        impl_->renderer_.capabilities();
    result.ownedContext = true;
    result.ownedSurface = true;
    return result;
}

void AndroidOpenGLVideoRenderer::setEventCallback(
    EventCallback callback)
{
    if (!impl_) {
        return;
    }
    std::lock_guard<std::recursive_mutex> lock(impl_->mutex_);
    impl_->eventCallback_ = std::move(callback);
}

bool AndroidOpenGLVideoRenderer::open(
    const VideoRenderConfig& config)
{
    if (!impl_) {
        return false;
    }
    std::string error;
    bool opened = false;
    {
        std::lock_guard<std::recursive_mutex> lock(impl_->mutex_);
        if (!impl_->window_) {
            error = "The Android native window is unavailable";
        } else if (!impl_->createSurface(error)) {
            opened = false;
        } else if (impl_->openEngine(config, error)) {
            impl_->open_ = true;
            opened = true;
        }
    }
    if (!opened) {
        impl_->notify(VideoRenderEventType::Error, std::move(error));
    }
    return opened;
}

bool AndroidOpenGLVideoRenderer::configure(
    const VideoRenderConfig& config)
{
    if (!impl_) {
        return false;
    }
    std::string error;
    bool configured = false;
    {
        std::lock_guard<std::recursive_mutex> lock(impl_->mutex_);
        configured = impl_->open_
            && impl_->surface_ != EGL_NO_SURFACE
            && impl_->openEngine(config, error);
    }
    if (!configured && error.empty()) {
        error =
            "The Android OpenGL ES renderer is closed or has no surface";
    }
    if (!configured) {
        impl_->notify(VideoRenderEventType::Error, std::move(error));
    }
    return configured;
}

bool AndroidOpenGLVideoRenderer::render(
    const VideoFrame& frame)
{
    if (!impl_) {
        return false;
    }
    std::string error;
    bool rendered = false;
    EGLint eglCode = EGL_SUCCESS;
    {
        std::lock_guard<std::recursive_mutex> lock(impl_->mutex_);
        if (!impl_->open_) {
            error = "The Android OpenGL ES renderer is not open";
        } else if (!impl_->makeCurrent(error)) {
            eglCode = eglGetError();
        } else {
            impl_->lastEngineError_.clear();
            if (!impl_->renderer_.render(frame)) {
                error = impl_->lastEngineError_.empty()
                    ? "The OpenGL ES engine could not render the frame"
                    : impl_->lastEngineError_;
            } else if (eglSwapBuffers(
                           impl_->display_,
                           impl_->surface_) != EGL_TRUE) {
                eglCode = eglGetError();
                error = std::string("eglSwapBuffers failed: ")
                    + eglErrorName(eglCode) + " ("
                    + std::to_string(eglCode) + ')';
            } else {
                rendered = true;
            }
        }
        impl_->doneCurrent();
    }
    if (!rendered) {
        const VideoRenderEventType type =
            eglCode == EGL_BAD_SURFACE
                || eglCode == EGL_BAD_NATIVE_WINDOW
                || error.find("surface") != std::string::npos
            ? VideoRenderEventType::SurfaceLost
            : VideoRenderEventType::Error;
        impl_->notify(type, std::move(error));
    }
    return rendered;
}

void AndroidOpenGLVideoRenderer::close() noexcept
{
    if (impl_) {
        impl_->close();
    }
}

bool AndroidOpenGLVideoRenderer::setWindow(
    ANativeWindow* window)
{
    if (!impl_) {
        return false;
    }
    std::string error;
    bool succeeded = false;
    {
        std::lock_guard<std::recursive_mutex> lock(impl_->mutex_);
        if (window == impl_->window_
            && impl_->surface_ != EGL_NO_SURFACE) {
            return window != nullptr;
        }
        impl_->destroySurface();
        if (impl_->window_) {
            ANativeWindow_release(impl_->window_);
        }
        impl_->window_ = window;
        if (impl_->window_) {
            ANativeWindow_acquire(impl_->window_);
            succeeded = impl_->createSurface(error);
            if (succeeded && impl_->open_) {
                VideoRenderConfig replacement = impl_->renderConfig_;
                replacement.viewport = {};
                succeeded = impl_->openEngine(replacement, error);
            }
        }
    }
    if (!succeeded) {
        impl_->notify(
            window ? VideoRenderEventType::Error
                   : VideoRenderEventType::SurfaceLost,
            error.empty()
                ? "The Android native window was removed"
                : std::move(error));
    }
    return succeeded;
}

VideoSize AndroidOpenGLVideoRenderer::surfaceSize() const noexcept
{
    if (!impl_) {
        return {};
    }
    std::lock_guard<std::recursive_mutex> lock(impl_->mutex_);
    return impl_->surfaceSize_;
}

std::uint64_t
AndroidOpenGLVideoRenderer::surfaceGeneration() const noexcept
{
    if (!impl_) {
        return 0;
    }
    std::lock_guard<std::recursive_mutex> lock(impl_->mutex_);
    return impl_->generation_;
}

} // namespace qtav
