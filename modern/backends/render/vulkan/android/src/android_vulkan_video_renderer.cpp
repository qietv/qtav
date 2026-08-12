// SPDX-License-Identifier: LGPL-2.1-or-later

#include <qtav/android_vulkan_video_renderer.h>

#include <algorithm>
#include <array>
#include <cstdint>
#include <limits>
#include <mutex>
#include <string>
#include <utility>
#include <vector>

namespace qtav {
namespace {

const char* resultName(VkResult result) noexcept
{
    switch (result) {
    case VK_SUCCESS:
        return "VK_SUCCESS";
    case VK_SUBOPTIMAL_KHR:
        return "VK_SUBOPTIMAL_KHR";
    case VK_ERROR_OUT_OF_DATE_KHR:
        return "VK_ERROR_OUT_OF_DATE_KHR";
    case VK_ERROR_SURFACE_LOST_KHR:
        return "VK_ERROR_SURFACE_LOST_KHR";
    case VK_ERROR_DEVICE_LOST:
        return "VK_ERROR_DEVICE_LOST";
    default:
        return "unknown VkResult";
    }
}

std::string resultError(const char* operation, VkResult result)
{
    return std::string(operation) + " failed: " + resultName(result)
        + " (" + std::to_string(static_cast<int>(result)) + ')';
}

VkCompositeAlphaFlagBitsKHR compositeAlpha(
    VkCompositeAlphaFlagsKHR supported) noexcept
{
    const std::array<VkCompositeAlphaFlagBitsKHR, 4> choices {
        VK_COMPOSITE_ALPHA_OPAQUE_BIT_KHR,
        VK_COMPOSITE_ALPHA_PRE_MULTIPLIED_BIT_KHR,
        VK_COMPOSITE_ALPHA_POST_MULTIPLIED_BIT_KHR,
        VK_COMPOSITE_ALPHA_INHERIT_BIT_KHR,
    };
    for (const auto choice : choices) {
        if ((supported & choice) != 0U) {
            return choice;
        }
    }
    return VK_COMPOSITE_ALPHA_OPAQUE_BIT_KHR;
}

VkXYColorEXT xyColor(const Chromaticity& value) noexcept
{
    return {
        static_cast<float>(value.x),
        static_cast<float>(value.y),
    };
}

VkHdrMetadataEXT hdrMetadata(const VideoFrame& frame) noexcept
{
    const MasteringDisplayMetadata mastering =
        frame.masteringDisplayMetadata();
    const ContentLightMetadata content =
        frame.contentLightMetadata();
    VkHdrMetadataEXT result { VK_STRUCTURE_TYPE_HDR_METADATA_EXT };
    if (mastering.hasPrimaries) {
        result.displayPrimaryRed = xyColor(mastering.primaries[0]);
        result.displayPrimaryGreen = xyColor(mastering.primaries[1]);
        result.displayPrimaryBlue = xyColor(mastering.primaries[2]);
        result.whitePoint = xyColor(mastering.whitePoint);
    }
    if (mastering.hasLuminance) {
        result.maxLuminance =
            static_cast<float>(mastering.maximumLuminance);
        result.minLuminance =
            static_cast<float>(mastering.minimumLuminance);
    }
    result.maxContentLightLevel =
        static_cast<float>(content.maximumContentLightLevel);
    result.maxFrameAverageLightLevel =
        static_cast<float>(content.maximumFrameAverageLightLevel);
    return result;
}

} // namespace

bool BorrowedAndroidVulkanContext::isValid() const noexcept
{
    return instance != VK_NULL_HANDLE && device.isValid()
        && instance == device.instance;
}

class AndroidVulkanVideoRenderer::Impl {
public:
    struct FrameSync {
        VkSemaphore imageAvailable = VK_NULL_HANDLE;
        VkSemaphore renderFinished = VK_NULL_HANDLE;
    };

