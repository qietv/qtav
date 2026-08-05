// SPDX-License-Identifier: LGPL-2.1-or-later

#include <qtav/mobile_video_renderer.h>

#include <algorithm>
#include <mutex>
#include <string>
#include <utility>

namespace qtav {
namespace {

std::string apiLabel(MobileRenderAPI api)
{
    return mobileRenderAPIName(api);
}

std::string joinDetail(
    const std::string& prefix,
    const std::string& detail)
{
    if (prefix.empty()) {
        return detail;
    }
    if (detail.empty()) {
        return prefix;
    }
    return prefix + ": " + detail;
}

} // namespace

const char* mobileRenderAPIName(MobileRenderAPI api) noexcept
{
    switch (api) {
    case MobileRenderAPI::None:
        return "none";
    case MobileRenderAPI::Vulkan:
        return "vulkan";
    case MobileRenderAPI::OpenGLES:
        return "opengl-es";
    }
    return "unknown";
}

const char* mobileHardwareFrameFallbackRouteName(
    MobileHardwareFrameFallbackRoute route) noexcept
{
    switch (route) {
    case MobileHardwareFrameFallbackRoute::None:
        return "none";
    case MobileHardwareFrameFallbackRoute::OpenGLESInterop:
        return "opengl-es-interop";
    case MobileHardwareFrameFallbackRoute::DirectSurface:
        return "direct-surface";
    case MobileHardwareFrameFallbackRoute::SoftwareDecode:
        return "software-decode";
    case MobileHardwareFrameFallbackRoute::NoVideo:
        return "no-video";
    }
    return "unknown";
}

class MobileVideoRendererSelector::Impl {
public:
    explicit Impl(MobileRendererSelectorConfig config)
        : selectorConfig_(std::move(config))
    {
    }

    ~Impl()
    {
        close();
    }

    MobileRendererFactory& factory(MobileRenderAPI api)
    {
        return api == MobileRenderAPI::Vulkan
            ? selectorConfig_.vulkan
            : selectorConfig_.openGLES;
    }

    void notifyRender(const VideoRenderEvent& event)
    {
        EventCallback callback = eventCallback_;
        if (callback) {
            callback(event);
        }
    }

    void notifySelection(
        MobileRendererSelectionEventType type,
        MobileRenderAPI previous,
        MobileRenderAPI selected,
        std::string detail)
    {
        SelectionCallback callback = selectionCallback_;
        if (callback) {
            callback({
                type,
                previous,
                selected,
                sessionGeneration_,
                std::move(detail),
            });
        }
    }

    bool candidateSupports(
        HardwareDeviceType device) const
    {
        if (!renderer_ || device == HardwareDeviceType::Unknown) {
            return false;
        }
        const auto devices = renderer_->capabilities().hardwareDevices;
        return std::find(devices.begin(), devices.end(), device)
            != devices.end();
    }

    static NativeHandle hardwareSurface(
        const VideoFrame& frame) noexcept
    {
        if (!frame.hasHardwareFrame()) {
            return {};
        }
        return frame.hardwareFrame().nativeHandle(
            HardwareHandleType::Surface);
    }

    bool isRetiredHardwareFrame(
        const VideoFrame& frame) const noexcept
    {
        if (!frame.hasHardwareFrame()) {
            return false;
        }
        if (hardwareFallbackRoute_
            == MobileHardwareFrameFallbackRoute::SoftwareDecode) {
            return true;
        }
        if (hardwareFallbackRoute_
                != MobileHardwareFrameFallbackRoute::OpenGLESInterop
            || retiredHardwareDevice_
                == HardwareDeviceType::Unknown) {
            return false;
        }
        const HardwareFrame hardware = frame.hardwareFrame();
        if (hardware.deviceType() != retiredHardwareDevice_) {
            return false;
        }
        const NativeHandle surface =
            hardware.nativeHandle(HardwareHandleType::Surface);
        return retiredHardwareSurface_
            && surface.value == retiredHardwareSurface_.value
            && surface.subresource
                == retiredHardwareSurface_.subresource;
    }

