// SPDX-License-Identifier: LGPL-2.1-or-later
#pragma once

#include <vulkan/vulkan.h>

#include <cstddef>
#include <cstdint>
#include <functional>
#include <memory>
#include <string>

#include <qtav/video_render_api.h>
#include <qtav/vulkan_export.h>

namespace qtav {

// PreferHdr falls back to SDR, RequireHdr returns no selection when the
// surface exposes no implemented HDR pair, and SdrOnly never selects HDR.
enum class VulkanOutputPreference {
    PreferHdr,
    RequireHdr,
    SdrOnly,
};

struct QTAV_RENDER_VULKAN_EXPORT BorrowedVulkanDevice {
    VkInstance instance = VK_NULL_HANDLE;
    VkPhysicalDevice physicalDevice = VK_NULL_HANDLE;
    VkDevice device = VK_NULL_HANDLE;
    VkQueue queue = VK_NULL_HANDLE;
    std::uint32_t queueFamilyIndex = 0;
    // libplacebo 7 requires these Vulkan 1.2 features to have been enabled
    // when the borrowed logical device was created.
    bool timelineSemaphoreEnabled = false;
    bool hostQueryResetEnabled = false;

    bool isValid() const noexcept;
};

// The image, view, and synchronization objects remain application-owned. They
// must stay valid until the submission fence associated with render() signals.
struct QTAV_RENDER_VULKAN_EXPORT VulkanRenderTarget {
    VkImage image = VK_NULL_HANDLE;
    VkImageView imageView = VK_NULL_HANDLE;
    VkFormat format = VK_FORMAT_UNDEFINED;
    // Defines the encoding written by libplacebo, not just display metadata.
    // The format/color-space pair must be supported by the renderer.
    VkColorSpaceKHR colorSpace = VK_COLOR_SPACE_SRGB_NONLINEAR_KHR;
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
    bool isHdr() const noexcept;
};

QTAV_RENDER_VULKAN_EXPORT bool vulkanColorSpaceIsHdr(
    VkColorSpaceKHR colorSpace) noexcept;

// Selects only format/color-space pairs whose render-target encoding is
// implemented by VulkanVideoRenderer. An undefined format reports that the
// requested preference cannot be satisfied.
QTAV_RENDER_VULKAN_EXPORT VkSurfaceFormatKHR selectVulkanSurfaceFormat(
    const VkSurfaceFormatKHR* formats,
    std::size_t count,
    VulkanOutputPreference preference) noexcept;

using VulkanCurrentTargetCallback = std::function<VulkanRenderTarget()>;

enum class VulkanHardwareImportStatus {
    Ready,
    Pending,
    Unsupported,
    Stale,
    Error,
};

struct QTAV_RENDER_VULKAN_EXPORT VulkanNormalizedSourceRect {
    float left = 0.0F;
    float top = 0.0F;
    float right = 1.0F;
    float bottom = 1.0F;

    bool isValid() const noexcept;
};

// A backend-specific, reference-counted view of a sampled image imported from
// a hardware decoder. The image, view, sampler, and synchronization objects
// remain valid while this object is alive. The renderer retains the object
// until its submission fence completes.
class QTAV_RENDER_VULKAN_EXPORT VulkanTextureFrame {
public:
    virtual ~VulkanTextureFrame();

    virtual int width() const noexcept = 0;
    virtual int height() const noexcept = 0;
    virtual VkImage image() const noexcept = 0;
    virtual VkImageView imageView() const noexcept = 0;
    virtual VkSampler sampler() const noexcept = 0;
    // Optional view/sampler pair which exposes unconverted YCbCr through
    // Vulkan's RGB_IDENTITY convention: R/G/B contain Cr/Y/Cb after the
    // driver-provided component mapping. The shared external normalizer
    // converts this once into Y/Cb/Cr for Dolby Vision RPU reshaping before
    // libplacebo performs color conversion. Implementations must not preapply
    // that final component rotation.
    virtual VkImageView unconvertedImageView() const noexcept;
    virtual VkSampler unconvertedSampler() const noexcept;
    // Optional ownership token for a sampler/conversion whose lifetime is
    // independent of the decoded image allocation. Returning a token lets
    // the external-image normalization pipeline retain immutable-sampler
    // state without retaining the producer buffer itself.
    virtual std::shared_ptr<void> samplerLifetime(
        VkSampler sampler) const noexcept;
    virtual VkFormat format() const noexcept;
    virtual VkImageUsageFlags usage() const noexcept;
    virtual VkSemaphore acquireSemaphore() const noexcept = 0;
    virtual VkSemaphore releaseSemaphore() const noexcept = 0;
    virtual VkImageLayout initialLayout() const noexcept;
    virtual VkImageLayout sampledLayout() const noexcept;
    virtual VkImageLayout releaseLayout() const noexcept;
    virtual std::uint32_t sourceQueueFamilyIndex() const noexcept;
    // The decoded image can occupy a cropped region of a larger native
    // allocation. Coordinates are normalized against that allocation.
    virtual VulkanNormalizedSourceRect normalizedSourceRect() const noexcept;

