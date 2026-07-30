// SPDX-License-Identifier: LGPL-2.1-or-later

#include "vulkan_video_renderer_test_support.h"

#include "frame_internal.h"

#include <algorithm>
#include <array>
#include <cmath>
#include <cstdint>
#include <cstring>
#include <limits>
#include <memory>
#include <string>
#include <vector>

extern "C" {
#include <libavutil/frame.h>
#include <libavutil/mastering_display_metadata.h>
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

struct HalfPixel {
    std::uint16_t red = 0;
    std::uint16_t green = 0;
    std::uint16_t blue = 0;
    std::uint16_t alpha = 0;
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
        std::string& error,
        VkFormat format = VK_FORMAT_B8G8R8A8_UNORM,
        VkColorSpaceKHR colorSpace =
            VK_COLOR_SPACE_SRGB_NONLINEAR_KHR)
    {
        if (!device_.isValid() || width == 0 || height == 0) {
            error = "The offscreen Vulkan target configuration is invalid";
            return false;
        }
        destroy();

        VkFormatProperties formatProperties {};
        vkGetPhysicalDeviceFormatProperties(
            device_.physicalDevice,
            format,
            &formatProperties);
        constexpr VkFormatFeatureFlags RequiredFormatFeatures =
            VK_FORMAT_FEATURE_COLOR_ATTACHMENT_BIT
            | VK_FORMAT_FEATURE_TRANSFER_SRC_BIT;
        if ((formatProperties.optimalTilingFeatures
                & RequiredFormatFeatures)
            != RequiredFormatFeatures) {
            error =
                "The Vulkan device cannot read back the requested offscreen target";
            return false;
        }

        extent_ = { width, height };
        format_ = format;
        colorSpace_ = colorSpace;
        pixelBytes_ = format_ == VK_FORMAT_R16G16B16A16_SFLOAT
            ? sizeof(HalfPixel)
            : sizeof(Pixel);
        VkImageCreateInfo imageInfo {
            VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO,
        };
        imageInfo.imageType = VK_IMAGE_TYPE_2D;
        imageInfo.format = format_;
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
        viewInfo.format = format_;
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
            static_cast<VkDeviceSize>(width) * height * pixelBytes_;
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
        result.format = format_;
        result.colorSpace = colorSpace_;
        result.extent = extent_;
        result.finalLayout = VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL;
        result.generation = generation_;
        result.waitUntilCompleted = waitUntilCompleted;
        return result;
    }

    bool readPixels(std::vector<Pixel>& pixels, std::string& error)
    {
        return readValues(pixels, error);
    }

    bool readHalfPixels(
        std::vector<HalfPixel>& pixels,
        std::string& error)
    {
        return readValues(pixels, error);
    }

private:
    template <typename Value>
    bool readValues(std::vector<Value>& pixels, std::string& error)
    {
        if (sizeof(Value) != pixelBytes_) {
            error =
                "The offscreen Vulkan readback value size does not match its format";
            return false;
        }
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
            pixelCount * sizeof(Value),
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
            pixelCount * sizeof(Value));
        vkUnmapMemory(device_.device, readbackMemory_);
        return true;
    }

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
        format_ = VK_FORMAT_UNDEFINED;
        colorSpace_ = VK_COLOR_SPACE_SRGB_NONLINEAR_KHR;
        pixelBytes_ = 0;
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
    VkFormat format_ = VK_FORMAT_UNDEFINED;
    VkColorSpaceKHR colorSpace_ = VK_COLOR_SPACE_SRGB_NONLINEAR_KHR;
    std::size_t pixelBytes_ = 0;
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

enum class HdrTransfer {
    PQ,
    HLG,
};

struct HdrVariant {
    VideoFrame frame;
    std::array<std::uint16_t, 4> lumaCodes {};
};

