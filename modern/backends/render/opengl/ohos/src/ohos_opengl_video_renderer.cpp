// SPDX-License-Identifier: LGPL-2.1-or-later

#include <qtav/ohos_opengl_video_renderer.h>

#include <EGL/egl.h>
#include <EGL/eglext.h>
#include <native_buffer/native_buffer.h>

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <mutex>
#include <string>
#include <utility>
#include <vector>

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

std::string eglError(const char* operation, EGLint& code)
{
    code = eglGetError();
    return std::string(operation) + " failed: " + eglErrorName(code)
        + " (" + std::to_string(code) + ')';
}

bool eglLifecycleLoss(EGLint code) noexcept
{
    return code == EGL_BAD_SURFACE
        || code == EGL_BAD_NATIVE_WINDOW
        || code == EGL_BAD_CONTEXT
        || code == EGL_BAD_DISPLAY
        || code == EGL_CONTEXT_LOST;
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

bool hasExtension(
    const char* extensions,
    const char* requested) noexcept
{
    if (!extensions || !requested || !*requested
        || std::strchr(requested, ' ')) {
        return false;
    }
    const std::size_t length = std::strlen(requested);
    const char* current = extensions;
    while ((current = std::strstr(current, requested))) {
        const bool startsToken =
            current == extensions || current[-1] == ' ';
        const bool endsToken =
            current[length] == '\0' || current[length] == ' ';
        if (startsToken && endsToken) {
            return true;
        }
        current += length;
    }
    return false;
}

bool chooseSdrConfig(
    EGLDisplay display,
    EGLConfig& result,
    EGLint& nativeVisual,
    std::string& error,
    EGLint& errorCode)
{
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
            display, attributes, nullptr, 0, &count) != EGL_TRUE
        || count <= 0) {
        error = "No exact RGBA8 OpenGL ES 3 OHOS window EGLConfig is available";
        return false;
    }
    std::vector<EGLConfig> configs(static_cast<std::size_t>(count));
    if (eglChooseConfig(
            display,
            attributes,
            configs.data(),
            count,
            &count) != EGL_TRUE) {
        error = eglError("eglChooseConfig", errorCode);
        return false;
    }
    configs.resize(static_cast<std::size_t>(std::max(count, 0)));
    for (EGLConfig config : configs) {
        EGLint red = 0;
        EGLint green = 0;
        EGLint blue = 0;
        EGLint alpha = 0;
        EGLint visual = 0;
        if (eglGetConfigAttrib(
                display, config, EGL_RED_SIZE, &red) != EGL_TRUE
            || eglGetConfigAttrib(
                   display, config, EGL_GREEN_SIZE, &green) != EGL_TRUE
            || eglGetConfigAttrib(
                   display, config, EGL_BLUE_SIZE, &blue) != EGL_TRUE
            || eglGetConfigAttrib(
                   display, config, EGL_ALPHA_SIZE, &alpha) != EGL_TRUE
            || eglGetConfigAttrib(
                   display,
                   config,
                   EGL_NATIVE_VISUAL_ID,
                   &visual) != EGL_TRUE) {
            continue;
        }
        if (red == 8 && green == 8 && blue == 8 && alpha == 8
            && visual != 0) {
            result = config;
            nativeVisual = visual;
            return true;
        }
    }
    error = "The OHOS EGL display exposes no exact RGBA8 native-window config";
    return false;
}

} // namespace