    Impl(
        BorrowedAndroidVulkanContext context,
        VulkanOutputPreference outputPreference)
        : context_(context)
        , outputPreference_(outputPreference)
        , renderer_(
              context.device,
              [this] { return activeTarget_; })
    {
        if (context_.hdrMetadataEnabled && context_.device.device) {
            setHdrMetadata_ =
                reinterpret_cast<PFN_vkSetHdrMetadataEXT>(
                    vkGetDeviceProcAddr(
                        context_.device.device,
                        "vkSetHdrMetadataEXT"));
        }
        renderer_.setEventCallback(
            [this](const VideoRenderEvent& event) {
                notify(event.type, event.detail);
            });
    }

    ~Impl()
    {
        close();
    }

    void notify(VideoRenderEventType type, std::string detail)
    {
        EventCallback callback;
        {
            std::lock_guard<std::recursive_mutex> lock(mutex_);
            callback = eventCallback_;
        }
        if (callback) {
            callback({ type, std::move(detail) });
        }
    }

    void destroySwapchain() noexcept
    {
        activeTarget_ = {};
        acquired_ = false;
        for (VkImageView view : views_) {
            vkDestroyImageView(context_.device.device, view, nullptr);
        }
        views_.clear();
        images_.clear();
        if (swapchain_) {
            vkDestroySwapchainKHR(
                context_.device.device,
                swapchain_,
                nullptr);
            swapchain_ = VK_NULL_HANDLE;
        }
        extent_ = {};
        format_ = VK_FORMAT_UNDEFINED;
        colorSpace_ = VK_COLOR_SPACE_SRGB_NONLINEAR_KHR;
    }

    void destroySurface() noexcept
    {
        destroySwapchain();
        if (surface_) {
            vkDestroySurfaceKHR(context_.instance, surface_, nullptr);
            surface_ = VK_NULL_HANDLE;
        }
    }

    bool createSemaphores(std::string& error)
    {
        const bool complete = std::all_of(
            sync_.begin(),
            sync_.end(),
            [](const FrameSync& sync) {
                return sync.imageAvailable && sync.renderFinished;
            });
        if (complete) {
            return true;
        }
        destroySemaphores();
        VkSemaphoreCreateInfo info {
            VK_STRUCTURE_TYPE_SEMAPHORE_CREATE_INFO,
        };
        for (auto& sync : sync_) {
            VkResult result = vkCreateSemaphore(
                context_.device.device,
                &info,
                nullptr,
                &sync.imageAvailable);
            if (result == VK_SUCCESS) {
                result = vkCreateSemaphore(
                    context_.device.device,
                    &info,
                    nullptr,
                    &sync.renderFinished);
            }
            if (result != VK_SUCCESS) {
                error = resultError("vkCreateSemaphore", result);
                return false;
            }
        }
        return true;
    }

    void destroySemaphores() noexcept
    {
        for (auto& sync : sync_) {
            if (sync.renderFinished) {
                vkDestroySemaphore(
                    context_.device.device,
                    sync.renderFinished,
                    nullptr);
            }
            if (sync.imageAvailable) {
                vkDestroySemaphore(
                    context_.device.device,
                    sync.imageAvailable,
                    nullptr);
            }
            sync = {};
        }
        syncIndex_ = 0;
    }

    FrameSync& activeSync() noexcept
    {
        return sync_[syncIndex_];
    }

    void advanceSync() noexcept
    {
        syncIndex_ = (syncIndex_ + 1) % sync_.size();
    }

    bool createSurface(std::string& error)
    {
        if (surface_) {
            return true;
        }
        if (!window_) {
            error = "The Android native window is unavailable";
            return false;
        }
        VkAndroidSurfaceCreateInfoKHR info {
            VK_STRUCTURE_TYPE_ANDROID_SURFACE_CREATE_INFO_KHR,
        };
        info.window = window_;
        const VkResult result = vkCreateAndroidSurfaceKHR(
            context_.instance,
            &info,
            nullptr,
            &surface_);
        if (result != VK_SUCCESS) {
            error = resultError("vkCreateAndroidSurfaceKHR", result);
            return false;
        }
        VkBool32 supported = VK_FALSE;
        const VkResult supportResult =
            vkGetPhysicalDeviceSurfaceSupportKHR(
                context_.device.physicalDevice,
                context_.device.queueFamilyIndex,
                surface_,
                &supported);
        if (supportResult != VK_SUCCESS || supported != VK_TRUE) {
            error = supportResult == VK_SUCCESS
                ? "The selected Vulkan queue cannot present to the Android surface"
                : resultError(
                      "vkGetPhysicalDeviceSurfaceSupportKHR",
                      supportResult);
            return false;
        }
        return true;
    }

