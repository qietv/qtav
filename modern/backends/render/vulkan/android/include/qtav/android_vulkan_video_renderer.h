// SPDX-License-Identifier: LGPL-2.1-or-later
#pragma once

#if !defined(__ANDROID__)
#  error "qtav/android_vulkan_video_renderer.h is Android-only"
#endif

#include <android/native_window.h>
#include <vulkan/vulkan.h>

#include <memory>

#include <qtav/android_vulkan_export.h>
#include <qtav/vulkan_video_renderer.h>

namespace qtav {

struct QTAV_RENDER_VULKAN_ANDROID_EXPORT
BorrowedAndroidVulkanContext {
    VkInstance instance = VK_NULL_HANDLE;
    BorrowedVulkanDevice device;
    // True only when VK_EXT_hdr_metadata was enabled at VkDevice creation.
    bool hdrMetadataEnabled = false;

    bool isValid() const noexcept;
};

// Android owns the NativeActivity and publishes its current ANativeWindow.
// This adapter retains the active window generation and owns only the
// VkSurfaceKHR, swapchain, image views, and acquire/present synchronization.
// The Vulkan instance, physical/logical device, and queue remain borrowed.
class QTAV_RENDER_VULKAN_ANDROID_EXPORT
AndroidVulkanVideoRenderer final : public VideoRenderAPI {
public:
    explicit AndroidVulkanVideoRenderer(
        BorrowedAndroidVulkanContext context,
        VulkanOutputPreference outputPreference =
            VulkanOutputPreference::PreferHdr);
    ~AndroidVulkanVideoRenderer() override;

    AndroidVulkanVideoRenderer(AndroidVulkanVideoRenderer&&) noexcept;
    AndroidVulkanVideoRenderer& operator=(
        AndroidVulkanVideoRenderer&&) noexcept;
    AndroidVulkanVideoRenderer(const AndroidVulkanVideoRenderer&) = delete;
    AndroidVulkanVideoRenderer& operator=(
        const AndroidVulkanVideoRenderer&) = delete;

    VideoRenderCapabilities capabilities() const override;
    void setEventCallback(EventCallback callback) override;
    bool open(const VideoRenderConfig& config) override;
    bool configure(const VideoRenderConfig& config) override;
    bool render(const VideoFrame& frame) override;
    void close() noexcept override;

    // The adapter acquires its own ANativeWindow reference. Republishing the
    // same window after its buffer geometry changes refreshes the swapchain;
    // passing nullptr invalidates the current surface generation.
    bool setWindow(ANativeWindow* window);
    VideoSize surfaceSize() const noexcept;
    VkSurfaceFormatKHR surfaceFormat() const noexcept;
    bool hdrOutputActive() const noexcept;
    BorrowedAndroidVulkanContext context() const noexcept;
    void setHardwareFrameInterop(
        std::shared_ptr<VulkanHardwareFrameInterop> hardwareInterop);
    std::shared_ptr<VulkanHardwareFrameInterop>
    hardwareFrameInterop() const noexcept;

private:
    class Impl;
    std::unique_ptr<Impl> impl_;
};

} // namespace qtav
