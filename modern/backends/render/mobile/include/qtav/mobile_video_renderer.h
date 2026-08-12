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

    // Applications may expose this as a user preference. Vulkan remains the
    // default; OpenGLES tries OpenGL ES first and uses Vulkan only when the
    // preferred backend cannot be opened for the new session.
    MobileRenderAPI preferredAPI = MobileRenderAPI::Vulkan;
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

// A hardware frame produced for one graphics API's native interop surface
// cannot be retried through another API. When Vulkan is retired while such a
// frame is current, the application selects one explicit route for subsequent
// decoder output. The current frame and any late frames from its retired
// surface are discarded without CPU mapping.
enum class MobileHardwareFrameFallbackRoute {
    None,
    OpenGLESInterop,
    DirectSurface,
    SoftwareDecode,
    NoVideo,
};

QTAV_RENDER_MOBILE_EXPORT const char*
mobileHardwareFrameFallbackRouteName(
    MobileHardwareFrameFallbackRoute route) noexcept;

struct QTAV_RENDER_MOBILE_EXPORT MobileHardwareFrameFallbackEvent {
    MobileRenderAPI previousAPI = MobileRenderAPI::None;
    MobileRenderAPI selectedAPI = MobileRenderAPI::None;
    HardwareDeviceType sourceDevice = HardwareDeviceType::Unknown;
    std::uint32_t sourceSurfaceGeneration = 0;
    std::uint64_t sessionGeneration = 0;
    std::string detail;
};

struct QTAV_RENDER_MOBILE_EXPORT MobileHardwareFrameFallbackDecision {
    MobileHardwareFrameFallbackRoute route =
        MobileHardwareFrameFallbackRoute::None;
    std::string detail;
};

// Platform-neutral mobile renderer policy. Platform factories create fully
// prepared Vulkan or OpenGL ES VideoRenderAPI adapters for the current native
// window generation. The selector starts with the configured preferred API,
// performs bounded same-API recreation for SurfaceLost, retires Vulkan after
// a fatal or repeatedly unrecoverable failure, and never probes it again until
// close() followed by a new open().
class QTAV_RENDER_MOBILE_EXPORT
MobileVideoRendererSelector final : public VideoRenderAPI {
public:
    using SelectionCallback =
        std::function<void(const MobileRendererSelectionEvent&)>;
    using HardwareFrameFallbackCallback =
        std::function<MobileHardwareFrameFallbackDecision(
            const MobileHardwareFrameFallbackEvent&)>;

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
    VideoRenderAttemptResult renderDetailed(
        const VideoFrame& frame) override;
    bool render(const VideoFrame& frame) override;
    void close() noexcept override;
    void invalidatePendingFrames() noexcept override;

    void setSelectionCallback(SelectionCallback callback);
    // Invoked synchronously after Vulkan is retired for a current hardware
    // frame, and again if the replacement OpenGL ES hardware interop later
    // fails. OpenGLESInterop requires the prepared OpenGL ES candidate to
    // advertise the source device and asks the callback to reconfigure future
    // decoder output for that interop surface. SoftwareDecode keeps OpenGL ES
    // active for future software frames. This permits the application to apply
    // an automatic Vulkan -> OpenGL ES interop -> software-decode chain without
    // coupling renderer policy to decoder control. DirectSurface hands
    // presentation back to the application, while NoVideo deliberately
    // discards future video. Returning None, or omitting this callback, reports
    // presentation unavailable. The callback may request thread-safe Player
    // control changes but must not destroy this selector.
    void setHardwareFrameFallbackCallback(
        HardwareFrameFallbackCallback callback);

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
    MobileHardwareFrameFallbackRoute
    hardwareFrameFallbackRoute() const noexcept;
    std::uint64_t sessionGeneration() const noexcept;
    std::string lastError() const;

private:
    class Impl;
    std::unique_ptr<Impl> impl_;
};

} // namespace qtav