    void onCandidateEvent(
        MobileRenderAPI api,
        std::uint64_t generation,
        const VideoRenderEvent& event)
    {
        std::lock_guard<std::recursive_mutex> lock(mutex_);
        if (generation != candidateGeneration_
            || api != selectedAPI_ || !renderer_) {
            return;
        }
        if (event.type == VideoRenderEventType::RedrawRequested) {
            notifyRender(event);
            return;
        }
        pendingEvent_ = event;
        hasPendingEvent_ = true;
    }

    void clearPendingEvent()
    {
        pendingEvent_ = {};
        hasPendingEvent_ = false;
    }

    VideoRenderEvent takeFailureEvent(std::string fallbackDetail)
    {
        if (hasPendingEvent_) {
            VideoRenderEvent result = std::move(pendingEvent_);
            clearPendingEvent();
            if (result.detail.empty()) {
                result.detail = std::move(fallbackDetail);
            }
            return result;
        }
        return {
            VideoRenderEventType::Error,
            std::move(fallbackDetail),
        };
    }

    void closeRenderer()
    {
        if (!renderer_) {
            return;
        }
        renderer_->setEventCallback({});
        renderer_->close();
        renderer_.reset();
        clearPendingEvent();
        ++candidateGeneration_;
    }

    bool activate(MobileRenderAPI api, std::string& failure)
    {
        MobileRendererFactory& create = factory(api);
        if (!create) {
            failure = apiLabel(api) + " renderer factory is unavailable";
            return false;
        }

        MobileRendererCandidate candidate = create();
        if (!candidate.renderer) {
            failure = candidate.detail.empty()
                ? apiLabel(api) + " renderer is unavailable"
                : candidate.detail;
            return false;
        }

        const std::uint64_t generation = ++candidateGeneration_;
        selectedAPI_ = api;
        renderer_ = candidate.renderer;
        clearPendingEvent();
        renderer_->setEventCallback(
            [this, api, generation](const VideoRenderEvent& event) {
                onCandidateEvent(api, generation, event);
            });
        if (!renderer_->open(renderConfig_)) {
            VideoRenderEvent event = takeFailureEvent(
                apiLabel(api) + " renderer failed to open");
            closeRenderer();
            selectedAPI_ = MobileRenderAPI::None;
            failure = joinDetail(event.detail, candidate.detail);
            return false;
        }

        suspended_ = false;
        lastError_.clear();
        return true;
    }

    bool renderOnce(
        const VideoFrame& frame,
        VideoRenderEvent& failure)
    {
        if (!renderer_) {
            lastAttempt_ = {
                suspended_ ? VideoRenderAttemptStatus::SurfaceLost
                           : VideoRenderAttemptStatus::FatalError,
                0,
                suspended_
                    ? "The mobile renderer surface is suspended"
                    : "Mobile video presentation is unavailable",
            };
            failure = {
                suspended_ ? VideoRenderEventType::SurfaceLost
                           : VideoRenderEventType::Error,
                suspended_
                    ? "The mobile renderer surface is suspended"
                    : "Mobile video presentation is unavailable",
            };
            return false;
        }
        clearPendingEvent();
        lastAttempt_ = renderer_->renderDetailed(frame);
        if (lastAttempt_.frameConsumed()) {
            clearPendingEvent();
            return true;
        }
        if (!hasPendingEvent_) {
            VideoRenderEventType failureType =
                VideoRenderEventType::RedrawRequested;
            if (lastAttempt_.status
                == VideoRenderAttemptStatus::SurfaceLost) {
                failureType = VideoRenderEventType::SurfaceLost;
            } else if (lastAttempt_.status
                       == VideoRenderAttemptStatus::FatalError) {
                failureType = VideoRenderEventType::Error;
            }
            failure = {
                failureType,
                lastAttempt_.detail.empty()
                    ? apiLabel(selectedAPI_)
                        + " renderer deferred this retryable render attempt"
                    : lastAttempt_.detail,
            };
            return false;
        }
        failure = takeFailureEvent(
            apiLabel(selectedAPI_) + " renderer failed to render");
        return false;
    }

