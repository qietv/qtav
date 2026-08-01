// SPDX-License-Identifier: LGPL-2.1-or-later

#include <qtav/android_opengl_video_renderer.h>

#include <EGL/egl.h>
#include <EGL/eglext.h>
#include <android/data_space.h>

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

bool eglLifecycleLoss(EGLint code) noexcept
{
    return code == EGL_BAD_SURFACE
        || code == EGL_BAD_NATIVE_WINDOW
        || code == EGL_BAD_CONTEXT
        || code == EGL_BAD_DISPLAY
        || code == EGL_CONTEXT_LOST;
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

struct EGLSurfaceCandidate {
    OpenGLOutputColorSpace colorSpace =
        OpenGLOutputColorSpace::SdrSrgb;
    EGLint eglColorSpace = EGL_GL_COLORSPACE_SRGB_KHR;
    ADataSpace dataSpace = ADATASPACE_SRGB;
    int componentBits = 8;
    const char* name = "SDR sRGB";
};

bool chooseConfig(
    EGLDisplay display,
    int componentBits,
    EGLConfig& result,
    EGLint& nativeVisual,
    std::string& error)
{
    const EGLint alphaBits = componentBits == 10 ? 2 : 8;
    const EGLint attributes[] {
        EGL_SURFACE_TYPE, EGL_WINDOW_BIT,
        EGL_RENDERABLE_TYPE, EGL_OPENGL_ES3_BIT,
        EGL_RED_SIZE, componentBits,
        EGL_GREEN_SIZE, componentBits,
        EGL_BLUE_SIZE, componentBits,
        EGL_ALPHA_SIZE, alphaBits,
        EGL_DEPTH_SIZE, 0,
        EGL_STENCIL_SIZE, 0,
        EGL_NONE,
    };
    EGLint count = 0;
    if (eglChooseConfig(
            display, attributes, nullptr, 0, &count) != EGL_TRUE
        || count <= 0) {
        error = componentBits == 10
            ? "No exact RGB10_A2 OpenGL ES 3 window EGLConfig is available"
            : "No exact RGBA8 OpenGL ES 3 window EGLConfig is available";
        return false;
    }
    std::vector<EGLConfig> configs(static_cast<std::size_t>(count));
    if (eglChooseConfig(
            display,
            attributes,
            configs.data(),
            count,
            &count) != EGL_TRUE) {
        error = eglError("eglChooseConfig");
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
        if (red == componentBits && green == componentBits
            && blue == componentBits && alpha == alphaBits
            && visual != 0) {
            result = config;
            nativeVisual = visual;
            return true;
        }
    }
    error = componentBits == 10
        ? "The EGL display exposes no exact RGB10_A2 native window config"
        : "The EGL display exposes no exact RGBA8 native window config";
    return false;
}

} // namespace

