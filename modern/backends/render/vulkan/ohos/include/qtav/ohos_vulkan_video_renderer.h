// SPDX-License-Identifier: LGPL-2.1-or-later
#pragma once

#if !defined(__OHOS__)
#  error "qtav/ohos_vulkan_video_renderer.h is OHOS-only"
#endif

#include <native_window/external_window.h>
#include <vulkan/vulkan.h>

#include <memory>

#include <qtav/ohos_vulkan_export.h>
#include <qtav/vulkan_video_renderer.h>

namespace qtav {

struct QTAV_RENDER_VULKAN_OHOS_EXPORT BorrowedOHOSVulkanContext {
    VkInstance instance = VK_NULL_HANDLE;
    BorrowedVulkanDevice device;
    // True only when VK_EXT_hdr_metadata was enabled at VkDevice creation.
    bool hdrMetadataEnabled = false;
    // True only when VK_EXT_swapchain_colorspace was enabled at instance
    // creation. Some OHOS WSI versions accept native HDR color spaces at
    // swapchain creation without advertising them from the surface query.
    // Kept after the original fields so existing aggregate initialization
    // retains its meaning.
    bool swapchainColorSpaceEnabled = false;

    bool isValid() const noexcept;
};

// ArkUI owns the XComponent and publishes its current OHNativeWindow. This
// adapter retains the active window generation and owns only its
// VkSurfaceKHR, swapchain, image views, and acquire/present synchronization.
// The Vulkan instance, physical/logical device, and queue remain borrowed.
class QTAV_RENDER_VULKAN_OHOS_EXPORT OHOSVulkanVideoRenderer final
    : public VideoRenderAPI {
public:
    explicit OHOSVulkanVideoRenderer(
        BorrowedOHOSVulkanContext context,
        VulkanOutputPreference outputPreference =
            VulkanOutputPreference::PreferHdr);
    ~OHOSVulkanVideoRenderer() override;

    OHOSVulkanVideoRenderer(OHOSVulkanVideoRenderer&&) noexcept;
    OHOSVulkanVideoRenderer& operator=(
        OHOSVulkanVideoRenderer&&) noexcept;
    OHOSVulkanVideoRenderer(const OHOSVulkanVideoRenderer&) = delete;
    OHOSVulkanVideoRenderer& operator=(
        const OHOSVulkanVideoRenderer&) = delete;

    VideoRenderCapabilities capabilities() const override;
    void setEventCallback(EventCallback callback) override;
    bool open(const VideoRenderConfig& config) override;
    bool configure(const VideoRenderConfig& config) override;
    VideoRenderAttemptResult renderDetailed(
        const VideoFrame& frame) override;
    bool render(const VideoFrame& frame) override;
    void close() noexcept override;
    void invalidatePendingFrames() noexcept override;
    void completePendingFrameInvalidation() noexcept override;

    // The adapter acquires its own OHNativeWindow reference. Republishing the
    // same window after an XComponent size change refreshes the swapchain;
    // passing nullptr invalidates the current surface generation.
    bool setWindow(OHNativeWindow* window);
    VideoSize surfaceSize() const noexcept;
    VkSurfaceFormatKHR surfaceFormat() const noexcept;
    bool hdrOutputActive() const noexcept;
    BorrowedOHOSVulkanContext context() const noexcept;
    void setHardwareFrameInterop(
        std::shared_ptr<VulkanHardwareFrameInterop> hardwareInterop);
    std::shared_ptr<VulkanHardwareFrameInterop>
    hardwareFrameInterop() const noexcept;

private:
    class Impl;
    std::unique_ptr<Impl> impl_;
};

} // namespace qtav