    // Queue the imported producer fence on the borrowed graphics queue. The
    // next libplacebo submission is then ordered after decoder completion.
    virtual bool waitForProducer(std::string& detail) noexcept;

    // Called immediately after a successful queue submission. Android
    // implementations export the signalled release semaphore as a sync fd and
    // return it through AImage_deleteAsync(). If export fails, the frame stays
    // retained and is released synchronously after the Vulkan fence completes.
    virtual void releaseToProducer() noexcept = 0;
};

struct QTAV_RENDER_VULKAN_EXPORT VulkanHardwareImportResult {
    VulkanHardwareImportStatus status =
        VulkanHardwareImportStatus::Unsupported;
    std::shared_ptr<VulkanTextureFrame> texture;
    std::string detail;

    explicit operator bool() const noexcept
    {
        return status == VulkanHardwareImportStatus::Ready
            && static_cast<bool>(texture);
    }
};

// Implemented by an optional platform interop target. Import must not map,
// transfer, stage, or re-upload decoded pixels through CPU memory.
class QTAV_RENDER_VULKAN_EXPORT VulkanHardwareFrameInterop {
public:
    using FrameAvailableCallback = std::function<void()>;

    virtual ~VulkanHardwareFrameInterop();

    virtual BorrowedVulkanDevice device() const noexcept = 0;
    virtual HardwareInteropCapabilities capabilities() const = 0;
    virtual bool supports(const HardwareFrame& frame) const noexcept = 0;
    // Starts producer release and reports whether the timestamp-correlated
    // native image is ready. Ready does not consume/import the image.
    virtual VulkanHardwareImportStatus prepareFrame(
        const VideoFrame& frame,
        std::string& detail) = 0;
    virtual VulkanHardwareImportResult importFrame(
        const VideoFrame& frame) = 0;
    virtual void setFrameAvailableCallback(
        FrameAvailableCallback callback) = 0;
    // Cancels producer/image associations which have not entered a Vulkan
    // submission. This is a non-blocking presentation-generation boundary;
    // submitted GPU work keeps its normal retained lifetime.
    virtual void invalidatePendingFrames() noexcept;
    virtual void completePendingFrameInvalidation() noexcept;
};

class QTAV_RENDER_VULKAN_EXPORT VulkanVideoRenderer final
    : public VideoRenderAPI {
public:
    static constexpr std::size_t FramesInFlight = 3;

    VulkanVideoRenderer(
        BorrowedVulkanDevice device,
        VulkanCurrentTargetCallback currentTarget,
        std::shared_ptr<VulkanHardwareFrameInterop> hardwareInterop = {});
    ~VulkanVideoRenderer() override;

    VulkanVideoRenderer(VulkanVideoRenderer&&) noexcept;
    VulkanVideoRenderer& operator=(VulkanVideoRenderer&&) noexcept;
    VulkanVideoRenderer(const VulkanVideoRenderer&) = delete;
    VulkanVideoRenderer& operator=(const VulkanVideoRenderer&) = delete;

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

    BorrowedVulkanDevice device() const noexcept;
    void setCurrentTargetCallback(VulkanCurrentTargetCallback callback);
    // Rotates the image into a presentation target whose platform transform
    // applies the inverse rotation. Unlike VideoRenderConfig::rotation, this
    // does not change the displayed source aspect ratio.
    void setPresentationRotation(VideoRotation rotation);
    VulkanHardwareImportStatus prepareHardwareFrame(
        const VideoFrame& frame,
        std::string* detail = nullptr);
    void setHardwareFrameInterop(
        std::shared_ptr<VulkanHardwareFrameInterop> hardwareInterop);
    std::shared_ptr<VulkanHardwareFrameInterop>
    hardwareFrameInterop() const noexcept;

private:
    class Impl;
    std::unique_ptr<Impl> impl_;
};

} // namespace qtav