double pqSignalForNits(double nits) noexcept
{
    constexpr double M1 = 2610.0 / 16384.0;
    constexpr double M2 = 2523.0 / 32.0;
    constexpr double C1 = 3424.0 / 4096.0;
    constexpr double C2 = 2413.0 / 128.0;
    constexpr double C3 = 2392.0 / 128.0;
    const double luminance =
        std::clamp(nits / 10000.0, 0.0, 1.0);
    const double power = std::pow(luminance, M1);
    return std::pow(
        (C1 + C2 * power) / (1.0 + C3 * power),
        M2);
}

double hlgSignalForNits(double nits) noexcept
{
    constexpr double A = 0.17883277;
    constexpr double B = 0.28466892;
    constexpr double C = 0.55991073;
    const double luminance =
        std::clamp(nits / 1000.0, 0.0, 1.0);
    if (luminance <= 1.0 / 12.0) {
        return std::sqrt(3.0 * luminance);
    }
    return A * std::log(12.0 * luminance - B) + C;
}

double pqNits(double signal) noexcept
{
    constexpr double M1 = 2610.0 / 16384.0;
    constexpr double M2 = 2523.0 / 32.0;
    constexpr double C1 = 3424.0 / 4096.0;
    constexpr double C2 = 2413.0 / 128.0;
    constexpr double C3 = 2392.0 / 128.0;
    const double power =
        std::pow(std::max(signal, 0.0), 1.0 / M2);
    const double numerator = std::max(power - C1, 0.0);
    const double denominator = std::max(C2 - C3 * power, 0.000001);
    return 10000.0
        * std::pow(numerator / denominator, 1.0 / M1);
}

double hlgNits(double signal) noexcept
{
    constexpr double A = 0.17883277;
    constexpr double B = 0.28466892;
    constexpr double C = 0.55991073;
    if (signal <= 0.5) {
        return 1000.0 * signal * signal / 3.0;
    }
    return 1000.0
        * (std::exp((signal - C) / A) + B) / 12.0;
}

std::uint8_t expectedHdrSdrCode(
    std::uint16_t lumaCode,
    HdrTransfer transfer,
    double maximumLuminance) noexcept
{
    const double signal = std::clamp(
        (static_cast<double>(lumaCode) - 64.0) / 876.0,
        0.0,
        1.0);
    const double nits = transfer == HdrTransfer::PQ
        ? pqNits(signal)
        : hlgNits(signal);
    const double compressed =
        nits / (1.0 + nits / maximumLuminance);
    const double linear =
        std::clamp(compressed / 100.0, 0.0, 1.0);
    const double srgb = linear > 0.0031308
        ? 1.055 * std::pow(linear, 1.0 / 2.4) - 0.055
        : 12.92 * linear;
    return static_cast<std::uint8_t>(
        std::clamp(std::lround(srgb * 255.0), 0L, 255L));
}