class OHOSOpenGLVideoRenderer::Impl {
public:
    explicit Impl(OpenGLOutputPreference outputPreference)
        : outputPreference_(outputPreference)
        , renderer_([this] {
              return OpenGLRenderTarget {
                  0,
                  surfaceSize_,
                  generation_,
                  OpenGLOutputColorSpace::SdrSrgb,
              };
          })
    {
        renderer_.setPresentCallback(
            [this](std::string& detail) {
                lastPresentEglError_ = EGL_SUCCESS;
                if (eglSwapBuffers(display_, surface_) == EGL_TRUE) {
                    return true;
                }
                lastPresentEglError_ = eglGetError();
                detail = std::string(
                    "OHOS EGL window presentation failed: ")
                    + eglErrorName(lastPresentEglError_) + " ("
                    + std::to_string(lastPresentEglError_) + ')';
                return false;
            });
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

    bool initializeDisplay(std::string& error)
    {
        if (display_ != EGL_NO_DISPLAY) {
            return true;
        }
        display_ = eglGetDisplay(EGL_DEFAULT_DISPLAY);
        if (display_ == EGL_NO_DISPLAY) {
            error = eglError("eglGetDisplay", lastEglError_);
            return false;
        }
        EGLint major = 0;
        EGLint minor = 0;
        if (eglInitialize(display_, &major, &minor) != EGL_TRUE) {
            error = eglError("eglInitialize", lastEglError_);
            display_ = EGL_NO_DISPLAY;
            return false;
        }
        if (eglBindAPI(EGL_OPENGL_ES_API) != EGL_TRUE) {
            error = eglError("eglBindAPI", lastEglError_);
            eglTerminate(display_);
            display_ = EGL_NO_DISPLAY;
            return false;
        }
        const char* extensions =
            eglQueryString(display_, EGL_EXTENSIONS);
        extensions_ = extensions ? extensions : "";
        return true;
    }

    bool configureNativeWindow(
        EGLint nativeVisual,
        std::string& error)
    {
        if (OH_NativeWindow_NativeWindowHandleOpt(
                window_, SET_FORMAT, nativeVisual)
            != 0) {
            error = "The OHOS native window rejected the RGBA8 EGL visual";
            return false;
        }
        int32_t actualFormat = 0;
        if (OH_NativeWindow_NativeWindowHandleOpt(
                window_, GET_FORMAT, &actualFormat)
                != 0
            || actualFormat != nativeVisual) {
            error = "The OHOS native window did not retain the RGBA8 EGL visual";
            return false;
        }
        if (OH_NativeWindow_NativeWindowHandleOpt(
                window_,
                SET_COLOR_GAMUT,
                static_cast<int32_t>(NATIVEBUFFER_COLOR_GAMUT_SRGB))
            != 0) {
            error = "The OHOS native window rejected the sRGB color gamut";
            return false;
        }
        int32_t actualGamut = 0;
        if (OH_NativeWindow_NativeWindowHandleOpt(
                window_, GET_COLOR_GAMUT, &actualGamut)
                != 0
            || actualGamut
                != static_cast<int32_t>(
                    NATIVEBUFFER_COLOR_GAMUT_SRGB)) {
            error = "The OHOS native window did not retain the sRGB color gamut";
            return false;
        }
        return true;
    }

    bool createWindowSurface(std::string& error)
    {
        if (!configureNativeWindow(nativeVisual_, error)) {
            return false;
        }
        const bool explicitColorSpace = hasExtension(
            extensions_.c_str(), "EGL_KHR_gl_colorspace");
        const EGLint surfaceAttributes[] {
            EGL_GL_COLORSPACE_KHR,
            EGL_GL_COLORSPACE_SRGB_KHR,
            EGL_NONE,
        };
        surface_ = eglCreateWindowSurface(
            display_,
            eglConfig_,
            reinterpret_cast<EGLNativeWindowType>(window_),
            explicitColorSpace ? surfaceAttributes : nullptr);
        if (surface_ == EGL_NO_SURFACE) {
            error = eglError("eglCreateWindowSurface", lastEglError_);
            return false;
        }
        if (explicitColorSpace) {
            EGLint actualColorSpace = EGL_GL_COLORSPACE_DEFAULT_EXT;
            if (eglQuerySurface(
                    display_,
                    surface_,
                    EGL_GL_COLORSPACE_KHR,
                    &actualColorSpace) != EGL_TRUE
                || actualColorSpace != EGL_GL_COLORSPACE_SRGB_KHR) {
                error = "The OHOS EGL surface did not retain the requested sRGB color space";
                eglDestroySurface(display_, surface_);
                surface_ = EGL_NO_SURFACE;
                return false;
            }
        }

        EGLint width = 0;
        EGLint height = 0;
        if (eglQuerySurface(
                display_, surface_, EGL_WIDTH, &width) != EGL_TRUE
            || eglQuerySurface(
                   display_, surface_, EGL_HEIGHT, &height) != EGL_TRUE
            || width <= 0 || height <= 0) {
            error = eglError("eglQuerySurface", lastEglError_);
            eglDestroySurface(display_, surface_);
            surface_ = EGL_NO_SURFACE;
            return false;
        }
        surfaceSize_ = { width, height };
        ++generation_;
        return true;
    }

    bool createSurface(std::string& error)
    {
        if (surface_ != EGL_NO_SURFACE) {
            return true;
        }
        if (!window_) {
            error = "The OHOS native window is unavailable";
            return false;
        }
        if (!initializeDisplay(error)) {
            return false;
        }
        if (outputPreference_ == OpenGLOutputPreference::RequireHdr) {
            error = "Native OHOS EGL HDR output has not passed the required format, color-space, and compositor capability gate";
            return false;
        }
        if (context_ != EGL_NO_CONTEXT && eglConfig_) {
            return createWindowSurface(error);
        }

        if (!chooseSdrConfig(
                display_,
                eglConfig_,
                nativeVisual_,
                error,
                lastEglError_)) {
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
            error = eglError("eglCreateContext", lastEglError_);
            eglConfig_ = nullptr;
            nativeVisual_ = 0;
            return false;
        }
        if (createWindowSurface(error)) {
            return true;
        }
        eglDestroyContext(display_, context_);
        context_ = EGL_NO_CONTEXT;
        eglConfig_ = nullptr;
        nativeVisual_ = 0;
        return false;
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
            error = "The OHOS EGL context or window surface is unavailable";
            lastEglError_ = EGL_BAD_SURFACE;
            return false;
        }
        if (eglMakeCurrent(
                display_, surface_, surface_, context_) != EGL_TRUE) {
            error = eglError("eglMakeCurrent", lastEglError_);
            return false;
        }
        lastEglError_ = EGL_SUCCESS;
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
            error = "The OHOS OpenGL ES surface configuration is invalid";
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
                ? "The OpenGL ES engine could not open on OHOS"
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
        nativeVisual_ = 0;
        extensions_.clear();
        lastEglError_ = EGL_SUCCESS;
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
            OH_NativeWindow_NativeObjectUnreference(window_);
            window_ = nullptr;
        }
    }

    mutable std::recursive_mutex mutex_;
    EventCallback eventCallback_;
    OpenGLOutputPreference outputPreference_ =
        OpenGLOutputPreference::PreferHdr;
    EGLDisplay display_ = EGL_NO_DISPLAY;
    EGLConfig eglConfig_ = nullptr;
    EGLContext context_ = EGL_NO_CONTEXT;
    EGLSurface surface_ = EGL_NO_SURFACE;
    OHNativeWindow* window_ = nullptr;
    VideoSize surfaceSize_;
    std::uint64_t generation_ = 0;
    std::string extensions_;
    EGLint nativeVisual_ = 0;
    VideoRenderConfig renderConfig_;
    bool open_ = false;
    bool engineOpen_ = false;
    std::string lastEngineError_;
    EGLint lastEglError_ = EGL_SUCCESS;
    EGLint lastPresentEglError_ = EGL_SUCCESS;
    OpenGLVideoRenderer renderer_;
};

