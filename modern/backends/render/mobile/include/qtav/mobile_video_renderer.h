// SPDX-License-Identifier: LGPL-2.1-or-later
#pragma once

#include <cstddef>
#include <cstdint>
#include <functional>
#include <memory>
#include <string>

#include <qtav/mobile_export.h>
#include <qtav/video_render_api.h>

namespace qtav {

enum class MobileRenderAPI {
    None,
    Vulkan,
    OpenGLES,
};

QTAV_RENDER_MOBILE_EXPORT const char*
mobileRenderAPIName(MobileRenderAPI api) noexcept;

struct QTAV_RENDER_MOBILE_EXPORT MobileRendererCandidate {
    std::shared_ptr<VideoRenderAPI> renderer;
    std::string detail;

    explicit operator bool() const noexcept
    {
        return static_cast<bool>(renderer);
    }
};

using MobileRendererFactory =
    std::function<MobileRendererCandidate()>;

struct QTAV_RENDER_MOBILE_EXPORT MobileRendererSelectorConfig {
    MobileRendererFactory vulkan;
    MobileRendererFactory openGLES;

    // Number of complete same-API renderer recreations attempted after a
    // recoverable surface failure before Vulkan is retired or OpenGL ES is
    // reported unavailable. A fatal renderer error bypasses these attempts.
    std::size_t maximumRecoveryAttempts = 2;
};

enum class MobileRendererSelectionEventType {
    Selected,
    Recovered,
    FellBack,
    Unavailable,
};

struct QTAV_RENDER_MOBILE_EXPORT MobileRendererSelectionEvent {
    MobileRendererSelectionEventType type =
        MobileRendererSelectionEventType::Selected;
    MobileRenderAPI previousAPI = MobileRenderAPI::None;
    MobileRenderAPI selectedAPI = MobileRenderAPI::None;
    std::uint64_t sessionGeneration = 0;
    std::string detail;
};

// Platform-neutral mobile renderer policy. Platform factories create fully
// prepared Vulkan or OpenGL ES VideoRenderAPI adapters for the current native
// window generation. The selector prefers Vulkan for each new open session,
// performs bounded same-API recreation for SurfaceLost, retires Vulkan after
// a fatal or repeatedly unrecoverable failure, and never probes it again until
// close() followed by a new open().
class QTAV_RENDER_MOBILE_EXPORT
MobileVideoRendererSelector final : public VideoRenderAPI {
public:
    using SelectionCallback =
        std::function<void(const MobileRendererSelectionEvent&)>;

    explicit MobileVideoRendererSelector(
        MobileRendererSelectorConfig config);
    ~MobileVideoRendererSelector() override;

    MobileVideoRendererSelector(
        MobileVideoRendererSelector&&) noexcept;
    MobileVideoRendererSelector& operator=(
        MobileVideoRendererSelector&&) noexcept;
    MobileVideoRendererSelector(
        const MobileVideoRendererSelector&) = delete;
    MobileVideoRendererSelector& operator=(
        const MobileVideoRendererSelector&) = delete;

    VideoRenderCapabilities capabilities() const override;
    void setEventCallback(EventCallback callback) override;
    bool open(const VideoRenderConfig& config) override;
    bool configure(const VideoRenderConfig& config) override;
    bool render(const VideoFrame& frame) override;
    void close() noexcept override;

    void setSelectionCallback(SelectionCallback callback);

    // Called after the platform invalidates the active native-window
    // generation. This closes native graphics resources while preserving the
    // selected API and the current renderer session.
    void suspendSurface() noexcept;

    // Called after the platform publishes a new current native-window
    // generation to the factories. It recreates the selected API first and
    // applies the same bounded, one-way fallback policy as render().
    bool recreateSurface();

    MobileRenderAPI selectedAPI() const noexcept;
    bool presentationAvailable() const noexcept;
    bool usingFallback() const noexcept;
    std::uint64_t sessionGeneration() const noexcept;
    std::string lastError() const;

private:
    class Impl;
    std::unique_ptr<Impl> impl_;
};

} // namespace qtav