    bool fallbackToOpenGLES(
        MobileRenderAPI previous,
        std::string reason,
        const VideoFrame* retryFrame)
    {
        const bool hardwareFallback =
            retryFrame && retryFrame->hasHardwareFrame();
        HardwareDeviceType sourceDevice =
            HardwareDeviceType::Unknown;
        NativeHandle sourceSurface;
        if (hardwareFallback) {
            const HardwareFrame hardware =
                retryFrame->hardwareFrame();
            sourceDevice = hardware.deviceType();
            sourceSurface = hardwareSurface(*retryFrame);
        }

        closeRenderer();
        selectedAPI_ = MobileRenderAPI::None;
        vulkanRetired_ = true;

        std::string openGLError;
        if (!activate(MobileRenderAPI::OpenGLES, openGLError)) {
            if (hardwareFallback) {
                return applyHardwareFrameFallback(
                    previous,
                    std::move(reason),
                    sourceDevice,
                    sourceSurface,
                    std::move(openGLError));
            }
            const std::string detail =
                joinDetail(
                    joinDetail(
                        "Vulkan was retired",
                        reason),
                    joinDetail(
                        "OpenGL ES is unavailable",
                        openGLError));
            return becomeUnavailable(previous, detail);
        }

        if (hardwareFallback) {
            return applyHardwareFrameFallback(
                previous,
                std::move(reason),
                sourceDevice,
                sourceSurface,
                {});
        }

        notifySelection(
            MobileRendererSelectionEventType::FellBack,
            previous,
            MobileRenderAPI::OpenGLES,
            joinDetail(
                "Selected OpenGL ES after Vulkan failure",
                reason));

        if (!retryFrame) {
            return true;
        }
        VideoRenderEvent failure;
        if (renderOnce(*retryFrame, failure)) {
            return true;
        }
        return processFailure(
            MobileRenderAPI::OpenGLES,
            std::move(failure),
            retryFrame);
    }

