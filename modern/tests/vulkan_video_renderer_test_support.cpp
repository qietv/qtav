// SPDX-License-Identifier: LGPL-2.1-or-later

#include "vulkan_video_renderer_test_support.h"

#include "frame_internal.h"

#include <cstdint>
#include <cstring>
#include <limits>
#include <memory>
#include <string>
#include <vector>

extern "C" {
#include <libavutil/frame.h>
#include <libavutil/pixfmt.h>
}

namespace qtav::test {
namespace {

struct Pixel {
    std::uint8_t blue = 0;
    std::uint8_t green = 0;
    std::uint8_t red = 0;
    std::uint8_t alpha = 0;
};

bool isBlack(Pixel pixel) noexcept
{
    return pixel.red < 16 && pixel.green < 16 && pixel.blue < 16
        && pixel.alpha > 239;
}

bool isRed(Pixel pixel) noexcept
{
    return pixel.red > 160
        && pixel.red > static_cast<int>(pixel.green) * 2
        && pixel.red > static_cast<int>(pixel.blue) * 2
        && pixel.alpha > 239;
}

bool isBlue(Pixel pixel) noexcept
{
    return pixel.blue > 160
        && pixel.blue > static_cast<int>(pixel.green) * 2
        && pixel.blue > static_cast<int>(pixel.red) * 2
        && pixel.alpha > 239;
}

std::string resultError(const char* operation, VkResult result)
{
    return std::string(operation) + " failed ("
        + std::to_string(static_cast<int>(result)) + ')';
}

class OffscreenTarget {
public:
    explicit OffscreenTarget(BorrowedVulkanDevice device)
        : device_(device)
    {
    }

    ~OffscreenTarget()
    {
        destroy();
    }

    OffscreenTarget(const OffscreenTarget&) = delete;
    OffscreenTarget& operator=(const OffscreenTarget&) = delete;

