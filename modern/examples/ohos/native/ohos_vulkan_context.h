// SPDX-License-Identifier: LGPL-2.1-or-later
#pragma once

#include <native_window/external_window.h>
#include <vulkan/vulkan.h>

#include <cstdint>
#include <string>

#include <qtav/ohos_vulkan_video_renderer.h>

namespace qtav::ohos_example {

class OHOSVulkanContext final {
public:
    OHOSVulkanContext() = default;
    ~OHOSVulkanContext();

    OHOSVulkanContext(const OHOSVulkanContext&) = delete;
    OHOSVulkanContext& operator=(const OHOSVulkanContext&) = delete;

    bool create(OHNativeWindow* window, std::string& error);
    void reset() noexcept;

    BorrowedOHOSVulkanContext borrowed() const noexcept;
    bool nativeBufferExternalMemoryEnabled() const noexcept;
    bool foreignQueueFamilyEnabled() const noexcept;
    bool syncFdSemaphoreEnabled() const noexcept;
    std::string description() const;

private:
    VkInstance instance_ = VK_NULL_HANDLE;
    VkPhysicalDevice physicalDevice_ = VK_NULL_HANDLE;
    VkDevice device_ = VK_NULL_HANDLE;
    VkQueue queue_ = VK_NULL_HANDLE;
    std::uint32_t queueFamilyIndex_ = 0;
    bool hdrMetadataEnabled_ = false;
    bool nativeBufferExternalMemoryEnabled_ = false;
    bool foreignQueueFamilyEnabled_ = false;
    bool syncFdSemaphoreEnabled_ = false;
    std::string deviceName_;
};

} // namespace qtav::ohos_example