    bool applyHardwareFrameFallback(
        MobileRenderAPI previous,
        std::string reason,
        HardwareDeviceType sourceDevice,
        NativeHandle sourceSurface,
        std::string openGLError)
    {
        if (!hardwareFallbackCallback_) {
            return becomeUnavailable(
                previous,
                joinDetail(
                    joinDetail(
                        "A Vulkan hardware frame requires an explicit "
                        "cross-API fallback policy",
                        reason),
                    openGLError));
        }

        MobileHardwareFrameFallbackEvent event;
        event.previousAPI = previous;
        event.selectedAPI = selectedAPI_;
        event.sourceDevice = sourceDevice;
        event.sourceSurfaceGeneration = sourceSurface.subresource;
        event.sessionGeneration = sessionGeneration_;
        event.detail = joinDetail(reason, openGLError);
        const MobileHardwareFrameFallbackDecision decision =
            hardwareFallbackCallback_(event);
        const std::string routeDetail = joinDetail(
            std::string("Selected hardware-frame route ")
                + mobileHardwareFrameFallbackRouteName(decision.route),
            decision.detail);

        if (decision.route
            == MobileHardwareFrameFallbackRoute::OpenGLESInterop) {
            if (selectedAPI_ != MobileRenderAPI::OpenGLES
                || !candidateSupports(sourceDevice)) {
                return becomeUnavailable(
                    previous,
                    joinDetail(
                        "The selected OpenGL ES candidate does not "
                        "advertise compatible native hardware interop",
                        routeDetail));
            }
            hardwareFallbackRoute_ = decision.route;
            retiredHardwareDevice_ = sourceDevice;
            retiredHardwareSurface_ = sourceSurface;
            notifySelection(
                MobileRendererSelectionEventType::FellBack,
                previous,
                MobileRenderAPI::OpenGLES,
                joinDetail(
                    "Selected OpenGL ES after Vulkan hardware-frame "
                    "failure; subsequent decoder output must use the "
                    "new native interop surface",
                    routeDetail));
            lastAttempt_ = {
                VideoRenderAttemptStatus::Discarded,
                0,
                "Discarded the frame from the retired Vulkan interop surface",
            };
            return true;
        }

        if (decision.route
            == MobileHardwareFrameFallbackRoute::SoftwareDecode) {
            if (selectedAPI_ != MobileRenderAPI::OpenGLES
                || !renderer_) {
                return becomeUnavailable(
                    previous,
                    joinDetail(
                        "Software decode fallback has no OpenGL ES "
                        "renderer",
                        routeDetail));
            }
            hardwareFallbackRoute_ = decision.route;
            retiredHardwareDevice_ = sourceDevice;
            retiredHardwareSurface_ = sourceSurface;
            notifySelection(
                MobileRendererSelectionEventType::FellBack,
                previous,
                MobileRenderAPI::OpenGLES,
                joinDetail(
                    "Selected OpenGL ES after Vulkan hardware-frame "
                    "failure; subsequent decoder output must be "
                    "software frames",
                    routeDetail));
            lastAttempt_ = {
                VideoRenderAttemptStatus::Discarded,
                0,
                "Discarded the retired hardware frame while switching to software decode",
            };
            return true;
        }

        if (decision.route
            == MobileHardwareFrameFallbackRoute::DirectSurface) {
            closeRenderer();
            selectedAPI_ = MobileRenderAPI::None;
            hardwareFallbackRoute_ = decision.route;
            retiredHardwareDevice_ = sourceDevice;
            retiredHardwareSurface_ = sourceSurface;
            lastError_.clear();
            notifySelection(
                MobileRendererSelectionEventType::FellBack,
                previous,
                MobileRenderAPI::None,
                joinDetail(
                    "Handed hardware video presentation to the "
                    "application's direct surface",
                    routeDetail));
            lastAttempt_ = {
                VideoRenderAttemptStatus::Discarded,
                0,
                "Handed subsequent hardware presentation to the direct surface",
            };
            return true;
        }

        if (decision.route
            == MobileHardwareFrameFallbackRoute::NoVideo) {
            closeRenderer();
            selectedAPI_ = MobileRenderAPI::None;
            hardwareFallbackRoute_ = decision.route;
            retiredHardwareDevice_ = sourceDevice;
            retiredHardwareSurface_ = sourceSurface;
            lastError_ = joinDetail(
                "Video presentation was disabled by the explicit "
                "hardware-frame fallback policy",
                routeDetail);
            notifySelection(
                MobileRendererSelectionEventType::Unavailable,
                previous,
                MobileRenderAPI::None,
                lastError_);
            lastAttempt_ = {
                VideoRenderAttemptStatus::Discarded,
                0,
                lastError_,
            };
            return true;
        }

        return becomeUnavailable(
            previous,
            joinDetail(
                "The hardware-frame fallback callback returned no "
                "explicit route",
                routeDetail));
    }

    bool becomeUnavailable(
        MobileRenderAPI previous,
        std::string detail)
    {
        closeRenderer();
        selectedAPI_ = MobileRenderAPI::None;
        suspended_ = false;
        lastError_ = detail.empty()
            ? "Mobile video presentation is unavailable"
            : std::move(detail);
        notifySelection(
            MobileRendererSelectionEventType::Unavailable,
            previous,
            MobileRenderAPI::None,
            lastError_);
        notifyRender({
            VideoRenderEventType::Error,
            lastError_,
        });
        lastAttempt_ = {
            VideoRenderAttemptStatus::FatalError,
            0,
            lastError_,
        };
        return false;
    }