    bool createSwapchain(std::string& error)
    {
        if (swapchain_) {
            return true;
        }
        if (context_.hdrMetadataEnabled && !setHdrMetadata_) {
            error =
                "VK_EXT_hdr_metadata was reported enabled but vkSetHdrMetadataEXT is unavailable";
            return false;
        }
        if (!createSurface(error) || !createSemaphores(error)) {
            return false;
        }

        VkSurfaceCapabilitiesKHR capabilities {};
        VkResult result = vkGetPhysicalDeviceSurfaceCapabilitiesKHR(
            context_.device.physicalDevice,
            surface_,
            &capabilities);
        if (result != VK_SUCCESS) {
            error = resultError(
                "vkGetPhysicalDeviceSurfaceCapabilitiesKHR",
                result);
            return false;
        }
        std::uint32_t formatCount = 0;
        result = vkGetPhysicalDeviceSurfaceFormatsKHR(
            context_.device.physicalDevice,
            surface_,
            &formatCount,
            nullptr);
        if (result != VK_SUCCESS || formatCount == 0) {
            error = result == VK_SUCCESS
                ? "The Android Vulkan surface exposes no formats"
                : resultError(
                      "vkGetPhysicalDeviceSurfaceFormatsKHR",
                      result);
            return false;
        }
        std::vector<VkSurfaceFormatKHR> formats(formatCount);
        result = vkGetPhysicalDeviceSurfaceFormatsKHR(
            context_.device.physicalDevice,
            surface_,
            &formatCount,
            formats.data());
        if (result != VK_SUCCESS) {
            error = resultError(
                "vkGetPhysicalDeviceSurfaceFormatsKHR",
                result);
            return false;
        }
        const VkSurfaceFormatKHR surfaceFormat =
            selectVulkanSurfaceFormat(
                formats.data(),
                formats.size(),
                outputPreference_);
        if (surfaceFormat.format == VK_FORMAT_UNDEFINED) {
            error = outputPreference_ == VulkanOutputPreference::RequireHdr
                ? "The Android Vulkan surface has no supported native HDR format/color-space pair"
                : "The Android Vulkan surface has no supported render-target format/color-space pair";
            return false;
        }

        VkExtent2D extent = capabilities.currentExtent;
        if (extent.width == std::numeric_limits<std::uint32_t>::max()) {
            extent.width = std::clamp(
                static_cast<std::uint32_t>(
                    std::max(1, ANativeWindow_getWidth(window_))),
                capabilities.minImageExtent.width,
                capabilities.maxImageExtent.width);
            extent.height = std::clamp(
                static_cast<std::uint32_t>(
                    std::max(1, ANativeWindow_getHeight(window_))),
                capabilities.minImageExtent.height,
                capabilities.maxImageExtent.height);
        }
        std::uint32_t imageCount = capabilities.minImageCount + 1;
        if (capabilities.maxImageCount > 0) {
            imageCount =
                std::min(imageCount, capabilities.maxImageCount);
        }
        VkSwapchainCreateInfoKHR swapchainInfo {
            VK_STRUCTURE_TYPE_SWAPCHAIN_CREATE_INFO_KHR,
        };
        swapchainInfo.surface = surface_;
        swapchainInfo.minImageCount = imageCount;
        swapchainInfo.imageFormat = surfaceFormat.format;
        swapchainInfo.imageColorSpace = surfaceFormat.colorSpace;
        swapchainInfo.imageExtent = extent;
        swapchainInfo.imageArrayLayers = 1;
        swapchainInfo.imageUsage = VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT;
        swapchainInfo.imageSharingMode = VK_SHARING_MODE_EXCLUSIVE;
        swapchainInfo.preTransform = capabilities.currentTransform;
        swapchainInfo.compositeAlpha =
            compositeAlpha(capabilities.supportedCompositeAlpha);
        swapchainInfo.presentMode = VK_PRESENT_MODE_FIFO_KHR;
        swapchainInfo.clipped = VK_TRUE;
        result = vkCreateSwapchainKHR(
            context_.device.device,
            &swapchainInfo,
            nullptr,
            &swapchain_);
        if (result != VK_SUCCESS) {
            error = resultError("vkCreateSwapchainKHR", result);
            return false;
        }

        std::uint32_t actualCount = 0;
        result = vkGetSwapchainImagesKHR(
            context_.device.device,
            swapchain_,
            &actualCount,
            nullptr);
        if (result != VK_SUCCESS || actualCount == 0) {
            error = result == VK_SUCCESS
                ? "The Android Vulkan swapchain exposes no images"
                : resultError("vkGetSwapchainImagesKHR", result);
            return false;
        }
        images_.resize(actualCount);
        result = vkGetSwapchainImagesKHR(
            context_.device.device,
            swapchain_,
            &actualCount,
            images_.data());
        if (result != VK_SUCCESS) {
            error = resultError("vkGetSwapchainImagesKHR", result);
            return false;
        }
        views_.reserve(images_.size());
        for (VkImage image : images_) {
            VkImageViewCreateInfo viewInfo {
                VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO,
            };
            viewInfo.image = image;
            viewInfo.viewType = VK_IMAGE_VIEW_TYPE_2D;
            viewInfo.format = surfaceFormat.format;
            viewInfo.components = {
                VK_COMPONENT_SWIZZLE_IDENTITY,
                VK_COMPONENT_SWIZZLE_IDENTITY,
                VK_COMPONENT_SWIZZLE_IDENTITY,
                VK_COMPONENT_SWIZZLE_IDENTITY,
            };
            viewInfo.subresourceRange.aspectMask =
                VK_IMAGE_ASPECT_COLOR_BIT;
            viewInfo.subresourceRange.levelCount = 1;
            viewInfo.subresourceRange.layerCount = 1;
            VkImageView view = VK_NULL_HANDLE;
            result = vkCreateImageView(
                context_.device.device,
                &viewInfo,
                nullptr,
                &view);
            if (result != VK_SUCCESS) {
                error = resultError("vkCreateImageView", result);
                return false;
            }
            views_.push_back(view);
        }
        extent_ = extent;
        format_ = surfaceFormat.format;
        colorSpace_ = surfaceFormat.colorSpace;
        ++generation_;
        return true;
    }