    bool create(
        std::uint32_t width,
        std::uint32_t height,
        std::string& error)
    {
        if (!device_.isValid() || width == 0 || height == 0) {
            error = "The offscreen Vulkan target configuration is invalid";
            return false;
        }
        destroy();

        VkFormatProperties formatProperties {};
        vkGetPhysicalDeviceFormatProperties(
            device_.physicalDevice,
            VK_FORMAT_B8G8R8A8_UNORM,
            &formatProperties);
        constexpr VkFormatFeatureFlags RequiredFormatFeatures =
            VK_FORMAT_FEATURE_COLOR_ATTACHMENT_BIT
            | VK_FORMAT_FEATURE_TRANSFER_SRC_BIT;
        if ((formatProperties.optimalTilingFeatures
                & RequiredFormatFeatures)
            != RequiredFormatFeatures) {
            error =
                "The Vulkan device cannot read back an offscreen BGRA8 target";
            return false;
        }

        extent_ = { width, height };
        VkImageCreateInfo imageInfo {
            VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO,
        };
        imageInfo.imageType = VK_IMAGE_TYPE_2D;
        imageInfo.format = VK_FORMAT_B8G8R8A8_UNORM;
        imageInfo.extent = { width, height, 1 };
        imageInfo.mipLevels = 1;
        imageInfo.arrayLayers = 1;
        imageInfo.samples = VK_SAMPLE_COUNT_1_BIT;
        imageInfo.tiling = VK_IMAGE_TILING_OPTIMAL;
        imageInfo.usage = VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT
            | VK_IMAGE_USAGE_TRANSFER_SRC_BIT;
        imageInfo.sharingMode = VK_SHARING_MODE_EXCLUSIVE;
        imageInfo.initialLayout = VK_IMAGE_LAYOUT_UNDEFINED;
        VkResult result = vkCreateImage(
            device_.device,
            &imageInfo,
            nullptr,
            &image_);
        if (result != VK_SUCCESS) {
            error = resultError("vkCreateImage", result);
            return false;
        }

        VkMemoryRequirements imageRequirements {};
        vkGetImageMemoryRequirements(
            device_.device,
            image_,
            &imageRequirements);
        std::uint32_t imageMemoryType = memoryType(
            imageRequirements.memoryTypeBits,
            VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT);
        if (imageMemoryType
            == std::numeric_limits<std::uint32_t>::max()) {
            imageMemoryType = memoryType(imageRequirements.memoryTypeBits, 0);
        }
        if (imageMemoryType
            == std::numeric_limits<std::uint32_t>::max()) {
            error = "The Vulkan device has no memory for the offscreen image";
            return false;
        }
        VkMemoryAllocateInfo imageAllocation {
            VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO,
        };
        imageAllocation.allocationSize = imageRequirements.size;
        imageAllocation.memoryTypeIndex = imageMemoryType;
        result = vkAllocateMemory(
            device_.device,
            &imageAllocation,
            nullptr,
            &imageMemory_);
        if (result == VK_SUCCESS) {
            result = vkBindImageMemory(
                device_.device,
                image_,
                imageMemory_,
                0);
        }
        if (result != VK_SUCCESS) {
            error = resultError("offscreen image memory", result);
            return false;
        }

        VkImageViewCreateInfo viewInfo {
            VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO,
        };
        viewInfo.image = image_;
        viewInfo.viewType = VK_IMAGE_VIEW_TYPE_2D;
        viewInfo.format = VK_FORMAT_B8G8R8A8_UNORM;
        viewInfo.subresourceRange.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
        viewInfo.subresourceRange.levelCount = 1;
        viewInfo.subresourceRange.layerCount = 1;
        result = vkCreateImageView(
            device_.device,
            &viewInfo,
            nullptr,
            &view_);
        if (result != VK_SUCCESS) {
            error = resultError("vkCreateImageView", result);
            return false;
        }

        const VkDeviceSize byteCount =
            static_cast<VkDeviceSize>(width) * height * sizeof(Pixel);
        VkBufferCreateInfo bufferInfo {
            VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO,
        };
        bufferInfo.size = byteCount;
        bufferInfo.usage = VK_BUFFER_USAGE_TRANSFER_DST_BIT;
        bufferInfo.sharingMode = VK_SHARING_MODE_EXCLUSIVE;
        result = vkCreateBuffer(
            device_.device,
            &bufferInfo,
            nullptr,
            &readbackBuffer_);
        if (result != VK_SUCCESS) {
            error = resultError("vkCreateBuffer", result);
            return false;
        }
        VkMemoryRequirements bufferRequirements {};
        vkGetBufferMemoryRequirements(
            device_.device,
            readbackBuffer_,
            &bufferRequirements);
        const std::uint32_t bufferMemoryType = memoryType(
            bufferRequirements.memoryTypeBits,
            VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT
                | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT);
        if (bufferMemoryType
            == std::numeric_limits<std::uint32_t>::max()) {
            error = "The Vulkan device has no coherent readback memory";
            return false;
        }
        VkMemoryAllocateInfo bufferAllocation {
            VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO,
        };
        bufferAllocation.allocationSize = bufferRequirements.size;
        bufferAllocation.memoryTypeIndex = bufferMemoryType;
        result = vkAllocateMemory(
            device_.device,
            &bufferAllocation,
            nullptr,
            &readbackMemory_);
        if (result == VK_SUCCESS) {
            result = vkBindBufferMemory(
                device_.device,
                readbackBuffer_,
                readbackMemory_,
                0);
        }
        if (result != VK_SUCCESS) {
            error = resultError("offscreen readback memory", result);
            return false;
        }

        VkCommandPoolCreateInfo poolInfo {
            VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO,
        };
        poolInfo.flags = VK_COMMAND_POOL_CREATE_RESET_COMMAND_BUFFER_BIT;
        poolInfo.queueFamilyIndex = device_.queueFamilyIndex;
        result = vkCreateCommandPool(
            device_.device,
            &poolInfo,
            nullptr,
            &commandPool_);
        if (result != VK_SUCCESS) {
            error = resultError("vkCreateCommandPool", result);
            return false;
        }
        VkCommandBufferAllocateInfo commandInfo {
            VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO,
        };
        commandInfo.commandPool = commandPool_;
        commandInfo.level = VK_COMMAND_BUFFER_LEVEL_PRIMARY;
        commandInfo.commandBufferCount = 1;
        result = vkAllocateCommandBuffers(
            device_.device,
            &commandInfo,
            &commandBuffer_);
        if (result != VK_SUCCESS) {
            error = resultError("vkAllocateCommandBuffers", result);
            return false;
        }
        VkFenceCreateInfo fenceInfo {
            VK_STRUCTURE_TYPE_FENCE_CREATE_INFO,
        };
        fenceInfo.flags = VK_FENCE_CREATE_SIGNALED_BIT;
        result = vkCreateFence(
            device_.device,
            &fenceInfo,
            nullptr,
            &fence_);
        if (result != VK_SUCCESS) {
            error = resultError("vkCreateFence", result);
            return false;
        }
        ++generation_;
        return true;
    }

