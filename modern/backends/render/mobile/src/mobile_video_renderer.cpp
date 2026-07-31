// SPDX-License-Identifier: LGPL-2.1-or-later

#include <qtav/mobile_video_renderer.h>

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
        if (renderer_->render(frame)) {
            clearPendingEvent();
            return true;
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
        closeRenderer();
        selectedAPI_ = MobileRenderAPI::None;
        vulkanRetired_ = true;

        std::string openGLError;
        if (!activate(MobileRenderAPI::OpenGLES, openGLError)) {
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

    bool render(const VideoFrame& frame)
    {
        std::lock_guard<std::recursive_mutex> lock(mutex_);
        if (!sessionConfigured_ || suspended_) {
            return false;
        }
        const MobileRenderAPI api = selectedAPI_;
        VideoRenderEvent failure;
        if (renderOnce(frame, failure)) {
            return true;
        }
        if (api == MobileRenderAPI::None) {
            return false;
        }
        return processFailure(api, std::move(failure), &frame);
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
    std::string lastError_;
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
    return impl_ && impl_->render(frame);
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