    bool recreate(std::string& error)
    {
        if (open_) {
            // The engine owns framebuffers that retain the current image
            // views. Close it before retiring a swapchain generation.
            renderer_.close();
            engineOpen_ = false;
        }
        const VkResult idle = vkDeviceWaitIdle(context_.device.device);
        if (idle != VK_SUCCESS) {
            error = resultError("vkDeviceWaitIdle", idle);
            return false;
        }
        destroySwapchain();
        syncIndex_ = 0;
        if (!createSwapchain(error)) {
            return false;
        }
        if (open_) {
            updateConfigForSurface();
            engineOpen_ = renderer_.open(config_);
            if (!engineOpen_) {
                error =
                    "The Vulkan engine rejected the recreated Android surface";
                return false;
            }
        }
        return true;
    }

    bool acquire(std::string& error)
    {
        if (!createSwapchain(error)) {
            return false;
        }
        if (open_ && !engineOpen_) {
            updateConfigForSurface();
            engineOpen_ = renderer_.open(config_);
            if (!engineOpen_) {
                error =
                    "The Vulkan engine could not reopen the Android surface";
                return false;
            }
        }
        VkResult result = vkAcquireNextImageKHR(
            context_.device.device,
            swapchain_,
            std::numeric_limits<std::uint64_t>::max(),
            activeSync().imageAvailable,
            VK_NULL_HANDLE,
            &imageIndex_);
        if (result == VK_ERROR_OUT_OF_DATE_KHR) {
            if (!recreate(error)) {
                return false;
            }
            result = vkAcquireNextImageKHR(
                context_.device.device,
                swapchain_,
                std::numeric_limits<std::uint64_t>::max(),
                activeSync().imageAvailable,
                VK_NULL_HANDLE,
                &imageIndex_);
        }
        if (result != VK_SUCCESS && result != VK_SUBOPTIMAL_KHR) {
            error = resultError("vkAcquireNextImageKHR", result);
            return false;
        }
        activeTarget_.image = images_[imageIndex_];
        activeTarget_.imageView = views_[imageIndex_];
        activeTarget_.format = format_;
        activeTarget_.colorSpace = colorSpace_;
        activeTarget_.extent = extent_;
        activeTarget_.finalLayout = VK_IMAGE_LAYOUT_PRESENT_SRC_KHR;
        activeTarget_.waitSemaphore = activeSync().imageAvailable;
        activeTarget_.signalSemaphore = activeSync().renderFinished;
        activeTarget_.generation = generation_;
        acquired_ = true;
        return true;
    }