class AndroidOpenGLVideoRenderer::Impl {
public:
    explicit Impl(OpenGLOutputPreference outputPreference)
        : outputPreference_(outputPreference)
        , renderer_([this] {
              return OpenGLRenderTarget {
                  0,
                  surfaceSize_,
                  generation_,
                  outputColorSpace_,
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

    bool initializeDisplay(std::string& error)
    {
        if (display_ != EGL_NO_DISPLAY) {
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
            display_ = EGL_NO_DISPLAY;
            return false;
        }
        if (eglBindAPI(EGL_OPENGL_ES_API) != EGL_TRUE) {
            error = eglError("eglBindAPI");
            eglTerminate(display_);
            display_ = EGL_NO_DISPLAY;
            return false;
        }
        const char* extensions =
            eglQueryString(display_, EGL_EXTENSIONS);
        extensions_ = extensions ? extensions : "";
        return true;
    }

    std::vector<EGLSurfaceCandidate> surfaceCandidates() const
    {
        std::vector<EGLSurfaceCandidate> result;
        const bool colorspace =
            hasExtension(
                extensions_.c_str(), "EGL_KHR_gl_colorspace");
        if (outputPreference_ != OpenGLOutputPreference::SdrOnly
            && colorspace) {
            if (hasExtension(
                    extensions_.c_str(),
                    "EGL_EXT_gl_colorspace_bt2020_pq")) {
                result.push_back({
                    OpenGLOutputColorSpace::HDR10PQ,
                    EGL_GL_COLORSPACE_BT2020_PQ_EXT,
                    ADATASPACE_BT2020_PQ,
                    10,
                    "BT.2020/PQ",
                });
            }
            if (hasExtension(
                    extensions_.c_str(),
                    "EGL_EXT_gl_colorspace_bt2020_hlg")) {
                result.push_back({
                    OpenGLOutputColorSpace::HDR10HLG,
                    EGL_GL_COLORSPACE_BT2020_HLG_EXT,
                    ADATASPACE_BT2020_HLG,
                    10,
                    "BT.2020/HLG",
                });
            }
        }
        if (outputPreference_
            != OpenGLOutputPreference::RequireHdr) {
            result.push_back({
                OpenGLOutputColorSpace::SdrSrgb,
                EGL_GL_COLORSPACE_SRGB_KHR,
                ADATASPACE_SRGB,
                8,
                "SDR sRGB",
            });
        }
        return result;
    }

    bool createWindowSurface(
        const EGLSurfaceCandidate& candidate,
        EGLint nativeVisual,
        std::string& error)
    {
        if (ANativeWindow_setBuffersGeometry(
                window_, 0, 0, nativeVisual) != 0) {
            error =
                "ANativeWindow_setBuffersGeometry rejected the selected "
                + std::string(candidate.name) + " EGL native visual";
            return false;
        }
        const bool explicitColorSpace =
            hasExtension(
                extensions_.c_str(), "EGL_KHR_gl_colorspace");
        const EGLint surfaceAttributes[] {
            EGL_GL_COLORSPACE_KHR,
            candidate.eglColorSpace,
            EGL_NONE,
        };
        surface_ = eglCreateWindowSurface(
            display_,
            eglConfig_,
            window_,
            explicitColorSpace ? surfaceAttributes : nullptr);
        if (surface_ == EGL_NO_SURFACE) {
            error = eglError("eglCreateWindowSurface");
            return false;
        }

        const int32_t dataSpaceResult =
            ANativeWindow_setBuffersDataSpace(
                window_, candidate.dataSpace);
        const int32_t actualDataSpace =
            ANativeWindow_getBuffersDataSpace(window_);
        if (dataSpaceResult != 0
            || actualDataSpace != candidate.dataSpace) {
            error =
                "The Android native window rejected the selected "
                + std::string(candidate.name) + " dataspace";
            eglDestroySurface(display_, surface_);
            surface_ = EGL_NO_SURFACE;
            return false;
        }
        if (explicitColorSpace) {
            EGLint actualColorSpace = EGL_GL_COLORSPACE_DEFAULT_EXT;
            if (eglQuerySurface(
                    display_,
                    surface_,
                    EGL_GL_COLORSPACE_KHR,
                    &actualColorSpace) != EGL_TRUE
                || actualColorSpace != candidate.eglColorSpace) {
                error =
                    "The EGL window surface did not retain the requested "
                    + std::string(candidate.name) + " color space";
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
                   display_,
                   surface_,
                   EGL_HEIGHT,
                   &height) != EGL_TRUE
            || width <= 0 || height <= 0) {
            error = eglError("eglQuerySurface");
            eglDestroySurface(display_, surface_);
            surface_ = EGL_NO_SURFACE;
            return false;
        }
        surfaceSize_ = { width, height };
        outputColorSpace_ = candidate.colorSpace;
        colorComponentBits_ = candidate.componentBits;
        activeCandidate_ = candidate;
        nativeVisual_ = nativeVisual;
        ++generation_;
        return true;
    }

    bool createCandidate(
        const EGLSurfaceCandidate& candidate,
        std::string& error)
    {
        EGLConfig config = nullptr;
        EGLint nativeVisual = 0;
        if (!chooseConfig(
                display_,
                candidate.componentBits,
                config,
                nativeVisual,
                error)) {
            return false;
        }
        const EGLint contextAttributes[] {
            EGL_CONTEXT_CLIENT_VERSION, 3,
            EGL_NONE,
        };
        eglConfig_ = config;
        context_ = eglCreateContext(
            display_,
            eglConfig_,
            EGL_NO_CONTEXT,
            contextAttributes);
        if (context_ == EGL_NO_CONTEXT) {
            error = eglError("eglCreateContext");
            eglConfig_ = nullptr;
            return false;
        }
        if (createWindowSurface(candidate, nativeVisual, error)) {
            return true;
        }
        eglDestroyContext(display_, context_);
        context_ = EGL_NO_CONTEXT;
        eglConfig_ = nullptr;
        return false;
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
        if (!initializeDisplay(error)) {
            return false;
        }
        if (context_ != EGL_NO_CONTEXT && eglConfig_) {
            return createWindowSurface(
                activeCandidate_, nativeVisual_, error);
        }

        const std::vector<EGLSurfaceCandidate> candidates =
            surfaceCandidates();
        if (candidates.empty()) {
            error =
                "The Android EGL display exposes no implemented native HDR "
                "color-space extension";
            return false;
        }
        std::string failures;
        for (const EGLSurfaceCandidate& candidate : candidates) {
            std::string candidateError;
            if (createCandidate(candidate, candidateError)) {
                return true;
            }
            if (!failures.empty()) {
                failures += "; ";
            }
            failures += std::string(candidate.name) + ": "
                + (candidateError.empty()
                        ? "surface creation failed"
                        : candidateError);
        }
        error = outputPreference_
                == OpenGLOutputPreference::RequireHdr
            ? "The Android EGL display could not create a required native "
                "HDR surface: " + failures
            : "The Android EGL display could not create an HDR or SDR "
                "surface: " + failures;
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
        extensions_.clear();
        activeCandidate_ = {};
        nativeVisual_ = 0;
        outputColorSpace_ = OpenGLOutputColorSpace::SdrSrgb;
        colorComponentBits_ = 0;
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
    OpenGLOutputPreference outputPreference_ =
        OpenGLOutputPreference::PreferHdr;
    EGLDisplay display_ = EGL_NO_DISPLAY;
    EGLConfig eglConfig_ = nullptr;
    EGLContext context_ = EGL_NO_CONTEXT;
    EGLSurface surface_ = EGL_NO_SURFACE;
    ANativeWindow* window_ = nullptr;
    VideoSize surfaceSize_;
    std::uint64_t generation_ = 0;
    std::string extensions_;
    EGLSurfaceCandidate activeCandidate_;
    EGLint nativeVisual_ = 0;
    OpenGLOutputColorSpace outputColorSpace_ =
        OpenGLOutputColorSpace::SdrSrgb;
    int colorComponentBits_ = 0;
    VideoRenderConfig renderConfig_;
    bool open_ = false;
    bool engineOpen_ = false;
    std::string lastEngineError_;
    OpenGLVideoRenderer renderer_;
};

AndroidOpenGLVideoRenderer::AndroidOpenGLVideoRenderer(
    OpenGLOutputPreference outputPreference)
    : impl_(std::make_unique<Impl>(outputPreference))
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
    bool retryPending = false;
    bool staleFrame = false;
    EGLint eglCode = EGL_SUCCESS;
    {
        std::lock_guard<std::recursive_mutex> lock(impl_->mutex_);
        if (!impl_->open_) {
            error = "The Android OpenGL ES renderer is not open";
        } else if (!impl_->makeCurrent(error)) {
            eglCode = eglGetError();
        } else {
            impl_->lastEngineError_.clear();
            OpenGLHardwareImportStatus hardwareStatus =
                OpenGLHardwareImportStatus::Ready;
            if (frame.hasHardwareFrame()) {
                hardwareStatus =
                    impl_->renderer_.prepareHardwareFrame(
                        frame,
                        &error);
                retryPending =
                    hardwareStatus
                    == OpenGLHardwareImportStatus::Pending;
                staleFrame =
                    hardwareStatus
                    == OpenGLHardwareImportStatus::Stale;
            }
            if (retryPending || staleFrame) {
                error.clear();
            } else if (hardwareStatus
                           != OpenGLHardwareImportStatus::Ready) {
                if (error.empty()) {
                    error =
                        "The Android OpenGL ES hardware-frame preparation failed";
                }
            } else if (!impl_->renderer_.render(frame)) {
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
    if (retryPending || staleFrame) {
        return false;
    }
    if (!rendered) {
        const VideoRenderEventType type =
            eglLifecycleLoss(eglCode)
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
        const bool sameWindow = window == impl_->window_;
        // SurfaceView can publish a same-identity Surface whose new base
        // geometry is not yet reflected by ANativeWindow_getWidth/Height.
        // Recreate the EGL surface so setBuffersGeometry(0, 0, visual) adopts
        // the latest Java-side base size.
        impl_->destroySurface();
        if (!sameWindow && impl_->window_) {
            ANativeWindow_release(impl_->window_);
        }
        if (!sameWindow) {
            impl_->window_ = window;
            if (impl_->window_) {
                ANativeWindow_acquire(impl_->window_);
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

OpenGLOutputColorSpace
AndroidOpenGLVideoRenderer::outputColorSpace() const noexcept
{
    if (!impl_) {
        return OpenGLOutputColorSpace::SdrSrgb;
    }
    std::lock_guard<std::recursive_mutex> lock(impl_->mutex_);
    return impl_->outputColorSpace_;
}

bool AndroidOpenGLVideoRenderer::hdrOutputActive() const noexcept
{
    if (!impl_) {
        return false;
    }
    std::lock_guard<std::recursive_mutex> lock(impl_->mutex_);
    return impl_->surface_ != EGL_NO_SURFACE
        && impl_->colorComponentBits_ >= 10
        && openGLColorSpaceIsHdr(impl_->outputColorSpace_);
}

int AndroidOpenGLVideoRenderer::colorComponentBits() const noexcept
{
    if (!impl_) {
        return 0;
    }
    std::lock_guard<std::recursive_mutex> lock(impl_->mutex_);
    return impl_->surface_ != EGL_NO_SURFACE
        ? impl_->colorComponentBits_
        : 0;
}

void AndroidOpenGLVideoRenderer::setHardwareFrameInterop(
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
                ? "Could not make the Android OpenGL ES context current for interop replacement"
                : std::move(error));
        return;
    }
    impl_->renderer_.setHardwareFrameInterop(
        std::move(hardwareInterop));
    if (needsCurrent) {
        impl_->doneCurrent();
    }
}

std::shared_ptr<OpenGLHardwareFrameInterop>
AndroidOpenGLVideoRenderer::hardwareFrameInterop() const noexcept
{
    if (!impl_) {
        return {};
    }
    std::lock_guard<std::recursive_mutex> lock(impl_->mutex_);
    return impl_->renderer_.hardwareFrameInterop();
}

} // namespace qtav