    VulkanRenderTarget renderTarget(bool waitUntilCompleted) const noexcept
    {
        VulkanRenderTarget result;
        result.image = image_;
        result.imageView = view_;
        result.format = VK_FORMAT_B8G8R8A8_UNORM;
        result.extent = extent_;
        result.finalLayout = VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL;
        result.generation = generation_;
        result.waitUntilCompleted = waitUntilCompleted;
        return result;
    }

    bool readPixels(std::vector<Pixel>& pixels, std::string& error)
    {
        VkResult result = vkWaitForFences(
            device_.device,
            1,
            &fence_,
            VK_TRUE,
            std::numeric_limits<std::uint64_t>::max());
        if (result == VK_SUCCESS) {
            result = vkResetFences(device_.device, 1, &fence_);
        }
        if (result == VK_SUCCESS) {
            result = vkResetCommandBuffer(commandBuffer_, 0);
        }
        if (result != VK_SUCCESS) {
            error = resultError("offscreen readback reset", result);
            return false;
        }

        VkCommandBufferBeginInfo beginInfo {
            VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO,
        };
        beginInfo.flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT;
        result = vkBeginCommandBuffer(commandBuffer_, &beginInfo);
        if (result != VK_SUCCESS) {
            error = resultError("vkBeginCommandBuffer", result);
            return false;
        }
        VkImageMemoryBarrier barrier {
            VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER,
        };
        barrier.srcAccessMask = VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT;
        barrier.dstAccessMask = VK_ACCESS_TRANSFER_READ_BIT;
        barrier.oldLayout = VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL;
        barrier.newLayout = VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL;
        barrier.srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
        barrier.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
        barrier.image = image_;
        barrier.subresourceRange.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
        barrier.subresourceRange.levelCount = 1;
        barrier.subresourceRange.layerCount = 1;
        vkCmdPipelineBarrier(
            commandBuffer_,
            VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT,
            VK_PIPELINE_STAGE_TRANSFER_BIT,
            0,
            0,
            nullptr,
            0,
            nullptr,
            1,
            &barrier);
        VkBufferImageCopy copy {};
        copy.imageSubresource.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
        copy.imageSubresource.layerCount = 1;
        copy.imageExtent = { extent_.width, extent_.height, 1 };
        vkCmdCopyImageToBuffer(
            commandBuffer_,
            image_,
            VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL,
            readbackBuffer_,
            1,
            &copy);
        result = vkEndCommandBuffer(commandBuffer_);
        if (result == VK_SUCCESS) {
            VkSubmitInfo submitInfo { VK_STRUCTURE_TYPE_SUBMIT_INFO };
            submitInfo.commandBufferCount = 1;
            submitInfo.pCommandBuffers = &commandBuffer_;
            result = vkQueueSubmit(
                device_.queue,
                1,
                &submitInfo,
                fence_);
        }
        if (result == VK_SUCCESS) {
            result = vkWaitForFences(
                device_.device,
                1,
                &fence_,
                VK_TRUE,
                std::numeric_limits<std::uint64_t>::max());
        }
        if (result != VK_SUCCESS) {
            error = resultError("offscreen readback submission", result);
            return false;
        }

        const std::size_t pixelCount =
            static_cast<std::size_t>(extent_.width) * extent_.height;
        void* mapped = nullptr;
        result = vkMapMemory(
            device_.device,
            readbackMemory_,
            0,
            pixelCount * sizeof(Pixel),
            0,
            &mapped);
        if (result != VK_SUCCESS) {
            error = resultError("vkMapMemory", result);
            return false;
        }
        pixels.resize(pixelCount);
        std::memcpy(
            pixels.data(),
            mapped,
            pixelCount * sizeof(Pixel));
        vkUnmapMemory(device_.device, readbackMemory_);
        return true;
    }

private:
    std::uint32_t memoryType(
        std::uint32_t bits,
        VkMemoryPropertyFlags required) const noexcept
    {
        VkPhysicalDeviceMemoryProperties properties {};
        vkGetPhysicalDeviceMemoryProperties(
            device_.physicalDevice,
            &properties);
        for (std::uint32_t index = 0;
             index < properties.memoryTypeCount;
             ++index) {
            if ((bits & (1U << index)) != 0U
                && (properties.memoryTypes[index].propertyFlags & required)
                    == required) {
                return index;
            }
        }
        return std::numeric_limits<std::uint32_t>::max();
    }

