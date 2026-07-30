// SPDX-License-Identifier: LGPL-2.1-or-later
#pragma once

#include <vulkan/vulkan.h>

#include <cstdint>
#include <functional>
#include <memory>

#include <qtav/video_render_api.h>
#include <qtav/vulkan_export.h>

namespace qtav {

struct QTAV_RENDER_VULKAN_EXPORT BorrowedVulkanDevice {
    VkPhysicalDevice physicalDevice = VK_NULL_HANDLE;
    VkDevice device = VK_NULL_HANDLE;
    VkQueue queue = VK_NULL_HANDLE;
    std::uint32_t queueFamilyIndex = 0;

    bool isValid() const noexcept;
};

// The image, view, and synchronization objects remain application-owned. They
// must stay valid until the submission fence associated with render() signals.
// The renderer serializes one retained frame at a time in this first engine
// checkpoint.
struct QTAV_RENDER_VULKAN_EXPORT VulkanRenderTarget {
    VkImage image = VK_NULL_HANDLE;
    VkImageView imageView = VK_NULL_HANDLE;
    VkFormat format = VK_FORMAT_UNDEFINED;
    VkExtent2D extent {};
    VkImageLayout finalLayout = VK_IMAGE_LAYOUT_PRESENT_SRC_KHR;
    VkSemaphore waitSemaphore = VK_NULL_HANDLE;
    VkPipelineStageFlags waitStage =
        VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT;
    // Optional for offscreen targets; swapchain adapters normally provide it.
    VkSemaphore signalSemaphore = VK_NULL_HANDLE;
    std::uint64_t generation = 0;
    // Intended for deterministic offscreen tests and diagnostics.
    bool waitUntilCompleted = false;

    bool isValid() const noexcept;
};

using VulkanCurrentTargetCallback = std::function<VulkanRenderTarget()>;

class QTAV_RENDER_VULKAN_EXPORT VulkanVideoRenderer final
    : public VideoRenderAPI {
public:
    VulkanVideoRenderer(
        BorrowedVulkanDevice device,
        VulkanCurrentTargetCallback currentTarget);
    ~VulkanVideoRenderer() override;

    VulkanVideoRenderer(VulkanVideoRenderer&&) noexcept;
    VulkanVideoRenderer& operator=(VulkanVideoRenderer&&) noexcept;
    VulkanVideoRenderer(const VulkanVideoRenderer&) = delete;
    VulkanVideoRenderer& operator=(const VulkanVideoRenderer&) = delete;

    VideoRenderCapabilities capabilities() const override;
    void setEventCallback(EventCallback callback) override;
    bool open(const VideoRenderConfig& config) override;
    bool configure(const VideoRenderConfig& config) override;
    bool render(const VideoFrame& frame) override;
    void close() noexcept override;

    BorrowedVulkanDevice device() const noexcept;
    void setCurrentTargetCallback(VulkanCurrentTargetCallback callback);

private:
    class Impl;
    std::unique_ptr<Impl> impl_;
};

} // namespace qtav