OHOSOpenGLVideoRenderer::OHOSOpenGLVideoRenderer(
    OpenGLOutputPreference outputPreference)
    : impl_(std::make_unique<Impl>(outputPreference))
{
}

OHOSOpenGLVideoRenderer::~OHOSOpenGLVideoRenderer() = default;
OHOSOpenGLVideoRenderer::OHOSOpenGLVideoRenderer(
    OHOSOpenGLVideoRenderer&&) noexcept = default;
OHOSOpenGLVideoRenderer& OHOSOpenGLVideoRenderer::operator=(
    OHOSOpenGLVideoRenderer&&) noexcept = default;

VideoRenderCapabilities OHOSOpenGLVideoRenderer::capabilities() const
{
    if (!impl_) {
        return {};
    }
    VideoRenderCapabilities result = impl_->renderer_.capabilities();
    result.ownedContext = true;
    result.ownedSurface = true;
    return result;
}

void OHOSOpenGLVideoRenderer::setEventCallback(
    EventCallback callback)
{
    if (!impl_) {
        return;
    }
    std::lock_guard<std::recursive_mutex> lock(impl_->mutex_);
    impl_->eventCallback_ = std::move(callback);
}

bool OHOSOpenGLVideoRenderer::open(const VideoRenderConfig& config)
{
    if (!impl_) {
        return false;
    }
    std::string error;
    bool opened = false;
    {
        std::lock_guard<std::recursive_mutex> lock(impl_->mutex_);
        if (!impl_->window_) {
            error = "The OHOS native window is unavailable";
        } else if (impl_->createSurface(error)
                   && impl_->openEngine(config, error)) {
            impl_->open_ = true;
            opened = true;
        }
    }
    if (!opened) {
        impl_->notify(VideoRenderEventType::Error, std::move(error));
    }
    return opened;
}

