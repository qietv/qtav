// SPDX-License-Identifier: LGPL-2.1-or-later
#pragma once

#include <android/native_window.h>
#include <vulkan/vulkan.h>

#include <cstdint>
#include <string>

#include <qtav/android_vulkan_video_renderer.h>

namespace qtav::android_player {

class AndroidVulkanContext final {
public:
    AndroidVulkanContext() = default;
    ~AndroidVulkanContext();

    AndroidVulkanContext(const AndroidVulkanContext&) = delete;
    AndroidVulkanContext& operator=(const AndroidVulkanContext&) = delete;

    bool create(
        ANativeWindow* window,
        bool requireMediaCodecInterop,
        bool enableDebugLayer,
        std::string& error);
    void reset() noexcept;

    BorrowedAndroidVulkanContext borrowed() const noexcept;
    bool mediaCodecInteropEnabled() const noexcept;
    bool debugLayerEnabled() const noexcept;
    std::string description() const;

private:
    VkInstance instance_ = VK_NULL_HANDLE;
    VkDebugUtilsMessengerEXT debugMessenger_ = VK_NULL_HANDLE;
    VkPhysicalDevice physicalDevice_ = VK_NULL_HANDLE;
    VkDevice device_ = VK_NULL_HANDLE;
    VkQueue queue_ = VK_NULL_HANDLE;
    std::uint32_t queueFamilyIndex_ = 0;
    bool hdrMetadataEnabled_ = false;
    bool androidHardwareBufferEnabled_ = false;
    bool externalSemaphoreFdEnabled_ = false;
    bool samplerYcbcrConversionEnabled_ = false;
    bool foreignQueueFamilyEnabled_ = false;
    bool debugLayerEnabled_ = false;
    std::string deviceName_;
};

} // namespace qtav::android_player