    bool recover(
        MobileRenderAPI api,
        std::string reason,
        const VideoFrame* retryFrame)
    {
        closeRenderer();
        selectedAPI_ = MobileRenderAPI::None;

        std::string lastRecoveryError;
        for (std::size_t attempt = 0;
             attempt < selectorConfig_.maximumRecoveryAttempts;
             ++attempt) {
            if (!activate(api, lastRecoveryError)) {
                continue;
            }
            if (retryFrame) {
                VideoRenderEvent retryFailure;
                if (!renderOnce(*retryFrame, retryFailure)) {
                    if (retryFailure.type
                        == VideoRenderEventType::RedrawRequested) {
                        notifySelection(
                            MobileRendererSelectionEventType::Recovered,
                            api,
                            api,
                            joinDetail(
                                "Recreated " + apiLabel(api)
                                    + " renderer on attempt "
                                    + std::to_string(attempt + 1),
                                reason));
                        return false;
                    }
                    lastRecoveryError = retryFailure.detail;
                    if (retryFailure.type
                        != VideoRenderEventType::SurfaceLost) {
                        closeRenderer();
                        selectedAPI_ = MobileRenderAPI::None;
                        break;
                    }
                    closeRenderer();
                    selectedAPI_ = MobileRenderAPI::None;
                    continue;
                }
            }
            notifySelection(
                MobileRendererSelectionEventType::Recovered,
                api,
                api,
                joinDetail(
                    "Recreated " + apiLabel(api)
                        + " renderer on attempt "
                        + std::to_string(attempt + 1),
                    reason));
            return true;
        }

        const std::string exhausted = joinDetail(
            joinDetail(
                "Exhausted "
                    + std::to_string(
                        selectorConfig_.maximumRecoveryAttempts)
                    + " " + apiLabel(api)
                    + " recovery attempt(s)",
                reason),
            lastRecoveryError);
        if (api == MobileRenderAPI::Vulkan) {
            return fallbackToOpenGLES(api, exhausted, retryFrame);
        }
        return becomeUnavailable(api, exhausted);
    }

    bool processFailure(
        MobileRenderAPI api,
        VideoRenderEvent failure,
        const VideoFrame* retryFrame)
    {
        if (failure.type == VideoRenderEventType::RedrawRequested) {
            return false;
        }
        if (failure.type == VideoRenderEventType::SurfaceLost) {
            return recover(api, std::move(failure.detail), retryFrame);
        }
        if (api == MobileRenderAPI::Vulkan) {
            return fallbackToOpenGLES(
                api,
                std::move(failure.detail),
                retryFrame);
        }
        return becomeUnavailable(api, std::move(failure.detail));
    }

    bool open(const VideoRenderConfig& config)
    {
        std::lock_guard<std::recursive_mutex> lock(mutex_);
        closeRenderer();
        selectedAPI_ = MobileRenderAPI::None;
        renderConfig_ = config;
        sessionConfigured_ = true;
        suspended_ = false;
        vulkanRetired_ = false;
        hardwareFallbackRoute_ =
            MobileHardwareFrameFallbackRoute::None;
        retiredHardwareDevice_ = HardwareDeviceType::Unknown;
        retiredHardwareSurface_ = {};
        lastError_.clear();
        ++sessionGeneration_;

        std::string vulkanError;
        if (activate(MobileRenderAPI::Vulkan, vulkanError)) {
            notifySelection(
                MobileRendererSelectionEventType::Selected,
                MobileRenderAPI::None,
                MobileRenderAPI::Vulkan,
                "Selected Vulkan for the new mobile renderer session");
            return true;
        }

        vulkanRetired_ = true;
        std::string openGLError;
        if (activate(MobileRenderAPI::OpenGLES, openGLError)) {
            notifySelection(
                MobileRendererSelectionEventType::FellBack,
                MobileRenderAPI::Vulkan,
                MobileRenderAPI::OpenGLES,
                joinDetail(
                    "Selected OpenGL ES during startup",
                    vulkanError));
            return true;
        }

        return becomeUnavailable(
            MobileRenderAPI::Vulkan,
            joinDetail(
                joinDetail(
                    "Vulkan startup failed",
                    vulkanError),
                joinDetail(
                    "OpenGL ES startup failed",
                    openGLError)));
    }

    bool configure(const VideoRenderConfig& config)
    {
        std::lock_guard<std::recursive_mutex> lock(mutex_);
        if (!sessionConfigured_) {
            return false;
        }
        renderConfig_ = config;
        if (suspended_) {
            return true;
        }
        if (!renderer_) {
            return false;
        }

        const MobileRenderAPI api = selectedAPI_;
        clearPendingEvent();
        if (renderer_->configure(renderConfig_)) {
            clearPendingEvent();
            return true;
        }
        VideoRenderEvent failure = takeFailureEvent(
            apiLabel(api) + " renderer rejected configuration");
        return processFailure(api, std::move(failure), nullptr);
    }