HdrVariant makeHdrVariant(
    HdrTransfer transfer,
    int masteringMaximumLuminance,
    unsigned int maximumContentLightLevel)
{
    HdrVariant result;
    AVFrame* native = av_frame_alloc();
    if (!native) {
        return result;
    }
    native->width = 4;
    native->height = 2;
    native->format = AV_PIX_FMT_P010LE;
    if (av_frame_get_buffer(native, 32) < 0) {
        av_frame_free(&native);
        return result;
    }

    constexpr std::array<double, 4> SampleNits {
        0.0,
        10.0,
        100.0,
        400.0,
    };
    for (std::size_t column = 0; column < SampleNits.size(); ++column) {
        const double signal = transfer == HdrTransfer::PQ
            ? pqSignalForNits(SampleNits[column])
            : hlgSignalForNits(SampleNits[column]);
        result.lumaCodes[column] =
            static_cast<std::uint16_t>(std::clamp(
                std::lround(64.0 + signal * 876.0),
                64L,
                940L));
    }
    for (int row = 0; row < native->height; ++row) {
        auto* luma = reinterpret_cast<std::uint16_t*>(
            native->data[0] + row * native->linesize[0]);
        for (std::size_t column = 0;
             column < result.lumaCodes.size();
             ++column) {
            luma[column] = static_cast<std::uint16_t>(
                result.lumaCodes[column] << 6U);
        }
    }
    auto* chroma =
        reinterpret_cast<std::uint16_t*>(native->data[1]);
    for (int pair = 0; pair < 2; ++pair) {
        chroma[pair * 2] = static_cast<std::uint16_t>(512U << 6U);
        chroma[pair * 2 + 1] =
            static_cast<std::uint16_t>(512U << 6U);
    }

    native->color_range = AVCOL_RANGE_MPEG;
    native->colorspace = AVCOL_SPC_BT2020_NCL;
    native->color_primaries = AVCOL_PRI_BT2020;
    native->color_trc = transfer == HdrTransfer::PQ
        ? AVCOL_TRC_SMPTE2084
        : AVCOL_TRC_ARIB_STD_B67;
    native->chroma_location = AVCHROMA_LOC_LEFT;

    if (masteringMaximumLuminance > 0) {
        AVMasteringDisplayMetadata* mastering =
            av_mastering_display_metadata_create_side_data(native);
        if (!mastering) {
            av_frame_free(&native);
            return {};
        }
        mastering->has_luminance = 1;
        mastering->min_luminance = { 1, 10000 };
        mastering->max_luminance = {
            masteringMaximumLuminance,
            1,
        };
        mastering->has_primaries = 1;
        mastering->display_primaries[0][0] = { 708, 1000 };
        mastering->display_primaries[0][1] = { 292, 1000 };
        mastering->display_primaries[1][0] = { 170, 1000 };
        mastering->display_primaries[1][1] = { 797, 1000 };
        mastering->display_primaries[2][0] = { 131, 1000 };
        mastering->display_primaries[2][1] = { 46, 1000 };
        mastering->white_point[0] = { 3127, 10000 };
        mastering->white_point[1] = { 3290, 10000 };
    }
    if (maximumContentLightLevel > 0) {
        AVContentLightMetadata* content =
            av_content_light_metadata_create_side_data(native);
        if (!content) {
            av_frame_free(&native);
            return {};
        }
        content->MaxCLL = maximumContentLightLevel;
        content->MaxFALL = maximumContentLightLevel / 2;
    }

    result.frame = detail::FrameFactory::video(native, 0, 0);
    av_frame_free(&native);
    return result;
}

bool closeToCode(std::uint8_t actual, std::uint8_t expected) noexcept
{
    return std::abs(
        static_cast<int>(actual) - static_cast<int>(expected))
        <= 4;
}

std::uint16_t expectedHdrOutputCode(
    std::uint16_t lumaCode,
    HdrTransfer sourceTransfer,
    VkColorSpaceKHR outputColorSpace) noexcept
{
    const double sourceSignal = std::clamp(
        (static_cast<double>(lumaCode) - 64.0) / 876.0,
        0.0,
        1.0);
    const double nits = sourceTransfer == HdrTransfer::PQ
        ? pqNits(sourceSignal)
        : hlgNits(sourceSignal);
    const double outputSignal =
        outputColorSpace == VK_COLOR_SPACE_HDR10_HLG_EXT
        ? hlgSignalForNits(nits)
        : pqSignalForNits(nits);
    return static_cast<std::uint16_t>(
        std::clamp(std::lround(outputSignal * 1023.0), 0L, 1023L));
}

bool closeToTenBit(std::uint32_t actual, std::uint16_t expected) noexcept
{
    return std::abs(
        static_cast<int>(actual) - static_cast<int>(expected))
        <= 4;
}

float halfValue(std::uint16_t value) noexcept
{
    const bool negative = (value & 0x8000U) != 0;
    const int exponent = static_cast<int>((value >> 10U) & 0x1fU);
    const int mantissa = static_cast<int>(value & 0x03ffU);
    float result = 0.0F;
    if (exponent == 0) {
        result = std::ldexp(static_cast<float>(mantissa), -24);
    } else if (exponent == 31) {
        result = mantissa == 0
            ? std::numeric_limits<float>::infinity()
            : std::numeric_limits<float>::quiet_NaN();
    } else {
        result = std::ldexp(
            1.0F + static_cast<float>(mantissa) / 1024.0F,
            exponent - 15);
    }
    return negative ? -result : result;
}

} // namespace