    void destroy() noexcept
    {
        if (!device_.device) {
            return;
        }
        vkDeviceWaitIdle(device_.device);
        if (fence_) {
            vkDestroyFence(device_.device, fence_, nullptr);
        }
        if (commandPool_) {
            vkDestroyCommandPool(device_.device, commandPool_, nullptr);
        }
        if (readbackBuffer_) {
            vkDestroyBuffer(device_.device, readbackBuffer_, nullptr);
        }
        if (readbackMemory_) {
            vkFreeMemory(device_.device, readbackMemory_, nullptr);
        }
        if (view_) {
            vkDestroyImageView(device_.device, view_, nullptr);
        }
        if (image_) {
            vkDestroyImage(device_.device, image_, nullptr);
        }
        if (imageMemory_) {
            vkFreeMemory(device_.device, imageMemory_, nullptr);
        }
        fence_ = VK_NULL_HANDLE;
        commandPool_ = VK_NULL_HANDLE;
        commandBuffer_ = VK_NULL_HANDLE;
        readbackBuffer_ = VK_NULL_HANDLE;
        readbackMemory_ = VK_NULL_HANDLE;
        view_ = VK_NULL_HANDLE;
        image_ = VK_NULL_HANDLE;
        imageMemory_ = VK_NULL_HANDLE;
        extent_ = {};
    }