    VideoRenderAttemptResult renderDetailed(const VideoFrame& frame)
    {
        std::lock_guard<std::recursive_mutex> lock(mutex_);
        if (!sessionConfigured_ || suspended_) {
            lastAttempt_ = {
                suspended_ ? VideoRenderAttemptStatus::SurfaceLost
                           : VideoRenderAttemptStatus::FatalError,
                0,
                suspended_
                    ? "The mobile renderer surface is suspended"
                    : "The mobile renderer session is not configured",
            };
            return lastAttempt_;
        }
        if (hardwareFallbackRoute_
                == MobileHardwareFrameFallbackRoute::DirectSurface
            || hardwareFallbackRoute_
                == MobileHardwareFrameFallbackRoute::NoVideo
            || isRetiredHardwareFrame(frame)) {
            lastAttempt_ = {
                VideoRenderAttemptStatus::Discarded,
                0,
                "The frame belongs to a retired or application-owned presentation route",
            };
            return lastAttempt_;
        }
        const MobileRenderAPI api = selectedAPI_;
        VideoRenderEvent failure;
        if (renderOnce(frame, failure)) {
            return lastAttempt_;
        }
        if (api == MobileRenderAPI::None) {
            return lastAttempt_;
        }
        processFailure(api, std::move(failure), &frame);
        return lastAttempt_;
    }

    void close() noexcept
    {
        std::lock_guard<std::recursive_mutex> lock(mutex_);
        closeRenderer();
        selectedAPI_ = MobileRenderAPI::None;
        renderConfig_ = {};
        sessionConfigured_ = false;
        suspended_ = false;
        vulkanRetired_ = false;
        hardwareFallbackRoute_ =
            MobileHardwareFrameFallbackRoute::None;
        retiredHardwareDevice_ = HardwareDeviceType::Unknown;
        retiredHardwareSurface_ = {};
        lastError_.clear();
    }

    void suspendSurface() noexcept
    {
        std::lock_guard<std::recursive_mutex> lock(mutex_);
        if (!sessionConfigured_ || suspended_) {
            return;
        }
        const MobileRenderAPI selected = selectedAPI_;
        closeRenderer();
        selectedAPI_ = selected;
        suspended_ = true;
        notifyRender({
            VideoRenderEventType::SurfaceLost,
            "The current mobile native-window generation was invalidated",
        });
    }

    bool recreateSurface()
    {
        std::lock_guard<std::recursive_mutex> lock(mutex_);
        if (!sessionConfigured_) {
            return false;
        }
        const MobileRenderAPI api = selectedAPI_;
        suspended_ = false;
        if (api == MobileRenderAPI::None) {
            return false;
        }
        return recover(
            api,
            "The platform published a new native-window generation",
            nullptr);
    }