bool OHOSOpenGLVideoRenderer::configure(
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
        error = "The OHOS OpenGL ES renderer is closed or has no surface";
    }
    if (!configured) {
        impl_->notify(VideoRenderEventType::Error, std::move(error));
    }
    return configured;
}

VideoRenderAttemptResult OHOSOpenGLVideoRenderer::renderDetailed(
    const VideoFrame& frame)
{
    if (!impl_) {
        return {
            VideoRenderAttemptStatus::FatalError,
            0,
            "The OHOS OpenGL ES renderer is unavailable",
        };
    }
    std::string error;
    VideoRenderAttemptResult attempt {
        VideoRenderAttemptStatus::FatalError,
        0,
        {},
    };
    EGLint eglCode = EGL_SUCCESS;
    {
        std::lock_guard<std::recursive_mutex> lock(impl_->mutex_);
        if (!impl_->open_) {
            error = "The OHOS OpenGL ES renderer is not open";
        } else if (!impl_->makeCurrent(error)) {
            eglCode = impl_->lastEglError_;
        } else {
            impl_->lastEngineError_.clear();
            impl_->lastPresentEglError_ = EGL_SUCCESS;
            OpenGLHardwareImportStatus hardwareStatus =
                OpenGLHardwareImportStatus::Ready;
            if (frame.hasHardwareFrame()) {
                hardwareStatus = impl_->renderer_.prepareHardwareFrame(
                    frame, &error);
            }
            if (hardwareStatus
                == OpenGLHardwareImportStatus::Pending) {
                attempt = {
                    VideoRenderAttemptStatus::DeferredUntilRedraw,
                    0,
                    error,
                };
                error.clear();
            } else if (hardwareStatus
                       == OpenGLHardwareImportStatus::Stale) {
                attempt = {
                    VideoRenderAttemptStatus::Discarded,
                    0,
                    error,
                };
                error.clear();
            } else if (hardwareStatus
                       != OpenGLHardwareImportStatus::Ready) {
                if (error.empty()) {
                    error = "OHOS OpenGL ES hardware-frame preparation failed";
                }
            } else {
                attempt = impl_->renderer_.renderDetailed(frame);
                if (attempt.status
                        == VideoRenderAttemptStatus::FatalError
                    || attempt.status
                        == VideoRenderAttemptStatus::SurfaceLost) {
                    error = attempt.detail.empty()
                        ? impl_->lastEngineError_
                        : attempt.detail;
                }
            }
            if (!attempt.presented()
                && impl_->lastPresentEglError_ != EGL_SUCCESS) {
                eglCode = impl_->lastPresentEglError_;
            }
        }
        impl_->doneCurrent();
    }
    if (attempt.status == VideoRenderAttemptStatus::DeferredUntilRedraw
        || attempt.status == VideoRenderAttemptStatus::RetryAfterBackoff
        || attempt.status == VideoRenderAttemptStatus::Discarded) {
        return attempt;
    }
    if (attempt.presented()) {
        return attempt;
    }
    const bool surfaceLost =
        attempt.status == VideoRenderAttemptStatus::SurfaceLost
        || eglLifecycleLoss(eglCode)
        || error.find("surface") != std::string::npos
        || error.find("window") != std::string::npos;
    if (error.empty()) {
        error = surfaceLost
            ? "The OHOS OpenGL ES surface is unavailable"
            : "The OHOS OpenGL ES renderer could not render the frame";
    }
    impl_->notify(
        surfaceLost ? VideoRenderEventType::SurfaceLost
                    : VideoRenderEventType::Error,
        error);
    return {
        surfaceLost ? VideoRenderAttemptStatus::SurfaceLost
                    : VideoRenderAttemptStatus::FatalError,
        0,
        error,
    };
}