    BorrowedVulkanDevice device_;
    VkImage image_ = VK_NULL_HANDLE;
    VkDeviceMemory imageMemory_ = VK_NULL_HANDLE;
    VkImageView view_ = VK_NULL_HANDLE;
    VkBuffer readbackBuffer_ = VK_NULL_HANDLE;
    VkDeviceMemory readbackMemory_ = VK_NULL_HANDLE;
    VkCommandPool commandPool_ = VK_NULL_HANDLE;
    VkCommandBuffer commandBuffer_ = VK_NULL_HANDLE;
    VkFence fence_ = VK_NULL_HANDLE;
    VkExtent2D extent_ {};
    std::uint64_t generation_ = 0;
};

Pixel pixel(
    const std::vector<Pixel>& pixels,
    std::uint32_t width,
    int x,
    int y)
{
    return pixels[static_cast<std::size_t>(y) * width
        + static_cast<std::size_t>(x)];
}

bool require(
    bool condition,
    const char* detail,
    std::string& error)
{
    if (!condition) {
        error = detail;
    }
    return condition;
}

VideoFrame makeColorVariant(bool fullRange, bool bt709)
{
    AVFrame* native = av_frame_alloc();
    if (!native) {
        return {};
    }
    native->width = 4;
    native->height = 2;
    native->format = AV_PIX_FMT_YUV420P;
    if (av_frame_get_buffer(native, 32) < 0) {
        av_frame_free(&native);
        return {};
    }

    const std::uint8_t redY =
        fullRange ? 76 : (bt709 ? 63 : 81);
    const std::uint8_t blueY =
        fullRange ? 29 : (bt709 ? 32 : 41);
    for (int row = 0; row < native->height; ++row) {
        for (int column = 0; column < native->width; ++column) {
            native->data[0][row * native->linesize[0] + column] =
                column < 2 ? redY : blueY;
        }
    }
    native->data[1][0] =
        fullRange ? 85 : (bt709 ? 102 : 90);
    native->data[1][1] = 240;
    native->data[2][0] = fullRange ? 255 : 240;
    native->data[2][1] =
        fullRange ? 107 : (bt709 ? 118 : 110);
    native->color_range =
        fullRange ? AVCOL_RANGE_JPEG : AVCOL_RANGE_MPEG;
    native->colorspace = bt709 ? AVCOL_SPC_BT709 : AVCOL_SPC_SMPTE170M;
    native->color_primaries = AVCOL_PRI_BT709;
    native->color_trc = AVCOL_TRC_BT709;

    VideoFrame result = detail::FrameFactory::video(native, 0, 0);
    av_frame_free(&native);
    return result;
}

} // namespace

bool runVulkanOffscreenRendererChecks(
    BorrowedVulkanDevice device,
    const VideoFrame& frame,
    std::string& error)
{
    if (!device.isValid() || !frame || frame.hasHardwareFrame()) {
        error = "The Vulkan offscreen check requires a software frame and device";
        return false;
    }

    OffscreenTarget target(device);
    if (!target.create(8, 8, error)) {
        return false;
    }
    bool waitUntilCompleted = false;
    auto renderer = std::make_shared<VulkanVideoRenderer>(
        device,
        [&] { return target.renderTarget(waitUntilCompleted); });
    renderer->setEventCallback(
        [&](const VideoRenderEvent& event) {
            if (event.type != VideoRenderEventType::RedrawRequested) {
                error = event.detail;
            }
        });

    VideoRenderConfig config;
    config.surfaceSize = { 8, 8 };
    if (!renderer->open(config)) {
        if (error.empty()) {
            error = "The Vulkan offscreen renderer could not open";
        }
        return false;
    }

    for (std::size_t index = 0;
         index < VulkanVideoRenderer::FramesInFlight * 2;
         ++index) {
        if (!renderer->render(frame)) {
            if (error.empty()) {
                error = "A Vulkan in-flight ring submission failed";
            }
            return false;
        }
    }
    waitUntilCompleted = true;
    if (!renderer->render(frame)) {
        if (error.empty()) {
            error = "The completed Vulkan ring submission failed";
        }
        return false;
    }

    std::vector<Pixel> pixels;
    if (!target.readPixels(pixels, error)
        || !require(
            isBlack(pixel(pixels, 8, 4, 0)),
            "Vulkan Fit did not preserve the top letterbox",
            error)
        || !require(
            isRed(pixel(pixels, 8, 1, 4)),
            "Vulkan Fit did not render the red source half",
            error)
        || !require(
            isBlue(pixel(pixels, 8, 6, 4)),
            "Vulkan Fit did not render the blue source half",
            error)
        || !require(
            isBlack(pixel(pixels, 8, 4, 7)),
            "Vulkan Fit did not preserve the bottom letterbox",
            error)) {
        return false;
    }

    config.aspectRatio = VideoAspectRatioMode::Stretch;
    const VideoFrame fullRange = makeColorVariant(true, false);
    const VideoFrame bt709 = makeColorVariant(false, true);
    if (!fullRange || !bt709) {
        error = "Could not create Vulkan color-space golden frames";
        return false;
    }
    const auto renderColorVariant =
        [&](const VideoFrame& colorFrame, const char* name) {
            if (!renderer->configure(config)
                || !renderer->render(colorFrame)
                || !target.readPixels(pixels, error)
                || !isRed(pixel(pixels, 8, 1, 4))
                || !isBlue(pixel(pixels, 8, 6, 4))) {
                if (error.empty()) {
                    error = std::string(
                        "Vulkan color conversion failed for ")
                        + name;
                }
                return false;
            }
            return true;
        };
    if (!renderColorVariant(fullRange, "full-range BT.601")
        || !renderColorVariant(bt709, "limited-range BT.709")) {
        return false;
    }

    config.viewport = { 2, 1, 4, 6 };
    config.aspectRatio = VideoAspectRatioMode::Stretch;
    if (!renderer->configure(config)
        || !renderer->render(frame)
        || !target.readPixels(pixels, error)
        || !require(
            isBlack(pixel(pixels, 8, 1, 4)),
            "Vulkan viewport wrote outside its left edge",
            error)
        || !require(
            isRed(pixel(pixels, 8, 2, 4)),
            "Vulkan viewport lost the red source half",
            error)
        || !require(
            isBlue(pixel(pixels, 8, 5, 4)),
            "Vulkan viewport lost the blue source half",
            error)
        || !require(
            isBlack(pixel(pixels, 8, 6, 4)),
            "Vulkan viewport wrote outside its right edge",
            error)) {
        return false;
    }

    config.viewport = {};
    config.rotation = VideoRotation::Rotate180;
    if (!renderer->configure(config)
        || !renderer->render(frame)
        || !target.readPixels(pixels, error)
        || !require(
            isBlue(pixel(pixels, 8, 1, 4)),
            "Vulkan rotation did not move blue to the left",
            error)
        || !require(
            isRed(pixel(pixels, 8, 6, 4)),
            "Vulkan rotation did not move red to the right",
            error)) {
        return false;
    }

    renderer->close();
    if (!target.create(6, 4, error)) {
        return false;
    }
    config.surfaceSize = { 6, 4 };
    config.rotation = VideoRotation::Rotate0;
    if (!renderer->open(config)
        || !renderer->render(frame)
        || !target.readPixels(pixels, error)
        || !require(
            isRed(pixel(pixels, 6, 1, 2)),
            "Vulkan target recreation lost the red source half",
            error)
        || !require(
            isBlue(pixel(pixels, 6, 4, 2)),
            "Vulkan target recreation lost the blue source half",
            error)) {
        return false;
    }
    renderer->close();
    return true;
}

} // namespace qtav::test