    void applyHdrMetadata(const VideoFrame& frame) noexcept
    {
        if (!swapchain_ || !vulkanColorSpaceIsHdr(colorSpace_)
            || !setHdrMetadata_) {
            return;
        }
        const VkHdrMetadataEXT metadata = hdrMetadata(frame);
        setHdrMetadata_(
            context_.device.device,
            1,
            &swapchain_,
            &metadata);
    }

    void updateConfigForSurface() noexcept
    {
        config_.surfaceSize = {
            static_cast<int>(extent_.width),
            static_cast<int>(extent_.height),
        };
        config_.viewport = {};
        config_.rotation = applicationRotation_;
    }

    bool present(std::string& error)
    {
        VkPresentInfoKHR info {
            VK_STRUCTURE_TYPE_PRESENT_INFO_KHR,
        };
        info.waitSemaphoreCount = 1;
        info.pWaitSemaphores = &activeSync().renderFinished;
        info.swapchainCount = 1;
        info.pSwapchains = &swapchain_;
        info.pImageIndices = &imageIndex_;
        const VkResult result =
            vkQueuePresentKHR(context_.device.queue, &info);
        acquired_ = false;
        activeTarget_ = {};
        advanceSync();
        if (result == VK_ERROR_OUT_OF_DATE_KHR
            || result == VK_SUBOPTIMAL_KHR) {
            return recreate(error);
        }
        if (result != VK_SUCCESS) {
            error = resultError("vkQueuePresentKHR", result);
            return false;
        }
        return true;
    }

    void close() noexcept
    {
        std::lock_guard<std::recursive_mutex> lock(mutex_);
        if (context_.device.device) {
            renderer_.close();
            engineOpen_ = false;
            vkDeviceWaitIdle(context_.device.device);
            destroySurface();
            destroySemaphores();
        }
        if (window_) {
            ANativeWindow_release(window_);
            window_ = nullptr;
        }
        open_ = false;
        engineOpen_ = false;
    }