    mutable std::recursive_mutex mutex_;
    MobileRendererSelectorConfig selectorConfig_;
    EventCallback eventCallback_;
    SelectionCallback selectionCallback_;
    HardwareFrameFallbackCallback hardwareFallbackCallback_;
    std::shared_ptr<VideoRenderAPI> renderer_;
    VideoRenderConfig renderConfig_;
    VideoRenderEvent pendingEvent_;
    MobileRenderAPI selectedAPI_ = MobileRenderAPI::None;
    std::uint64_t sessionGeneration_ = 0;
    std::uint64_t candidateGeneration_ = 0;
    bool sessionConfigured_ = false;
    bool suspended_ = false;
    bool vulkanRetired_ = false;
    bool hasPendingEvent_ = false;
    MobileHardwareFrameFallbackRoute hardwareFallbackRoute_ =
        MobileHardwareFrameFallbackRoute::None;
    HardwareDeviceType retiredHardwareDevice_ =
        HardwareDeviceType::Unknown;
    NativeHandle retiredHardwareSurface_;
    std::string lastError_;
    VideoRenderAttemptResult lastAttempt_;
};

MobileVideoRendererSelector::MobileVideoRendererSelector(
    MobileRendererSelectorConfig config)
    : impl_(std::make_unique<Impl>(std::move(config)))
{
}

MobileVideoRendererSelector::~MobileVideoRendererSelector() = default;
MobileVideoRendererSelector::MobileVideoRendererSelector(
    MobileVideoRendererSelector&&) noexcept = default;
MobileVideoRendererSelector&
MobileVideoRendererSelector::operator=(
    MobileVideoRendererSelector&&) noexcept = default;

VideoRenderCapabilities
MobileVideoRendererSelector::capabilities() const
{
    if (!impl_) {
        return {};
    }
    std::lock_guard<std::recursive_mutex> lock(impl_->mutex_);
    return impl_->renderer_
        ? impl_->renderer_->capabilities()
        : VideoRenderCapabilities {};
}

void MobileVideoRendererSelector::setEventCallback(
    EventCallback callback)
{
    if (!impl_) {
        return;
    }
    std::lock_guard<std::recursive_mutex> lock(impl_->mutex_);
    impl_->eventCallback_ = std::move(callback);
}

bool MobileVideoRendererSelector::open(
    const VideoRenderConfig& config)
{
    return impl_ && impl_->open(config);
}

bool MobileVideoRendererSelector::configure(
    const VideoRenderConfig& config)
{
    return impl_ && impl_->configure(config);
}

bool MobileVideoRendererSelector::render(
    const VideoFrame& frame)
{
    return renderDetailed(frame).frameConsumed();
}

VideoRenderAttemptResult
MobileVideoRendererSelector::renderDetailed(
    const VideoFrame& frame)
{
    if (!impl_) {
        return {
            VideoRenderAttemptStatus::FatalError,
            0,
            "The mobile renderer selector is unavailable",
        };
    }
    return impl_->renderDetailed(frame);
}

void MobileVideoRendererSelector::close() noexcept
{
    if (impl_) {
        impl_->close();
    }
}

void MobileVideoRendererSelector::setSelectionCallback(
    SelectionCallback callback)
{
    if (!impl_) {
        return;
    }
    std::lock_guard<std::recursive_mutex> lock(impl_->mutex_);
    impl_->selectionCallback_ = std::move(callback);
}

void MobileVideoRendererSelector::setHardwareFrameFallbackCallback(
    HardwareFrameFallbackCallback callback)
{
    if (!impl_) {
        return;
    }
    std::lock_guard<std::recursive_mutex> lock(impl_->mutex_);
    impl_->hardwareFallbackCallback_ = std::move(callback);
}

void MobileVideoRendererSelector::suspendSurface() noexcept
{
    if (impl_) {
        impl_->suspendSurface();
    }
}

bool MobileVideoRendererSelector::recreateSurface()
{
    return impl_ && impl_->recreateSurface();
}

MobileRenderAPI
MobileVideoRendererSelector::selectedAPI() const noexcept
{
    if (!impl_) {
        return MobileRenderAPI::None;
    }
    std::lock_guard<std::recursive_mutex> lock(impl_->mutex_);
    return impl_->selectedAPI_;
}

bool MobileVideoRendererSelector::presentationAvailable() const noexcept
{
    if (!impl_) {
        return false;
    }
    std::lock_guard<std::recursive_mutex> lock(impl_->mutex_);
    return static_cast<bool>(impl_->renderer_) && !impl_->suspended_;
}

bool MobileVideoRendererSelector::usingFallback() const noexcept
{
    if (!impl_) {
        return false;
    }
    std::lock_guard<std::recursive_mutex> lock(impl_->mutex_);
    return impl_->selectedAPI_ == MobileRenderAPI::OpenGLES
        && impl_->vulkanRetired_;
}

MobileHardwareFrameFallbackRoute
MobileVideoRendererSelector::hardwareFrameFallbackRoute() const noexcept
{
    if (!impl_) {
        return MobileHardwareFrameFallbackRoute::None;
    }
    std::lock_guard<std::recursive_mutex> lock(impl_->mutex_);
    return impl_->hardwareFallbackRoute_;
}

std::uint64_t
MobileVideoRendererSelector::sessionGeneration() const noexcept
{
    if (!impl_) {
        return 0;
    }
    std::lock_guard<std::recursive_mutex> lock(impl_->mutex_);
    return impl_->sessionGeneration_;
}

std::string MobileVideoRendererSelector::lastError() const
{
    if (!impl_) {
        return "The mobile renderer selector is unavailable";
    }
    std::lock_guard<std::recursive_mutex> lock(impl_->mutex_);
    return impl_->lastError_;
}

} // namespace qtav