bool OHOSOpenGLVideoRenderer::render(const VideoFrame& frame)
{
    return renderDetailed(frame).frameConsumed();
}

void OHOSOpenGLVideoRenderer::close() noexcept
{
    if (impl_) {
        impl_->close();
    }
}

bool OHOSOpenGLVideoRenderer::setWindow(OHNativeWindow* window)
{
    if (!impl_) {
        return false;
    }
    std::string error;
    bool succeeded = false;
    {
        std::lock_guard<std::recursive_mutex> lock(impl_->mutex_);
        const bool sameWindow = window == impl_->window_;
        impl_->destroySurface();
        if (!sameWindow && impl_->window_) {
            OH_NativeWindow_NativeObjectUnreference(impl_->window_);
            impl_->window_ = nullptr;
        }
        if (!sameWindow && window) {
            if (OH_NativeWindow_NativeObjectReference(window) != 0) {
                error = "Could not retain the OHOS native window";
            } else {
                impl_->window_ = window;
            }
        }
        if (impl_->window_) {
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
                ? "The OHOS native window was removed"
                : std::move(error));
    }
    return succeeded;
}

VideoSize OHOSOpenGLVideoRenderer::surfaceSize() const noexcept
{
    if (!impl_) {
        return {};
    }
    std::lock_guard<std::recursive_mutex> lock(impl_->mutex_);
    return impl_->surfaceSize_;
}

std::uint64_t OHOSOpenGLVideoRenderer::surfaceGeneration() const noexcept
{
    if (!impl_) {
        return 0;
    }
    std::lock_guard<std::recursive_mutex> lock(impl_->mutex_);
    return impl_->generation_;
}

OpenGLOutputColorSpace
OHOSOpenGLVideoRenderer::outputColorSpace() const noexcept
{
    return OpenGLOutputColorSpace::SdrSrgb;
}

bool OHOSOpenGLVideoRenderer::hdrOutputActive() const noexcept
{
    return false;
}

int OHOSOpenGLVideoRenderer::colorComponentBits() const noexcept
{
    if (!impl_) {
        return 0;
    }
    std::lock_guard<std::recursive_mutex> lock(impl_->mutex_);
    return impl_->surface_ != EGL_NO_SURFACE ? 8 : 0;
}

void OHOSOpenGLVideoRenderer::setHardwareFrameInterop(
    std::shared_ptr<OpenGLHardwareFrameInterop> hardwareInterop)
{
    if (!impl_) {
        return;
    }
    std::lock_guard<std::recursive_mutex> lock(impl_->mutex_);
    std::string error;
    const bool needsCurrent = impl_->engineOpen_;
    if (needsCurrent && !impl_->makeCurrent(error)) {
        impl_->notify(
            VideoRenderEventType::Error,
            error.empty()
                ? "Could not make the OHOS OpenGL ES context current for interop replacement"
                : std::move(error));
        return;
    }
    impl_->renderer_.setHardwareFrameInterop(std::move(hardwareInterop));
    if (needsCurrent) {
        impl_->doneCurrent();
    }
}

std::shared_ptr<OpenGLHardwareFrameInterop>
OHOSOpenGLVideoRenderer::hardwareFrameInterop() const noexcept
{
    if (!impl_) {
        return {};
    }
    std::lock_guard<std::recursive_mutex> lock(impl_->mutex_);
    return impl_->renderer_.hardwareFrameInterop();
}

} // namespace qtav