    mutable std::recursive_mutex mutex_;
    BorrowedAndroidVulkanContext context_;
    VulkanOutputPreference outputPreference_ =
        VulkanOutputPreference::PreferHdr;
    VulkanVideoRenderer renderer_;
    EventCallback eventCallback_;
    VideoRenderConfig config_;
    ANativeWindow* window_ = nullptr;
    VkSurfaceKHR surface_ = VK_NULL_HANDLE;
    VkSwapchainKHR swapchain_ = VK_NULL_HANDLE;
    std::vector<VkImage> images_;
    std::vector<VkImageView> views_;
    VkExtent2D extent_ {};
    VkFormat format_ = VK_FORMAT_UNDEFINED;
    VkColorSpaceKHR colorSpace_ = VK_COLOR_SPACE_SRGB_NONLINEAR_KHR;
    PFN_vkSetHdrMetadataEXT setHdrMetadata_ = nullptr;
    std::array<
        FrameSync,
        VulkanVideoRenderer::FramesInFlight> sync_;
    std::size_t syncIndex_ = 0;
    VulkanRenderTarget activeTarget_;
    std::uint32_t imageIndex_ = 0;
    std::uint64_t generation_ = 0;
    bool acquired_ = false;
    bool open_ = false;
    bool engineOpen_ = false;
    VideoRotation applicationRotation_ = VideoRotation::Rotate0;
};

AndroidVulkanVideoRenderer::AndroidVulkanVideoRenderer(
    BorrowedAndroidVulkanContext context,
    VulkanOutputPreference outputPreference)
    : impl_(std::make_unique<Impl>(context, outputPreference))
{
}

AndroidVulkanVideoRenderer::~AndroidVulkanVideoRenderer() = default;
AndroidVulkanVideoRenderer::AndroidVulkanVideoRenderer(
    AndroidVulkanVideoRenderer&&) noexcept = default;
AndroidVulkanVideoRenderer& AndroidVulkanVideoRenderer::operator=(
    AndroidVulkanVideoRenderer&&) noexcept = default;

VideoRenderCapabilities AndroidVulkanVideoRenderer::capabilities() const
{
    return impl_ ? impl_->renderer_.capabilities()
                 : VideoRenderCapabilities {};
}

void AndroidVulkanVideoRenderer::setEventCallback(EventCallback callback)
{
    if (!impl_) {
        return;
    }
    std::lock_guard<std::recursive_mutex> lock(impl_->mutex_);
    impl_->eventCallback_ = std::move(callback);
}

bool AndroidVulkanVideoRenderer::open(
    const VideoRenderConfig& config)
{
    if (!impl_) {
        return false;
    }
    std::string error;
    bool opened = false;
    {
        std::lock_guard<std::recursive_mutex> lock(impl_->mutex_);
        if (!impl_->context_.isValid()) {
            error = "The Android Vulkan adapter requires borrowed Vulkan context objects";
        } else if (!impl_->window_) {
            error = "The Android native window is unavailable";
        } else if (!impl_->createSwapchain(error)) {
            opened = false;
        } else {
            impl_->config_ = config;
            impl_->applicationRotation_ = config.rotation;
            impl_->updateConfigForSurface();
            opened = impl_->renderer_.open(impl_->config_);
            impl_->engineOpen_ = opened;
            impl_->open_ = opened;
            if (!opened && error.empty()) {
                error = "The Vulkan engine could not open the Android surface";
            }
        }
    }
    if (!opened) {
        impl_->notify(VideoRenderEventType::Error, std::move(error));
    }
    return opened;
}

bool AndroidVulkanVideoRenderer::configure(
    const VideoRenderConfig& config)
{
    if (!impl_) {
        return false;
    }
    std::lock_guard<std::recursive_mutex> lock(impl_->mutex_);
    if (!impl_->open_ || !impl_->swapchain_) {
        return false;
    }
    impl_->config_ = config;
    impl_->applicationRotation_ = config.rotation;
    impl_->updateConfigForSurface();
    return impl_->renderer_.configure(impl_->config_);
}

VideoRenderAttemptResult AndroidVulkanVideoRenderer::renderDetailed(
    const VideoFrame& frame)
{
    if (!impl_) {
        return {
            VideoRenderAttemptStatus::FatalError,
            0,
            "The Android Vulkan renderer is unavailable",
        };
    }
    std::string error;
    VideoRenderAttemptResult attempt {
        VideoRenderAttemptStatus::FatalError,
        0,
        {},
    };
    {
        std::lock_guard<std::recursive_mutex> lock(impl_->mutex_);
        if (!impl_->open_) {
            error = "The Android Vulkan renderer is not open";
        } else if (frame.hasHardwareFrame()) {
            const VulkanHardwareImportStatus status =
                impl_->renderer_.prepareHardwareFrame(
                    frame,
                    &error);
            if (status == VulkanHardwareImportStatus::Pending) {
                attempt = {
                    VideoRenderAttemptStatus::DeferredUntilRedraw,
                    0,
                    error,
                };
                error.clear();
            } else if (status == VulkanHardwareImportStatus::Stale) {
                attempt = {
                    VideoRenderAttemptStatus::Discarded,
                    0,
                    error,
                };
                error.clear();
            } else if (status
                       != VulkanHardwareImportStatus::Ready
                       && error.empty()) {
                error =
                    "The Android Vulkan hardware-frame preparation failed";
            }
            if (status != VulkanHardwareImportStatus::Ready) {
                // Pending and stale are fully classified above. Unsupported
                // or failed imports retain the default fatal result.
            } else if (!impl_->acquire(error)) {
            } else {
                impl_->applyHdrMetadata(frame);
                attempt = impl_->renderer_.renderDetailed(frame);
                if (!attempt.presented()) {
                    impl_->renderer_.close();
                    impl_->engineOpen_ = false;
                    vkDeviceWaitIdle(
                        impl_->context_.device.device);
                    impl_->destroySwapchain();
                    impl_->destroySemaphores();
                    std::string recoveryError;
                    if (impl_->window_
                        && impl_->createSwapchain(
                            recoveryError)) {
                        impl_->updateConfigForSurface();
                        impl_->engineOpen_ =
                            impl_->renderer_.open(
                                impl_->config_);
                    }
                    impl_->acquired_ = false;
                    impl_->activeTarget_ = {};
                    if (attempt.status
                            == VideoRenderAttemptStatus::FatalError
                        || attempt.status
                            == VideoRenderAttemptStatus::SurfaceLost) {
                        error = attempt.detail.empty()
                            ? "The Vulkan engine could not render the imported Android image"
                            : attempt.detail;
                    }
                } else {
                    if (!impl_->present(error)) {
                        attempt = {
                            VideoRenderAttemptStatus::FatalError,
                            0,
                            error,
                        };
                    }
                }
            }
        } else if (!impl_->acquire(error)) {
        } else {
            impl_->applyHdrMetadata(frame);
            attempt = impl_->renderer_.renderDetailed(frame);
            if (!attempt.presented()) {
                // No submission consumed imageAvailable_. Retire this
                // generation and its signaled semaphore before permitting
                // another acquire.
                impl_->renderer_.close();
                impl_->engineOpen_ = false;
                vkDeviceWaitIdle(impl_->context_.device.device);
                impl_->destroySwapchain();
                impl_->destroySemaphores();
                std::string recoveryError;
                if (impl_->window_
                    && impl_->createSwapchain(recoveryError)) {
                    impl_->updateConfigForSurface();
                    impl_->engineOpen_ =
                        impl_->renderer_.open(impl_->config_);
                }
                impl_->acquired_ = false;
                impl_->activeTarget_ = {};
                if (attempt.status
                        == VideoRenderAttemptStatus::FatalError
                    || attempt.status
                        == VideoRenderAttemptStatus::SurfaceLost) {
                    error = attempt.detail.empty()
                        ? "The Vulkan engine could not render the acquired Android image"
                        : attempt.detail;
                }
            } else {
                if (!impl_->present(error)) {
                    attempt = {
                        VideoRenderAttemptStatus::FatalError,
                        0,
                        error,
                    };
                }
            }
        }
    }
    if (attempt.status == VideoRenderAttemptStatus::DeferredUntilRedraw
        || attempt.status == VideoRenderAttemptStatus::RetryAfterBackoff
        || attempt.status == VideoRenderAttemptStatus::Discarded) {
        return attempt;
    }
    if (attempt.presented() && error.empty()) {
        return attempt;
    }
    const bool surfaceLost =
        attempt.status == VideoRenderAttemptStatus::SurfaceLost
        || error.find("SURFACE_LOST") != std::string::npos
        || error.find("native window") != std::string::npos;
    if (error.empty()) {
        error = surfaceLost
            ? "The Android Vulkan surface is unavailable"
            : "The Android Vulkan renderer could not render the frame";
    }
    const VideoRenderEventType type = surfaceLost
        ? VideoRenderEventType::SurfaceLost
        : VideoRenderEventType::Error;
    impl_->notify(type, error);
    return {
        surfaceLost ? VideoRenderAttemptStatus::SurfaceLost
                    : VideoRenderAttemptStatus::FatalError,
        0,
        error,
    };
}

bool AndroidVulkanVideoRenderer::render(const VideoFrame& frame)
{
    return renderDetailed(frame).frameConsumed();
}

void AndroidVulkanVideoRenderer::close() noexcept
{
    if (impl_) {
        impl_->close();
    }
}

void AndroidVulkanVideoRenderer::invalidatePendingFrames() noexcept
{
    if (impl_) {
        impl_->renderer_.invalidatePendingFrames();
    }
}

bool AndroidVulkanVideoRenderer::setWindow(ANativeWindow* window)
{
    if (!impl_) {
        return false;
    }
    std::string error;
    bool succeeded = true;
    {
        std::lock_guard<std::recursive_mutex> lock(impl_->mutex_);
        if (window == impl_->window_) {
            if (!window) {
                return false;
            }
            // SurfaceView can publish a same-identity Surface whose new base
            // geometry is not yet reflected by ANativeWindow_getWidth/Height.
            // A same-window publication is therefore an explicit refresh
            // signal, not a value-based no-op.
            succeeded = impl_->recreate(error);
        } else {
            if (impl_->context_.device.device) {
                impl_->renderer_.close();
                impl_->engineOpen_ = false;
                vkDeviceWaitIdle(impl_->context_.device.device);
            }
            impl_->destroySurface();
            if (impl_->window_) {
                ANativeWindow_release(impl_->window_);
            }
            impl_->window_ = window;
            if (impl_->window_) {
                ANativeWindow_acquire(impl_->window_);
                succeeded = impl_->createSwapchain(error);
                if (succeeded && impl_->open_) {
                    impl_->updateConfigForSurface();
                    succeeded = impl_->renderer_.open(impl_->config_);
                    impl_->engineOpen_ = succeeded;
                }
            } else {
                succeeded = false;
            }
        }
    }
    if (!succeeded) {
        impl_->notify(
            window ? VideoRenderEventType::Error
                   : VideoRenderEventType::SurfaceLost,
            error.empty()
                ? "The Android native window was removed"
                : std::move(error));
    }
    return succeeded;
}

VideoSize AndroidVulkanVideoRenderer::surfaceSize() const noexcept
{
    if (!impl_) {
        return {};
    }
    std::lock_guard<std::recursive_mutex> lock(impl_->mutex_);
    return {
        static_cast<int>(impl_->extent_.width),
        static_cast<int>(impl_->extent_.height),
    };
}

VkSurfaceFormatKHR
AndroidVulkanVideoRenderer::surfaceFormat() const noexcept
{
    if (!impl_) {
        return {
            VK_FORMAT_UNDEFINED,
            VK_COLOR_SPACE_SRGB_NONLINEAR_KHR,
        };
    }
    std::lock_guard<std::recursive_mutex> lock(impl_->mutex_);
    return { impl_->format_, impl_->colorSpace_ };
}

bool AndroidVulkanVideoRenderer::hdrOutputActive() const noexcept
{
    if (!impl_) {
        return false;
    }
    std::lock_guard<std::recursive_mutex> lock(impl_->mutex_);
    return impl_->format_ != VK_FORMAT_UNDEFINED
        && vulkanColorSpaceIsHdr(impl_->colorSpace_);
}

BorrowedAndroidVulkanContext
AndroidVulkanVideoRenderer::context() const noexcept
{
    return impl_ ? impl_->context_
                 : BorrowedAndroidVulkanContext {};
}

void AndroidVulkanVideoRenderer::setHardwareFrameInterop(
    std::shared_ptr<VulkanHardwareFrameInterop> hardwareInterop)
{
    if (!impl_) {
        return;
    }
    std::lock_guard<std::recursive_mutex> lock(impl_->mutex_);
    impl_->renderer_.setHardwareFrameInterop(
        std::move(hardwareInterop));
}

std::shared_ptr<VulkanHardwareFrameInterop>
AndroidVulkanVideoRenderer::hardwareFrameInterop() const noexcept
{
    if (!impl_) {
        return {};
    }
    std::lock_guard<std::recursive_mutex> lock(impl_->mutex_);
    return impl_->renderer_.hardwareFrameInterop();
}

} // namespace qtav