VideoFrame makeVulkanHdrTestFrame()
{
    return makeHdrVariant(HdrTransfer::PQ, 1000, 4000).frame;
}

bool runVulkanOffscreenRendererChecks(
    BorrowedVulkanDevice device,
    const VideoFrame& frame,
    std::string& error)
{
    if (!device.isValid() || !frame || frame.hasHardwareFrame()) {
        error = "The Vulkan offscreen check requires a software frame and device";
        return false;
    }
    const std::array<VkSurfaceFormatKHR, 3> surfaceFormats {
        VkSurfaceFormatKHR {
            VK_FORMAT_B8G8R8A8_UNORM,
            VK_COLOR_SPACE_SRGB_NONLINEAR_KHR,
        },
        VkSurfaceFormatKHR {
            VK_FORMAT_A2B10G10R10_UNORM_PACK32,
            VK_COLOR_SPACE_HDR10_HLG_EXT,
        },
        VkSurfaceFormatKHR {
            VK_FORMAT_A2B10G10R10_UNORM_PACK32,
            VK_COLOR_SPACE_HDR10_ST2084_EXT,
        },
    };
    const VkSurfaceFormatKHR preferred = selectVulkanSurfaceFormat(
        surfaceFormats.data(),
        surfaceFormats.size(),
        VulkanOutputPreference::PreferHdr);
    const VkSurfaceFormatKHR sdr = selectVulkanSurfaceFormat(
        surfaceFormats.data(),
        surfaceFormats.size(),
        VulkanOutputPreference::SdrOnly);
    const VkSurfaceFormatKHR unavailable = selectVulkanSurfaceFormat(
        surfaceFormats.data(),
        1,
        VulkanOutputPreference::RequireHdr);
    if (preferred.colorSpace != VK_COLOR_SPACE_HDR10_ST2084_EXT
        || sdr.colorSpace != VK_COLOR_SPACE_SRGB_NONLINEAR_KHR
        || unavailable.format != VK_FORMAT_UNDEFINED) {
        error = "Vulkan surface-format preference checks failed";
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

    const HdrVariant pqMastering =
        makeHdrVariant(HdrTransfer::PQ, 1000, 4000);
    const HdrVariant pqContent =
        makeHdrVariant(HdrTransfer::PQ, 0, 4000);
    const HdrVariant pqFallback =
        makeHdrVariant(HdrTransfer::PQ, 0, 0);
    const HdrVariant hlgMastering =
        makeHdrVariant(HdrTransfer::HLG, 1000, 0);
    if (!pqMastering.frame || !pqContent.frame || !pqFallback.frame
        || !hlgMastering.frame) {
        error = "Could not create Vulkan P010 HDR golden frames";
        return false;
    }
    const auto validateHdrMetadata =
        [&](const HdrVariant& variant,
            ColorTransfer transfer,
            bool expectsMastering,
            bool expectsContent,
            const char* name) {
            const VideoColorSpace color =
                variant.frame.colorSpaceInfo();
            const MasteringDisplayMetadata mastering =
                variant.frame.masteringDisplayMetadata();
            const ContentLightMetadata content =
                variant.frame.contentLightMetadata();
            if (variant.frame.format() != PixelFormat::P010
                || color.range != ColorRange::Limited
                || color.matrix != ColorMatrix::BT2020NCL
                || color.primaries != ColorPrimaries::BT2020
                || color.transfer != transfer
                || mastering.isValid() != expectsMastering
                || mastering.hasPrimaries != expectsMastering
                || mastering.hasLuminance != expectsMastering
                || content.isValid() != expectsContent) {
                error = std::string(
                    "Vulkan HDR metadata did not survive for ")
                    + name;
                return false;
            }
            return true;
        };
    if (!validateHdrMetadata(
            pqMastering,
            ColorTransfer::PQ,
            true,
            true,
            "PQ mastering precedence")
        || !validateHdrMetadata(
            pqContent,
            ColorTransfer::PQ,
            false,
            true,
            "PQ MaxCLL fallback")
        || !validateHdrMetadata(
            pqFallback,
            ColorTransfer::PQ,
            false,
            false,
            "PQ default luminance fallback")
        || !validateHdrMetadata(
            hlgMastering,
            ColorTransfer::HLG,
            true,
            false,
            "HLG mastering")) {
        return false;
    }

    const auto renderHdrVariant =
        [&](const HdrVariant& variant,
            HdrTransfer transfer,
            double maximumLuminance,
            const char* name) {
            if (!renderer->configure(config)
                || !renderer->render(variant.frame)
                || !target.readPixels(pixels, error)) {
                if (error.empty()) {
                    error = std::string("Vulkan HDR rendering failed for ")
                        + name;
                }
                return false;
            }
            constexpr std::array<int, 4> SampleX { 1, 3, 5, 7 };
            for (std::size_t index = 0; index < SampleX.size(); ++index) {
                const Pixel actual =
                    pixel(pixels, 8, SampleX[index], 4);
                const std::uint8_t expected = expectedHdrSdrCode(
                    variant.lumaCodes[index],
                    transfer,
                    maximumLuminance);
                if (!closeToCode(actual.red, expected)
                    || !closeToCode(actual.green, expected)
                    || !closeToCode(actual.blue, expected)
                    || actual.alpha < 239) {
                    error = std::string(
                        "Vulkan HDR-to-SDR numeric golden failed for ")
                        + name + " sample "
                        + std::to_string(index) + ": expected "
                        + std::to_string(expected) + ", got RGB("
                        + std::to_string(actual.red) + ','
                        + std::to_string(actual.green) + ','
                        + std::to_string(actual.blue) + ')';
                    return false;
                }
            }
            return true;
        };
    if (!renderHdrVariant(
            pqMastering,
            HdrTransfer::PQ,
            1000.0,
            "P010 BT.2020 PQ mastering")
        || !renderHdrVariant(
            pqContent,
            HdrTransfer::PQ,
            4000.0,
            "P010 BT.2020 PQ MaxCLL")
        || !renderHdrVariant(
            pqFallback,
            HdrTransfer::PQ,
            1000.0,
            "P010 BT.2020 PQ default")
        || !renderHdrVariant(
            hlgMastering,
            HdrTransfer::HLG,
            1000.0,
            "P010 BT.2020 HLG")) {
        return false;
    }

    const auto validateNativeHdrTarget =
        [&](const HdrVariant& variant,
            HdrTransfer sourceTransfer,
            VkColorSpaceKHR outputColorSpace,
            const char* name) {
            renderer->close();
            if (!target.create(
                    8,
                    8,
                    error,
                    VK_FORMAT_A2B10G10R10_UNORM_PACK32,
                    outputColorSpace)) {
                return false;
            }
            const VulkanRenderTarget renderTarget =
                target.renderTarget(true);
            if (!renderTarget.isValid() || !renderTarget.isHdr()) {
                error = std::string(
                    "Vulkan native HDR target contract failed for ")
                    + name;
                return false;
            }
            if (!renderer->open(config)
                || !renderer->render(variant.frame)
                || !target.readPixels(pixels, error)) {
                if (error.empty()) {
                    error = std::string(
                        "Vulkan native HDR rendering failed for ")
                        + name;
                }
                return false;
            }
            constexpr std::array<int, 4> SampleX { 1, 3, 5, 7 };
            for (std::size_t index = 0; index < SampleX.size(); ++index) {
                const Pixel packed =
                    pixel(pixels, 8, SampleX[index], 4);
                std::uint32_t word = 0;
                static_assert(sizeof(word) == sizeof(packed));
                std::memcpy(&word, &packed, sizeof(word));
                const std::uint16_t expected = expectedHdrOutputCode(
                    variant.lumaCodes[index],
                    sourceTransfer,
                    outputColorSpace);
                const std::uint32_t red = word & 1023U;
                const std::uint32_t green = (word >> 10U) & 1023U;
                const std::uint32_t blue = (word >> 20U) & 1023U;
                const std::uint32_t alpha = word >> 30U;
                if (!closeToTenBit(red, expected)
                    || !closeToTenBit(green, expected)
                    || !closeToTenBit(blue, expected)
                    || alpha != 3U) {
                    error = std::string(
                        "Vulkan native HDR numeric golden failed for ")
                        + name + " sample " + std::to_string(index)
                        + ": expected " + std::to_string(expected)
                        + ", got RGB10(" + std::to_string(red) + ','
                        + std::to_string(green) + ','
                        + std::to_string(blue) + ')';
                    return false;
                }
            }
            return true;
        };
    if (!validateNativeHdrTarget(
            pqMastering,
            HdrTransfer::PQ,
            VK_COLOR_SPACE_HDR10_ST2084_EXT,
            "P010 BT.2020 PQ to HDR10/PQ")
        || !validateNativeHdrTarget(
            hlgMastering,
            HdrTransfer::HLG,
            VK_COLOR_SPACE_HDR10_ST2084_EXT,
            "P010 BT.2020 HLG to HDR10/PQ")
        || !validateNativeHdrTarget(
            hlgMastering,
            HdrTransfer::HLG,
            VK_COLOR_SPACE_HDR10_HLG_EXT,
            "P010 BT.2020 HLG to native HLG")) {
        return false;
    }
    const auto validateLinearHdrTarget =
        [&](VkColorSpaceKHR outputColorSpace, const char* name) {
            renderer->close();
            if (!target.create(
                    8,
                    8,
                    error,
                    VK_FORMAT_R16G16B16A16_SFLOAT,
                    outputColorSpace)) {
                return false;
            }
            if (!target.renderTarget(true).isHdr()
                || !renderer->open(config)
                || !renderer->render(pqMastering.frame)) {
                if (error.empty()) {
                    error = std::string(
                        "Vulkan linear HDR rendering failed for ")
                        + name;
                }
                return false;
            }
            std::vector<HalfPixel> halfPixels;
            if (!target.readHalfPixels(halfPixels, error)) {
                return false;
            }
            constexpr std::array<int, 4> SampleX { 1, 3, 5, 7 };
            for (std::size_t index = 0; index < SampleX.size(); ++index) {
                const HalfPixel actual =
                    halfPixels[static_cast<std::size_t>(4) * 8
                        + static_cast<std::size_t>(SampleX[index])];
                const double signal = std::clamp(
                    (static_cast<double>(pqMastering.lumaCodes[index]) - 64.0)
                        / 876.0,
                    0.0,
                    1.0);
                const float expected =
                    static_cast<float>(pqNits(signal) / 100.0);
                const float tolerance =
                    std::max(0.03F, expected * 0.02F);
                const float red = halfValue(actual.red);
                const float green = halfValue(actual.green);
                const float blue = halfValue(actual.blue);
                const float alpha = halfValue(actual.alpha);
                if (std::abs(red - expected) > tolerance
                    || std::abs(green - expected) > tolerance
                    || std::abs(blue - expected) > tolerance
                    || std::abs(alpha - 1.0F) > 0.01F) {
                    error = std::string(
                        "Vulkan linear HDR numeric golden failed for ")
                        + name + " sample " + std::to_string(index)
                        + ": expected " + std::to_string(expected)
                        + ", got RGB(" + std::to_string(red) + ','
                        + std::to_string(green) + ','
                        + std::to_string(blue) + ')';
                    return false;
                }
            }
            return true;
        };
    if (!validateLinearHdrTarget(
            VK_COLOR_SPACE_BT2020_LINEAR_EXT,
            "P010 BT.2020 PQ to linear BT.2020")
        || !validateLinearHdrTarget(
            VK_COLOR_SPACE_EXTENDED_SRGB_LINEAR_EXT,
            "P010 BT.2020 PQ to extended linear sRGB")) {
        return false;
    }
    renderer->close();
    if (!target.create(8, 8, error) || !renderer->open(config)) {
        if (error.empty()) {
            error =
                "Could not restore the Vulkan SDR target after HDR checks";
        }
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
