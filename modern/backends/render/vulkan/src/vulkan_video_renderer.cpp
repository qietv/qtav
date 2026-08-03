// SPDX-License-Identifier: LGPL-2.1-or-later

#include <qtav/vulkan_video_renderer.h>

#include <libplacebo/log.h>
#include <libplacebo/renderer.h>
#include <libplacebo/vulkan.h>

#include <algorithm>
#include <array>
#include <cmath>
#include <cstdint>
#include <deque>
#include <limits>
#include <mutex>
#include <string>
#include <utility>

#include "frame_internal.h"
#include "qtav_libplacebo_ffmpeg_bridge.h"

#if defined(__ANDROID__)
#include "external_normalize_frag_spv.inc"
#include "external_normalize_vert_spv.inc"
#endif

namespace qtav {
namespace {

bool validViewport(
    const VideoViewport& viewport,
    const VideoSize& surface) noexcept
{
    if (!viewport.isValid()) {
        return true;
    }
    return viewport.x >= 0 && viewport.y >= 0
        && viewport.x <= surface.width - viewport.width
        && viewport.y <= surface.height - viewport.height;
}

bool supportedConfig(const VideoRenderConfig& config) noexcept
{
    return config.surfaceSize.isValid()
        && validViewport(config.viewport, config.surfaceSize)
        && config.deviceOwnership == NativeResourceOwnership::Borrowed
        && config.contextOwnership == NativeResourceOwnership::Borrowed
        && config.surfaceOwnership == NativeResourceOwnership::Borrowed;
}

bool sameDeviceContext(
    const BorrowedVulkanDevice& left,
    const BorrowedVulkanDevice& right) noexcept
{
    return left.instance == right.instance
        && left.physicalDevice == right.physicalDevice
        && left.device == right.device
        && left.queue == right.queue
        && left.queueFamilyIndex == right.queueFamilyIndex;
}

VideoViewport effectiveViewport(const VideoRenderConfig& config) noexcept
{
    return config.viewport.isValid()
        ? config.viewport
        : VideoViewport {
              0,
              0,
              config.surfaceSize.width,
              config.surfaceSize.height,
          };
}

bool isEightBitTargetFormat(VkFormat format) noexcept
{
    return format == VK_FORMAT_B8G8R8A8_UNORM
        || format == VK_FORMAT_B8G8R8A8_SRGB
        || format == VK_FORMAT_R8G8B8A8_UNORM
        || format == VK_FORMAT_R8G8B8A8_SRGB;
}

bool isTenBitTargetFormat(VkFormat format) noexcept
{
    return format == VK_FORMAT_A2B10G10R10_UNORM_PACK32
        || format == VK_FORMAT_A2R10G10B10_UNORM_PACK32;
}

bool isFloatTargetFormat(VkFormat format) noexcept
{
    return format == VK_FORMAT_R16G16B16A16_SFLOAT;
}

bool supportedTargetFormat(
    VkFormat format,
    VkColorSpaceKHR colorSpace) noexcept
{
    switch (colorSpace) {
    case VK_COLOR_SPACE_SRGB_NONLINEAR_KHR:
    case VK_COLOR_SPACE_BT709_NONLINEAR_EXT:
        return isEightBitTargetFormat(format);
    case VK_COLOR_SPACE_EXTENDED_SRGB_LINEAR_EXT:
    case VK_COLOR_SPACE_BT2020_LINEAR_EXT:
        return isFloatTargetFormat(format);
    case VK_COLOR_SPACE_HDR10_ST2084_EXT:
    case VK_COLOR_SPACE_HDR10_HLG_EXT:
        return isTenBitTargetFormat(format);
    default:
        return false;
    }
}

pl_rotation rotation(VideoRotation value) noexcept
{
    switch (value) {
    case VideoRotation::Rotate90:
        return PL_ROTATION_90;
    case VideoRotation::Rotate180:
        return PL_ROTATION_180;
    case VideoRotation::Rotate270:
        return PL_ROTATION_270;
    default:
        return PL_ROTATION_0;
    }
}

enum pl_color_levels levels(ColorRange value) noexcept
{
    switch (value) {
    case ColorRange::Limited:
        return PL_COLOR_LEVELS_LIMITED;
    case ColorRange::Full:
        return PL_COLOR_LEVELS_FULL;
    default:
        return PL_COLOR_LEVELS_UNKNOWN;
    }
}

enum pl_color_system system(ColorMatrix value) noexcept
{
    switch (value) {
    case ColorMatrix::RGB:
        return PL_COLOR_SYSTEM_RGB;
    case ColorMatrix::BT709:
        return PL_COLOR_SYSTEM_BT_709;
    case ColorMatrix::SMPTE240M:
        return PL_COLOR_SYSTEM_SMPTE_240M;
    case ColorMatrix::BT2020NCL:
        return PL_COLOR_SYSTEM_BT_2020_NC;
    case ColorMatrix::BT2020CL:
        return PL_COLOR_SYSTEM_BT_2020_C;
    case ColorMatrix::ICtCp:
        return PL_COLOR_SYSTEM_BT_2100_PQ;
    case ColorMatrix::YCgCo:
        return PL_COLOR_SYSTEM_YCGCO;
    default:
        return PL_COLOR_SYSTEM_BT_601;
    }
}

enum pl_color_primaries primaries(ColorPrimaries value) noexcept
{
    switch (value) {
    case ColorPrimaries::BT470M:
        return PL_COLOR_PRIM_BT_470M;
    case ColorPrimaries::BT470BG:
        return PL_COLOR_PRIM_BT_601_625;
    case ColorPrimaries::SMPTE170M:
    case ColorPrimaries::SMPTE240M:
        return PL_COLOR_PRIM_BT_601_525;
    case ColorPrimaries::BT2020:
        return PL_COLOR_PRIM_BT_2020;
    case ColorPrimaries::SMPTE431:
        return PL_COLOR_PRIM_DCI_P3;
    case ColorPrimaries::SMPTE432:
        return PL_COLOR_PRIM_DISPLAY_P3;
    case ColorPrimaries::EBU3213:
        return PL_COLOR_PRIM_EBU_3213;
    case ColorPrimaries::BT709:
    default:
        return PL_COLOR_PRIM_BT_709;
    }
}

enum pl_color_transfer transfer(ColorTransfer value) noexcept
{
    switch (value) {
    case ColorTransfer::Gamma22:
        return PL_COLOR_TRC_GAMMA22;
    case ColorTransfer::Gamma28:
        return PL_COLOR_TRC_GAMMA28;
    case ColorTransfer::Linear:
        return PL_COLOR_TRC_LINEAR;
    case ColorTransfer::SRGB:
        return PL_COLOR_TRC_SRGB;
    case ColorTransfer::PQ:
        return PL_COLOR_TRC_PQ;
    case ColorTransfer::HLG:
        return PL_COLOR_TRC_HLG;
    case ColorTransfer::SMPTE428:
        return PL_COLOR_TRC_ST428;
    default:
        return PL_COLOR_TRC_BT_1886;
    }
}

enum pl_chroma_location chromaLocation(ChromaLocation value) noexcept
{
    switch (value) {
    case ChromaLocation::Left:
        return PL_CHROMA_LEFT;
    case ChromaLocation::Center:
        return PL_CHROMA_CENTER;
    case ChromaLocation::TopLeft:
        return PL_CHROMA_TOP_LEFT;
    case ChromaLocation::Top:
        return PL_CHROMA_TOP_CENTER;
    case ChromaLocation::BottomLeft:
        return PL_CHROMA_BOTTOM_LEFT;
    case ChromaLocation::Bottom:
        return PL_CHROMA_BOTTOM_CENTER;
    default:
        return PL_CHROMA_UNKNOWN;
    }
}

void setHdrMetadata(
    const VideoFrame& frame,
    struct pl_hdr_metadata& destination) noexcept
{
    const MasteringDisplayMetadata mastering =
        frame.masteringDisplayMetadata();
    if (mastering.hasPrimaries) {
        destination.prim.red = {
            static_cast<float>(mastering.primaries[0].x),
            static_cast<float>(mastering.primaries[0].y),
        };
        destination.prim.green = {
            static_cast<float>(mastering.primaries[1].x),
            static_cast<float>(mastering.primaries[1].y),
        };
        destination.prim.blue = {
            static_cast<float>(mastering.primaries[2].x),
            static_cast<float>(mastering.primaries[2].y),
        };
        destination.prim.white = {
            static_cast<float>(mastering.whitePoint.x),
            static_cast<float>(mastering.whitePoint.y),
        };
    }
    if (mastering.hasLuminance) {
        destination.min_luma = static_cast<float>(
            std::max(mastering.minimumLuminance, 0.000001));
        destination.max_luma = static_cast<float>(
            mastering.maximumLuminance);
    }
    const ContentLightMetadata light = frame.contentLightMetadata();
    destination.max_cll = static_cast<float>(
        light.maximumContentLightLevel);
    destination.max_fall = static_cast<float>(
        light.maximumFrameAverageLightLevel);
}

void setSourceColor(const VideoFrame& source, struct pl_frame& frame) noexcept
{
    const VideoColorSpace color = source.colorSpaceInfo();
    frame.repr.sys = system(color.matrix);
    frame.repr.levels = levels(color.range);
    frame.repr.alpha = PL_ALPHA_NONE;
    frame.color.primaries = primaries(color.primaries);
    frame.color.transfer = transfer(color.transfer);
    setHdrMetadata(source, frame.color.hdr);
    pl_frame_set_chroma_location(&frame, chromaLocation(color.chromaLocation));
}

void setTargetColor(
    const VulkanRenderTarget& source,
    struct pl_frame& target) noexcept
{
    target.repr.sys = PL_COLOR_SYSTEM_RGB;
    target.repr.levels = PL_COLOR_LEVELS_FULL;
    target.repr.alpha = PL_ALPHA_NONE;
    switch (source.colorSpace) {
    case VK_COLOR_SPACE_HDR10_ST2084_EXT:
        target.color = pl_color_space_hdr10;
        break;
    case VK_COLOR_SPACE_HDR10_HLG_EXT:
        target.color = pl_color_space_bt2020_hlg;
        break;
    case VK_COLOR_SPACE_BT2020_LINEAR_EXT:
        target.color.primaries = PL_COLOR_PRIM_BT_2020;
        target.color.transfer = PL_COLOR_TRC_LINEAR;
        target.color.hdr = pl_hdr_metadata_hdr10;
        break;
    case VK_COLOR_SPACE_EXTENDED_SRGB_LINEAR_EXT:
        target.color.primaries = PL_COLOR_PRIM_BT_709;
        target.color.transfer = PL_COLOR_TRC_LINEAR;
        target.color.hdr = pl_hdr_metadata_hdr10;
        break;
    default:
        target.color = pl_color_space_srgb;
        break;
    }
    if (isTenBitTargetFormat(source.format)) {
        target.repr.bits = { 10, 10, 0 };
    } else if (isFloatTargetFormat(source.format)) {
        target.repr.bits = { 16, 16, 0 };
    } else {
        target.repr.bits = { 8, 8, 0 };
    }
}

void applyGeometry(
    struct pl_frame& image,
    struct pl_frame& target,
    const VideoRenderConfig& config) noexcept
{
    image.rotation = rotation(config.rotation);
    const VideoViewport viewport = effectiveViewport(config);
    target.crop = {
        static_cast<float>(viewport.x),
        static_cast<float>(viewport.y),
        static_cast<float>(viewport.x + viewport.width),
        static_cast<float>(viewport.y + viewport.height),
    };
    if (config.aspectRatio == VideoAspectRatioMode::Stretch) {
        return;
    }
    if (config.aspectRatio == VideoAspectRatioMode::Fit) {
        const float sourceAspect = pl_aspect_rotate(
            pl_rect2df_aspect(&image.crop),
            image.rotation);
        pl_rect2df_aspect_set(&target.crop, sourceAspect, 0.0F);
        return;
    }
    const float targetAspect = pl_rect2df_aspect(&target.crop);
    const float sourceAspect = pl_aspect_rotate(
        targetAspect,
        image.rotation);
    pl_rect2df_aspect_set(&image.crop, sourceAspect, 0.0F);
}

void initializePlane(
    struct pl_plane& plane,
    pl_tex texture,
    int components,
    int firstComponent) noexcept
{
    plane.texture = texture;
    plane.components = components;
    std::fill(
        std::begin(plane.component_mapping),
        std::end(plane.component_mapping),
        -1);
    for (int component = 0; component < components; ++component) {
        plane.component_mapping[component] = firstComponent + component;
    }
}

std::string vkResult(const char* operation, VkResult result)
{
    return std::string(operation) + " failed ("
        + std::to_string(static_cast<int>(result)) + ')';
}

#if defined(__ANDROID__)
class ExternalImageNormalizer {
public:
    explicit ExternalImageNormalizer(BorrowedVulkanDevice device)
        : device_(device)
    {
    }

    ~ExternalImageNormalizer()
    {
        destroy();
    }

    VkImage image() const noexcept
    {
        return image_;
    }

    VkFormat format() const noexcept
    {
        return VK_FORMAT_R16G16B16A16_SFLOAT;
    }

    bool convert(
        const std::shared_ptr<VulkanTextureFrame>& source,
        int width,
        int height,
        bool preserveYcbcr,
        std::string& error)
    {
        const VkImageView sourceView = preserveYcbcr
            ? source ? source->unconvertedImageView() : VK_NULL_HANDLE
            : source ? source->imageView() : VK_NULL_HANDLE;
        const VkSampler sourceSampler = preserveYcbcr
            ? source ? source->unconvertedSampler() : VK_NULL_HANDLE
            : source ? source->sampler() : VK_NULL_HANDLE;
        if (!source || !source->image() || !sourceView
            || !sourceSampler || width <= 0 || height <= 0) {
            error =
                preserveYcbcr
                ? "The Android external-format image cannot expose unconverted YCbCr for Dolby Vision"
                : "The Android external-format image is incomplete";
            return false;
        }
        if (!ensure(source, sourceSampler, width, height, error)) {
            return false;
        }
        const VulkanNormalizedSourceRect crop =
            source->normalizedSourceRect();
        if (!crop.isValid()) {
            error = "The Android external-format image has an invalid crop";
            return false;
        }

        const VkDescriptorImageInfo imageInfo {
            sourceSampler,
            sourceView,
            source->sampledLayout(),
        };
        VkWriteDescriptorSet write {
            VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET,
        };
        write.dstSet = descriptorSet_;
        write.dstBinding = 0;
        write.descriptorCount = 1;
        write.descriptorType =
            VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
        write.pImageInfo = &imageInfo;
        vkUpdateDescriptorSets(
            device_.device,
            1,
            &write,
            0,
            nullptr);

        VkResult result = vkResetFences(
            device_.device,
            1,
            &fence_);
        if (result == VK_SUCCESS) {
            result = vkResetCommandBuffer(commandBuffer_, 0);
        }
        if (result != VK_SUCCESS) {
            error = vkResult("reset external normalization", result);
            return false;
        }
        VkCommandBufferBeginInfo beginInfo {
            VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO,
        };
        beginInfo.flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT;
        result = vkBeginCommandBuffer(commandBuffer_, &beginInfo);
        if (result != VK_SUCCESS) {
            error = vkResult("vkBeginCommandBuffer(external)", result);
            return false;
        }

        VkImageMemoryBarrier sourceAcquire {
            VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER,
        };
        sourceAcquire.dstAccessMask = VK_ACCESS_SHADER_READ_BIT;
        sourceAcquire.oldLayout = source->initialLayout();
        sourceAcquire.newLayout = source->sampledLayout();
        sourceAcquire.srcQueueFamilyIndex =
            source->sourceQueueFamilyIndex();
        sourceAcquire.dstQueueFamilyIndex = device_.queueFamilyIndex;
        sourceAcquire.image = source->image();
        sourceAcquire.subresourceRange.aspectMask =
            VK_IMAGE_ASPECT_COLOR_BIT;
        sourceAcquire.subresourceRange.levelCount = 1;
        sourceAcquire.subresourceRange.layerCount = 1;

        VkImageMemoryBarrier outputAcquire {
            VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER,
        };
        outputAcquire.srcAccessMask = initialized_
            ? VK_ACCESS_SHADER_READ_BIT
            : 0;
        outputAcquire.dstAccessMask =
            VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT;
        outputAcquire.oldLayout = initialized_
            ? VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL
            : VK_IMAGE_LAYOUT_UNDEFINED;
        outputAcquire.newLayout =
            VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL;
        outputAcquire.srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
        outputAcquire.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
        outputAcquire.image = image_;
        outputAcquire.subresourceRange.aspectMask =
            VK_IMAGE_ASPECT_COLOR_BIT;
        outputAcquire.subresourceRange.levelCount = 1;
        outputAcquire.subresourceRange.layerCount = 1;
        const std::array<VkImageMemoryBarrier, 2> acquireBarriers {
            sourceAcquire,
            outputAcquire,
        };
        vkCmdPipelineBarrier(
            commandBuffer_,
            initialized_
                ? VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT
                : VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT,
            VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT
                | VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT,
            0,
            0,
            nullptr,
            0,
            nullptr,
            static_cast<std::uint32_t>(acquireBarriers.size()),
            acquireBarriers.data());

        const VkClearValue clear { { { 0.0F, 0.0F, 0.0F, 1.0F } } };
        VkRenderPassBeginInfo passInfo {
            VK_STRUCTURE_TYPE_RENDER_PASS_BEGIN_INFO,
        };
        passInfo.renderPass = renderPass_;
        passInfo.framebuffer = framebuffer_;
        passInfo.renderArea.extent = {
            static_cast<std::uint32_t>(width_),
            static_cast<std::uint32_t>(height_),
        };
        passInfo.clearValueCount = 1;
        passInfo.pClearValues = &clear;
        vkCmdBeginRenderPass(
            commandBuffer_,
            &passInfo,
            VK_SUBPASS_CONTENTS_INLINE);
        vkCmdBindPipeline(
            commandBuffer_,
            VK_PIPELINE_BIND_POINT_GRAPHICS,
            pipeline_);
        vkCmdBindDescriptorSets(
            commandBuffer_,
            VK_PIPELINE_BIND_POINT_GRAPHICS,
            pipelineLayout_,
            0,
            1,
            &descriptorSet_,
            0,
            nullptr);
        const std::array<float, 4> sourceRect {
            crop.left,
            crop.top,
            crop.right,
            crop.bottom,
        };
        vkCmdPushConstants(
            commandBuffer_,
            pipelineLayout_,
            VK_SHADER_STAGE_FRAGMENT_BIT,
            0,
            sizeof(sourceRect),
            sourceRect.data());
        const VkViewport viewport {
            0.0F,
            0.0F,
            static_cast<float>(width_),
            static_cast<float>(height_),
            0.0F,
            1.0F,
        };
        const VkRect2D scissor {
            { 0, 0 },
            {
                static_cast<std::uint32_t>(width_),
                static_cast<std::uint32_t>(height_),
            },
        };
        vkCmdSetViewport(commandBuffer_, 0, 1, &viewport);
        vkCmdSetScissor(commandBuffer_, 0, 1, &scissor);
        vkCmdDraw(commandBuffer_, 3, 1, 0, 0);
        vkCmdEndRenderPass(commandBuffer_);

        VkImageMemoryBarrier sourceRelease {
            VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER,
        };
        sourceRelease.srcAccessMask = VK_ACCESS_SHADER_READ_BIT;
        sourceRelease.oldLayout = source->sampledLayout();
        sourceRelease.newLayout = source->releaseLayout();
        sourceRelease.srcQueueFamilyIndex = device_.queueFamilyIndex;
        sourceRelease.dstQueueFamilyIndex =
            source->sourceQueueFamilyIndex();
        sourceRelease.image = source->image();
        sourceRelease.subresourceRange =
            sourceAcquire.subresourceRange;
        VkImageMemoryBarrier outputRelease {
            VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER,
        };
        outputRelease.srcAccessMask =
            VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT;
        outputRelease.dstAccessMask = VK_ACCESS_SHADER_READ_BIT;
        outputRelease.oldLayout =
            VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL;
        outputRelease.newLayout =
            VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;
        outputRelease.srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
        outputRelease.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
        outputRelease.image = image_;
        outputRelease.subresourceRange =
            outputAcquire.subresourceRange;
        const std::array<VkImageMemoryBarrier, 2> releaseBarriers {
            sourceRelease,
            outputRelease,
        };
        vkCmdPipelineBarrier(
            commandBuffer_,
            VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT
                | VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT,
            VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT
                | VK_PIPELINE_STAGE_BOTTOM_OF_PIPE_BIT,
            0,
            0,
            nullptr,
            0,
            nullptr,
            static_cast<std::uint32_t>(releaseBarriers.size()),
            releaseBarriers.data());
        result = vkEndCommandBuffer(commandBuffer_);
        if (result != VK_SUCCESS) {
            error = vkResult("vkEndCommandBuffer(external)", result);
            return false;
        }

        VkSubmitInfo submit {
            VK_STRUCTURE_TYPE_SUBMIT_INFO,
        };
        VkPipelineStageFlags waitStage =
            VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT;
        const VkSemaphore acquire = source->acquireSemaphore();
        if (acquire) {
            submit.waitSemaphoreCount = 1;
            submit.pWaitSemaphores = &acquire;
            submit.pWaitDstStageMask = &waitStage;
        }
        submit.commandBufferCount = 1;
        submit.pCommandBuffers = &commandBuffer_;
        result = vkQueueSubmit(
            device_.queue,
            1,
            &submit,
            fence_);
        if (result == VK_SUCCESS) {
            result = vkWaitForFences(
                device_.device,
                1,
                &fence_,
                VK_TRUE,
                5'000'000'000ULL);
        }
        if (result != VK_SUCCESS) {
            error = vkResult("external image normalization", result);
            return false;
        }
        initialized_ = true;
        source->releaseToProducer();
        return true;
    }

    void finish() noexcept
    {
        if (image_ && device_.queue) {
            vkQueueWaitIdle(device_.queue);
        }
    }

private:
    std::uint32_t memoryType(
        std::uint32_t bits,
        VkMemoryPropertyFlags preferred) const noexcept
    {
        VkPhysicalDeviceMemoryProperties properties {};
        vkGetPhysicalDeviceMemoryProperties(
            device_.physicalDevice,
            &properties);
        for (std::uint32_t index = 0;
             index < properties.memoryTypeCount;
             ++index) {
            if ((bits & (1U << index)) != 0U
                && (properties.memoryTypes[index].propertyFlags & preferred)
                    == preferred) {
                return index;
            }
        }
        return std::numeric_limits<std::uint32_t>::max();
    }

    bool ensure(
        const std::shared_ptr<VulkanTextureFrame>& source,
        VkSampler sourceSampler,
        int width,
        int height,
        std::string& error)
    {
        if (image_ && width_ == width && height_ == height
            && sampler_ == sourceSampler) {
            return true;
        }
        if (device_.queue) {
            vkQueueWaitIdle(device_.queue);
        }
        destroy();
        width_ = width;
        height_ = height;
        sampler_ = sourceSampler;
        samplerOwner_ = source;

        VkImageCreateInfo imageInfo {
            VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO,
        };
        imageInfo.imageType = VK_IMAGE_TYPE_2D;
        imageInfo.format = format();
        imageInfo.extent = {
            static_cast<std::uint32_t>(width),
            static_cast<std::uint32_t>(height),
            1,
        };
        imageInfo.mipLevels = 1;
        imageInfo.arrayLayers = 1;
        imageInfo.samples = VK_SAMPLE_COUNT_1_BIT;
        imageInfo.tiling = VK_IMAGE_TILING_OPTIMAL;
        imageInfo.usage = VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT
            | VK_IMAGE_USAGE_SAMPLED_BIT;
        VkResult result = vkCreateImage(
            device_.device,
            &imageInfo,
            nullptr,
            &image_);
        if (result != VK_SUCCESS) {
            error = vkResult("vkCreateImage(external normalized)", result);
            return false;
        }
        VkMemoryRequirements requirements {};
        vkGetImageMemoryRequirements(
            device_.device,
            image_,
            &requirements);
        const std::uint32_t type = memoryType(
            requirements.memoryTypeBits,
            VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT);
        if (type == std::numeric_limits<std::uint32_t>::max()) {
            error = "No device-local memory for external normalization";
            return false;
        }
        VkMemoryAllocateInfo allocation {
            VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO,
        };
        allocation.allocationSize = requirements.size;
        allocation.memoryTypeIndex = type;
        result = vkAllocateMemory(
            device_.device,
            &allocation,
            nullptr,
            &memory_);
        if (result == VK_SUCCESS) {
            result = vkBindImageMemory(
                device_.device,
                image_,
                memory_,
                0);
        }
        if (result != VK_SUCCESS) {
            error = vkResult("allocate external normalized image", result);
            return false;
        }
        VkImageViewCreateInfo viewInfo {
            VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO,
        };
        viewInfo.image = image_;
        viewInfo.viewType = VK_IMAGE_VIEW_TYPE_2D;
        viewInfo.format = format();
        viewInfo.subresourceRange.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
        viewInfo.subresourceRange.levelCount = 1;
        viewInfo.subresourceRange.layerCount = 1;
        result = vkCreateImageView(
            device_.device,
            &viewInfo,
            nullptr,
            &imageView_);
        if (result != VK_SUCCESS) {
            error = vkResult("vkCreateImageView(external normalized)", result);
            return false;
        }

        VkAttachmentDescription attachment {};
        attachment.format = format();
        attachment.samples = VK_SAMPLE_COUNT_1_BIT;
        attachment.loadOp = VK_ATTACHMENT_LOAD_OP_CLEAR;
        attachment.storeOp = VK_ATTACHMENT_STORE_OP_STORE;
        attachment.initialLayout =
            VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL;
        attachment.finalLayout =
            VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL;
        VkAttachmentReference reference {
            0,
            VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL,
        };
        VkSubpassDescription subpass {};
        subpass.pipelineBindPoint = VK_PIPELINE_BIND_POINT_GRAPHICS;
        subpass.colorAttachmentCount = 1;
        subpass.pColorAttachments = &reference;
        VkRenderPassCreateInfo renderPassInfo {
            VK_STRUCTURE_TYPE_RENDER_PASS_CREATE_INFO,
        };
        renderPassInfo.attachmentCount = 1;
        renderPassInfo.pAttachments = &attachment;
        renderPassInfo.subpassCount = 1;
        renderPassInfo.pSubpasses = &subpass;
        result = vkCreateRenderPass(
            device_.device,
            &renderPassInfo,
            nullptr,
            &renderPass_);
        if (result != VK_SUCCESS) {
            error = vkResult("vkCreateRenderPass(external)", result);
            return false;
        }

        VkDescriptorSetLayoutBinding binding {};
        binding.binding = 0;
        binding.descriptorType =
            VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
        binding.descriptorCount = 1;
        binding.stageFlags = VK_SHADER_STAGE_FRAGMENT_BIT;
        binding.pImmutableSamplers = &sampler_;
        VkDescriptorSetLayoutCreateInfo descriptorInfo {
            VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO,
        };
        descriptorInfo.bindingCount = 1;
        descriptorInfo.pBindings = &binding;
        result = vkCreateDescriptorSetLayout(
            device_.device,
            &descriptorInfo,
            nullptr,
            &descriptorSetLayout_);
        if (result != VK_SUCCESS) {
            error = vkResult("vkCreateDescriptorSetLayout(external)", result);
            return false;
        }
        VkPushConstantRange pushRange {};
        pushRange.stageFlags = VK_SHADER_STAGE_FRAGMENT_BIT;
        pushRange.size = sizeof(float) * 4;
        VkPipelineLayoutCreateInfo layoutInfo {
            VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO,
        };
        layoutInfo.setLayoutCount = 1;
        layoutInfo.pSetLayouts = &descriptorSetLayout_;
        layoutInfo.pushConstantRangeCount = 1;
        layoutInfo.pPushConstantRanges = &pushRange;
        result = vkCreatePipelineLayout(
            device_.device,
            &layoutInfo,
            nullptr,
            &pipelineLayout_);
        if (result != VK_SUCCESS
            || !createPipeline(error)) {
            if (result != VK_SUCCESS) {
                error = vkResult("vkCreatePipelineLayout(external)", result);
            }
            return false;
        }

        VkDescriptorPoolSize poolSize {
            VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER,
            1,
        };
        VkDescriptorPoolCreateInfo poolInfo {
            VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO,
        };
        poolInfo.maxSets = 1;
        poolInfo.poolSizeCount = 1;
        poolInfo.pPoolSizes = &poolSize;
        result = vkCreateDescriptorPool(
            device_.device,
            &poolInfo,
            nullptr,
            &descriptorPool_);
        if (result != VK_SUCCESS) {
            error = vkResult("vkCreateDescriptorPool(external)", result);
            return false;
        }
        VkDescriptorSetAllocateInfo setInfo {
            VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO,
        };
        setInfo.descriptorPool = descriptorPool_;
        setInfo.descriptorSetCount = 1;
        setInfo.pSetLayouts = &descriptorSetLayout_;
        result = vkAllocateDescriptorSets(
            device_.device,
            &setInfo,
            &descriptorSet_);
        if (result != VK_SUCCESS) {
            error = vkResult("vkAllocateDescriptorSets(external)", result);
            return false;
        }

        VkFramebufferCreateInfo framebufferInfo {
            VK_STRUCTURE_TYPE_FRAMEBUFFER_CREATE_INFO,
        };
        framebufferInfo.renderPass = renderPass_;
        framebufferInfo.attachmentCount = 1;
        framebufferInfo.pAttachments = &imageView_;
        framebufferInfo.width = static_cast<std::uint32_t>(width_);
        framebufferInfo.height = static_cast<std::uint32_t>(height_);
        framebufferInfo.layers = 1;
        result = vkCreateFramebuffer(
            device_.device,
            &framebufferInfo,
            nullptr,
            &framebuffer_);
        if (result != VK_SUCCESS) {
            error = vkResult("vkCreateFramebuffer(external)", result);
            return false;
        }
        VkCommandPoolCreateInfo commandPoolInfo {
            VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO,
        };
        commandPoolInfo.flags =
            VK_COMMAND_POOL_CREATE_RESET_COMMAND_BUFFER_BIT;
        commandPoolInfo.queueFamilyIndex = device_.queueFamilyIndex;
        result = vkCreateCommandPool(
            device_.device,
            &commandPoolInfo,
            nullptr,
            &commandPool_);
        if (result == VK_SUCCESS) {
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
        }
        if (result == VK_SUCCESS) {
            VkFenceCreateInfo fenceInfo {
                VK_STRUCTURE_TYPE_FENCE_CREATE_INFO,
            };
            result = vkCreateFence(
                device_.device,
                &fenceInfo,
                nullptr,
                &fence_);
        }
        if (result != VK_SUCCESS) {
            error = vkResult("create external command resources", result);
            return false;
        }
        return true;
    }

    bool createPipeline(std::string& error)
    {
        const auto createShader = [&](const unsigned char* bytes,
                                      std::size_t size,
                                      VkShaderModule& shader) {
            VkShaderModuleCreateInfo info {
                VK_STRUCTURE_TYPE_SHADER_MODULE_CREATE_INFO,
            };
            info.codeSize = size;
            info.pCode = reinterpret_cast<const std::uint32_t*>(bytes);
            return vkCreateShaderModule(
                device_.device,
                &info,
                nullptr,
                &shader);
        };
        VkShaderModule vertex = VK_NULL_HANDLE;
        VkShaderModule fragment = VK_NULL_HANDLE;
        VkResult result = createShader(
            external_normalize_vert_spv,
            sizeof(external_normalize_vert_spv),
            vertex);
        if (result == VK_SUCCESS) {
            result = createShader(
                external_normalize_frag_spv,
                sizeof(external_normalize_frag_spv),
                fragment);
        }
        if (result != VK_SUCCESS) {
            if (vertex) {
                vkDestroyShaderModule(device_.device, vertex, nullptr);
            }
            error = vkResult("vkCreateShaderModule(external)", result);
            return false;
        }
        std::array<VkPipelineShaderStageCreateInfo, 2> stages {};
        stages[0].sType =
            VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO;
        stages[0].stage = VK_SHADER_STAGE_VERTEX_BIT;
        stages[0].module = vertex;
        stages[0].pName = "main";
        stages[1].sType = stages[0].sType;
        stages[1].stage = VK_SHADER_STAGE_FRAGMENT_BIT;
        stages[1].module = fragment;
        stages[1].pName = "main";
        VkPipelineVertexInputStateCreateInfo vertexInput {
            VK_STRUCTURE_TYPE_PIPELINE_VERTEX_INPUT_STATE_CREATE_INFO,
        };
        VkPipelineInputAssemblyStateCreateInfo assembly {
            VK_STRUCTURE_TYPE_PIPELINE_INPUT_ASSEMBLY_STATE_CREATE_INFO,
        };
        assembly.topology = VK_PRIMITIVE_TOPOLOGY_TRIANGLE_LIST;
        VkPipelineViewportStateCreateInfo viewport {
            VK_STRUCTURE_TYPE_PIPELINE_VIEWPORT_STATE_CREATE_INFO,
        };
        viewport.viewportCount = 1;
        viewport.scissorCount = 1;
        VkPipelineRasterizationStateCreateInfo rasterization {
            VK_STRUCTURE_TYPE_PIPELINE_RASTERIZATION_STATE_CREATE_INFO,
        };
        rasterization.polygonMode = VK_POLYGON_MODE_FILL;
        rasterization.cullMode = VK_CULL_MODE_NONE;
        rasterization.frontFace = VK_FRONT_FACE_COUNTER_CLOCKWISE;
        rasterization.lineWidth = 1.0F;
        VkPipelineMultisampleStateCreateInfo multisample {
            VK_STRUCTURE_TYPE_PIPELINE_MULTISAMPLE_STATE_CREATE_INFO,
        };
        multisample.rasterizationSamples = VK_SAMPLE_COUNT_1_BIT;
        VkPipelineColorBlendAttachmentState blendAttachment {};
        blendAttachment.colorWriteMask = VK_COLOR_COMPONENT_R_BIT
            | VK_COLOR_COMPONENT_G_BIT | VK_COLOR_COMPONENT_B_BIT
            | VK_COLOR_COMPONENT_A_BIT;
        VkPipelineColorBlendStateCreateInfo blend {
            VK_STRUCTURE_TYPE_PIPELINE_COLOR_BLEND_STATE_CREATE_INFO,
        };
        blend.attachmentCount = 1;
        blend.pAttachments = &blendAttachment;
        const std::array<VkDynamicState, 2> dynamicStates {
            VK_DYNAMIC_STATE_VIEWPORT,
            VK_DYNAMIC_STATE_SCISSOR,
        };
        VkPipelineDynamicStateCreateInfo dynamic {
            VK_STRUCTURE_TYPE_PIPELINE_DYNAMIC_STATE_CREATE_INFO,
        };
        dynamic.dynamicStateCount =
            static_cast<std::uint32_t>(dynamicStates.size());
        dynamic.pDynamicStates = dynamicStates.data();
        VkGraphicsPipelineCreateInfo info {
            VK_STRUCTURE_TYPE_GRAPHICS_PIPELINE_CREATE_INFO,
        };
        info.stageCount = static_cast<std::uint32_t>(stages.size());
        info.pStages = stages.data();
        info.pVertexInputState = &vertexInput;
        info.pInputAssemblyState = &assembly;
        info.pViewportState = &viewport;
        info.pRasterizationState = &rasterization;
        info.pMultisampleState = &multisample;
        info.pColorBlendState = &blend;
        info.pDynamicState = &dynamic;
        info.layout = pipelineLayout_;
        info.renderPass = renderPass_;
        result = vkCreateGraphicsPipelines(
            device_.device,
            VK_NULL_HANDLE,
            1,
            &info,
            nullptr,
            &pipeline_);
        vkDestroyShaderModule(device_.device, fragment, nullptr);
        vkDestroyShaderModule(device_.device, vertex, nullptr);
        if (result != VK_SUCCESS) {
            error = vkResult("vkCreateGraphicsPipelines(external)", result);
            return false;
        }
        return true;
    }

    void destroy() noexcept
    {
        if (!device_.device) {
            return;
        }
        if (fence_) vkDestroyFence(device_.device, fence_, nullptr);
        if (commandPool_) {
            vkDestroyCommandPool(device_.device, commandPool_, nullptr);
        }
        if (framebuffer_) {
            vkDestroyFramebuffer(device_.device, framebuffer_, nullptr);
        }
        if (descriptorPool_) {
            vkDestroyDescriptorPool(device_.device, descriptorPool_, nullptr);
        }
        if (pipeline_) vkDestroyPipeline(device_.device, pipeline_, nullptr);
        if (pipelineLayout_) {
            vkDestroyPipelineLayout(device_.device, pipelineLayout_, nullptr);
        }
        if (descriptorSetLayout_) {
            vkDestroyDescriptorSetLayout(
                device_.device,
                descriptorSetLayout_,
                nullptr);
        }
        if (renderPass_) {
            vkDestroyRenderPass(device_.device, renderPass_, nullptr);
        }
        if (imageView_) {
            vkDestroyImageView(device_.device, imageView_, nullptr);
        }
        if (image_) vkDestroyImage(device_.device, image_, nullptr);
        if (memory_) vkFreeMemory(device_.device, memory_, nullptr);
        fence_ = VK_NULL_HANDLE;
        commandPool_ = VK_NULL_HANDLE;
        commandBuffer_ = VK_NULL_HANDLE;
        framebuffer_ = VK_NULL_HANDLE;
        descriptorPool_ = VK_NULL_HANDLE;
        descriptorSet_ = VK_NULL_HANDLE;
        pipeline_ = VK_NULL_HANDLE;
        pipelineLayout_ = VK_NULL_HANDLE;
        descriptorSetLayout_ = VK_NULL_HANDLE;
        renderPass_ = VK_NULL_HANDLE;
        imageView_ = VK_NULL_HANDLE;
        image_ = VK_NULL_HANDLE;
        memory_ = VK_NULL_HANDLE;
        sampler_ = VK_NULL_HANDLE;
        samplerOwner_.reset();
        width_ = 0;
        height_ = 0;
        initialized_ = false;
    }

    BorrowedVulkanDevice device_;
    int width_ = 0;
    int height_ = 0;
    bool initialized_ = false;
    VkSampler sampler_ = VK_NULL_HANDLE;
    std::shared_ptr<VulkanTextureFrame> samplerOwner_;
    VkImage image_ = VK_NULL_HANDLE;
    VkDeviceMemory memory_ = VK_NULL_HANDLE;
    VkImageView imageView_ = VK_NULL_HANDLE;
    VkRenderPass renderPass_ = VK_NULL_HANDLE;
    VkDescriptorSetLayout descriptorSetLayout_ = VK_NULL_HANDLE;
    VkPipelineLayout pipelineLayout_ = VK_NULL_HANDLE;
    VkPipeline pipeline_ = VK_NULL_HANDLE;
    VkDescriptorPool descriptorPool_ = VK_NULL_HANDLE;
    VkDescriptorSet descriptorSet_ = VK_NULL_HANDLE;
    VkFramebuffer framebuffer_ = VK_NULL_HANDLE;
    VkCommandPool commandPool_ = VK_NULL_HANDLE;
    VkCommandBuffer commandBuffer_ = VK_NULL_HANDLE;
    VkFence fence_ = VK_NULL_HANDLE;
};
#endif

} // namespace

bool BorrowedVulkanDevice::isValid() const noexcept
{
    return instance != VK_NULL_HANDLE
        && physicalDevice != VK_NULL_HANDLE
        && device != VK_NULL_HANDLE
        && queue != VK_NULL_HANDLE;
}

bool VulkanRenderTarget::isValid() const noexcept
{
    return image != VK_NULL_HANDLE
        && imageView != VK_NULL_HANDLE
        && format != VK_FORMAT_UNDEFINED
        && extent.width > 0 && extent.height > 0
        && supportedTargetFormat(format, colorSpace);
}

bool VulkanRenderTarget::isHdr() const noexcept
{
    return vulkanColorSpaceIsHdr(colorSpace);
}

bool vulkanColorSpaceIsHdr(VkColorSpaceKHR colorSpace) noexcept
{
    return colorSpace == VK_COLOR_SPACE_EXTENDED_SRGB_LINEAR_EXT
        || colorSpace == VK_COLOR_SPACE_BT2020_LINEAR_EXT
        || colorSpace == VK_COLOR_SPACE_HDR10_ST2084_EXT
        || colorSpace == VK_COLOR_SPACE_HDR10_HLG_EXT;
}

VkSurfaceFormatKHR selectVulkanSurfaceFormat(
    const VkSurfaceFormatKHR* formats,
    std::size_t count,
    VulkanOutputPreference preference) noexcept
{
    if (!formats || count == 0) {
        return { VK_FORMAT_UNDEFINED, VK_COLOR_SPACE_SRGB_NONLINEAR_KHR };
    }
    const auto find = [&](VkColorSpaceKHR colorSpace, auto predicate) {
        for (std::size_t index = 0; index < count; ++index) {
            if (formats[index].colorSpace == colorSpace
                && predicate(formats[index].format)
                && supportedTargetFormat(
                    formats[index].format,
                    formats[index].colorSpace)) {
                return formats[index];
            }
        }
        return VkSurfaceFormatKHR {
            VK_FORMAT_UNDEFINED,
            VK_COLOR_SPACE_SRGB_NONLINEAR_KHR,
        };
    };
    if (preference != VulkanOutputPreference::SdrOnly) {
        VkSurfaceFormatKHR selected = find(
            VK_COLOR_SPACE_HDR10_ST2084_EXT,
            isTenBitTargetFormat);
        if (selected.format != VK_FORMAT_UNDEFINED) {
            return selected;
        }
        selected = find(
            VK_COLOR_SPACE_HDR10_HLG_EXT,
            isTenBitTargetFormat);
        if (selected.format != VK_FORMAT_UNDEFINED) {
            return selected;
        }
        selected = find(
            VK_COLOR_SPACE_EXTENDED_SRGB_LINEAR_EXT,
            isFloatTargetFormat);
        if (selected.format != VK_FORMAT_UNDEFINED) {
            return selected;
        }
        selected = find(
            VK_COLOR_SPACE_BT2020_LINEAR_EXT,
            isFloatTargetFormat);
        if (selected.format != VK_FORMAT_UNDEFINED) {
            return selected;
        }
        if (preference == VulkanOutputPreference::RequireHdr) {
            return selected;
        }
    }
    VkSurfaceFormatKHR selected = find(
        VK_COLOR_SPACE_SRGB_NONLINEAR_KHR,
        isEightBitTargetFormat);
    if (selected.format != VK_FORMAT_UNDEFINED) {
        return selected;
    }
    return find(
        VK_COLOR_SPACE_BT709_NONLINEAR_EXT,
        isEightBitTargetFormat);
}

bool VulkanNormalizedSourceRect::isValid() const noexcept
{
    return std::isfinite(left) && std::isfinite(top)
        && std::isfinite(right) && std::isfinite(bottom)
        && left >= 0.0F && top >= 0.0F
        && right <= 1.0F && bottom <= 1.0F
        && right > left && bottom > top;
}

VulkanTextureFrame::~VulkanTextureFrame() = default;

VkImageView VulkanTextureFrame::unconvertedImageView() const noexcept
{
    return VK_NULL_HANDLE;
}

VkSampler VulkanTextureFrame::unconvertedSampler() const noexcept
{
    return VK_NULL_HANDLE;
}

VkFormat VulkanTextureFrame::format() const noexcept
{
    return VK_FORMAT_UNDEFINED;
}

VkImageUsageFlags VulkanTextureFrame::usage() const noexcept
{
    return VK_IMAGE_USAGE_SAMPLED_BIT;
}

VkImageLayout VulkanTextureFrame::initialLayout() const noexcept
{
    return VK_IMAGE_LAYOUT_UNDEFINED;
}

VkImageLayout VulkanTextureFrame::sampledLayout() const noexcept
{
    return VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;
}

VkImageLayout VulkanTextureFrame::releaseLayout() const noexcept
{
    return VK_IMAGE_LAYOUT_GENERAL;
}

std::uint32_t VulkanTextureFrame::sourceQueueFamilyIndex() const noexcept
{
#ifdef VK_QUEUE_FAMILY_FOREIGN_EXT
    return VK_QUEUE_FAMILY_FOREIGN_EXT;
#else
    return VK_QUEUE_FAMILY_EXTERNAL;
#endif
}

VulkanNormalizedSourceRect
VulkanTextureFrame::normalizedSourceRect() const noexcept
{
    return {};
}

bool VulkanTextureFrame::waitForProducer(std::string& detail) noexcept
{
    if (!acquireSemaphore()) {
        return true;
    }
    detail =
        "The Vulkan texture has an acquire semaphore but no platform queue bridge";
    return false;
}

VulkanHardwareFrameInterop::~VulkanHardwareFrameInterop() = default;

class VulkanVideoRenderer::Impl {
public:
    struct RetainedHardwareFrame {
        std::uint64_t completionValue = 0;
        VideoFrame videoFrame;
        std::shared_ptr<VulkanTextureFrame> texture;
    };

    Impl(
        BorrowedVulkanDevice device,
        VulkanCurrentTargetCallback currentTarget,
        std::shared_ptr<VulkanHardwareFrameInterop> hardwareInterop)
        : device_(device)
        , currentTarget_(std::move(currentTarget))
        , hardwareInterop_(std::move(hardwareInterop))
#if defined(__ANDROID__)
        , externalNormalizer_(device)
#endif
    {
        connectHardwareInterop();
    }

    ~Impl()
    {
        close();
    }

    void notify(VideoRenderEventType type, std::string detail)
    {
        EventCallback callback;
        {
            std::lock_guard<std::mutex> lock(stateMutex_);
            callback = eventCallback_;
        }
        if (callback) {
            callback({ type, std::move(detail) });
        }
    }

    static void logCallback(
        void* privateData,
        enum pl_log_level level,
        const char* message)
    {
        if (!privateData || !message || level > PL_LOG_ERR) {
            return;
        }
        auto* self = static_cast<Impl*>(privateData);
        std::lock_guard<std::mutex> lock(self->logMutex_);
        self->lastLogError_ = message;
    }

    bool createCommon(std::string& error)
    {
        if (!device_.instance
            || !device_.timelineSemaphoreEnabled
            || !device_.hostQueryResetEnabled) {
            error =
                "libplacebo requires a Vulkan 1.2 instance and a borrowed device created with timelineSemaphore and hostQueryReset";
            return false;
        }
        VkPhysicalDeviceProperties properties {};
        vkGetPhysicalDeviceProperties(device_.physicalDevice, &properties);
        if (properties.apiVersion < VK_API_VERSION_1_2) {
            error = "libplacebo requires Vulkan 1.2 or newer";
            return false;
        }

        pl_log_params logParams {};
        logParams.log_cb = &Impl::logCallback;
        logParams.log_priv = this;
        logParams.log_level = PL_LOG_ERR;
        log_ = pl_log_create(PL_API_VER, &logParams);

        VkPhysicalDeviceVulkan12Features features12 {
            VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_VULKAN_1_2_FEATURES,
        };
        features12.timelineSemaphore = VK_TRUE;
        features12.hostQueryReset = VK_TRUE;
        VkPhysicalDeviceFeatures2 features2 {
            VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_FEATURES_2,
        };
        features2.pNext = &features12;
        pl_vulkan_import_params importParams {};
        importParams.instance = device_.instance;
        importParams.get_proc_addr = vkGetInstanceProcAddr;
        importParams.phys_device = device_.physicalDevice;
        importParams.device = device_.device;
        importParams.queue_graphics = {
            device_.queueFamilyIndex,
            1,
        };
        importParams.features = &features2;
        importParams.no_compute = true;
        vulkan_ = pl_vulkan_import(log_, &importParams);
        if (!vulkan_) {
            error = takeLogError(
                "libplacebo could not import the borrowed Vulkan device");
            destroyCommon();
            return false;
        }
        renderer_ = pl_renderer_create(log_, vulkan_->gpu);
        if (!renderer_) {
            error = "libplacebo could not create its renderer";
            destroyCommon();
            return false;
        }
        getSemaphoreCounterValue_ =
            reinterpret_cast<PFN_vkGetSemaphoreCounterValue>(
                vkGetDeviceProcAddr(
                    device_.device,
                    "vkGetSemaphoreCounterValue"));
        if (!getSemaphoreCounterValue_) {
            getSemaphoreCounterValue_ =
                reinterpret_cast<PFN_vkGetSemaphoreCounterValue>(
                    vkGetDeviceProcAddr(
                        device_.device,
                        "vkGetSemaphoreCounterValueKHR"));
        }
        if (!getSemaphoreCounterValue_) {
            error =
                "The Vulkan device does not expose timeline semaphore counters";
            destroyCommon();
            return false;
        }

        VkSemaphoreTypeCreateInfo typeInfo {
            VK_STRUCTURE_TYPE_SEMAPHORE_TYPE_CREATE_INFO,
        };
        typeInfo.semaphoreType = VK_SEMAPHORE_TYPE_TIMELINE;
        VkSemaphoreCreateInfo semaphoreInfo {
            VK_STRUCTURE_TYPE_SEMAPHORE_CREATE_INFO,
        };
        semaphoreInfo.pNext = &typeInfo;
        const VkResult semaphoreResult = vkCreateSemaphore(
            device_.device,
            &semaphoreInfo,
            nullptr,
            &completionSemaphore_);
        if (semaphoreResult != VK_SUCCESS) {
            error = vkResult(
                "vkCreateSemaphore(libplacebo completion)",
                semaphoreResult);
            destroyCommon();
            return false;
        }
        return true;
    }

    void destroyCommon() noexcept
    {
        if (vulkan_) {
            pl_gpu_finish(vulkan_->gpu);
        }
#if defined(__ANDROID__)
        externalNormalizer_.finish();
#endif
        retainedHardwareFrames_.clear();
        for (pl_tex& texture : uploadTextures_) {
            if (texture && vulkan_) {
                pl_tex_destroy(vulkan_->gpu, &texture);
            }
        }
        if (renderer_) {
            pl_renderer_destroy(&renderer_);
        }
        if (completionSemaphore_ && device_.device) {
            vkDestroySemaphore(
                device_.device,
                completionSemaphore_,
                nullptr);
            completionSemaphore_ = VK_NULL_HANDLE;
        }
        if (vulkan_) {
            pl_vulkan_destroy(&vulkan_);
        }
        if (log_) {
            pl_log_destroy(&log_);
        }
        completionValue_ = 0;
        getSemaphoreCounterValue_ = nullptr;
    }

    void close() noexcept
    {
        std::lock_guard<std::mutex> renderLock(renderMutex_);
        {
            std::lock_guard<std::mutex> stateLock(stateMutex_);
            open_ = false;
            config_ = {};
        }
        destroyCommon();
    }

    std::string takeLogError(std::string fallback)
    {
        std::lock_guard<std::mutex> lock(logMutex_);
        if (lastLogError_.empty()) {
            return fallback;
        }
        fallback += ": " + lastLogError_;
        lastLogError_.clear();
        return fallback;
    }

    void retireCompletedHardwareFrames()
    {
        if (!completionSemaphore_ || retainedHardwareFrames_.empty()) {
            return;
        }
        std::uint64_t completed = 0;
        if (!getSemaphoreCounterValue_
            || getSemaphoreCounterValue_(
                device_.device,
                completionSemaphore_,
                &completed)
            != VK_SUCCESS) {
            return;
        }
        while (!retainedHardwareFrames_.empty()
               && retainedHardwareFrames_.front().completionValue
                   <= completed) {
            retainedHardwareFrames_.pop_front();
        }
    }

    std::uint64_t nextCompletionValue() noexcept
    {
        return ++completionValue_;
    }

    bool wrapTarget(
        const VulkanRenderTarget& native,
        pl_tex& texture,
        struct pl_frame& frame,
        std::string& error)
    {
        pl_vulkan_wrap_params wrapParams {};
        wrapParams.image = native.image;
        wrapParams.width = static_cast<int>(native.extent.width);
        wrapParams.height = static_cast<int>(native.extent.height);
        wrapParams.format = native.format;
        wrapParams.usage = VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT
            | VK_IMAGE_USAGE_TRANSFER_DST_BIT;
        wrapParams.debug_tag = "QtAVCore target";
        texture = pl_vulkan_wrap(vulkan_->gpu, &wrapParams);
        if (!texture || !texture->params.renderable) {
            error = takeLogError(
                "libplacebo cannot wrap the Vulkan render target");
            return false;
        }
        pl_vulkan_release_params release {};
        release.tex = texture;
        release.layout = VK_IMAGE_LAYOUT_UNDEFINED;
        release.qf = device_.queueFamilyIndex;
        release.semaphore.sem = native.waitSemaphore;
        pl_vulkan_release_ex(vulkan_->gpu, &release);

        frame.num_planes = 1;
        initializePlane(frame.planes[0], texture, 4, 0);
        frame.crop = {
            0.0F,
            0.0F,
            static_cast<float>(native.extent.width),
            static_cast<float>(native.extent.height),
        };
        setTargetColor(native, frame);
        return true;
    }

    bool wrapHardwareSource(
        const VideoFrame& source,
        const std::shared_ptr<VulkanTextureFrame>& imported,
        pl_tex& texture,
        struct pl_frame& frame,
        struct pl_dovi_metadata& dovi,
        bool& externalNormalized,
        std::string& error)
    {
        VkImage sourceImage = imported->image();
        VkFormat sourceFormat = imported->format();
        VkImageUsageFlags sourceUsage = imported->usage();
        VkImageLayout sourceLayout = imported->initialLayout();
        std::uint32_t sourceQueueFamily =
            imported->sourceQueueFamilyIndex();
        int sourceWidth = imported->width();
        int sourceHeight = imported->height();
        const AVFrame* native =
            detail::FrameFactory::nativeVideoFrame(source);
        const int doviBitDepth = native
            ? qtav_pl_dovi_bit_depth(native)
            : 0;
        const bool hasDovi = doviBitDepth != 0;
        externalNormalized = false;
        if (sourceFormat == VK_FORMAT_UNDEFINED) {
#if defined(__ANDROID__)
            if (!externalNormalizer_.convert(
                    imported,
                    source.width(),
                    source.height(),
                    hasDovi,
                    error)) {
                return false;
            }
            sourceImage = externalNormalizer_.image();
            sourceFormat = externalNormalizer_.format();
            sourceUsage = VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT
                | VK_IMAGE_USAGE_SAMPLED_BIT;
            sourceLayout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;
            sourceQueueFamily = device_.queueFamilyIndex;
            sourceWidth = source.width();
            sourceHeight = source.height();
            externalNormalized = true;
#else
            error =
                "libplacebo cannot wrap this opaque Vulkan external format";
            return false;
#endif
        } else if (!imported->waitForProducer(error)) {
            return false;
        }
        pl_vulkan_wrap_params wrapParams {};
        wrapParams.image = sourceImage;
        wrapParams.width = sourceWidth;
        wrapParams.height = sourceHeight;
        wrapParams.format = sourceFormat;
        wrapParams.usage = sourceUsage;
        wrapParams.debug_tag = "QtAVCore MediaCodec source";
        texture = pl_vulkan_wrap(vulkan_->gpu, &wrapParams);
        if (!texture) {
            error = takeLogError(
                "libplacebo cannot wrap the imported MediaCodec image");
            return false;
        }
        pl_vulkan_release_params release {};
        release.tex = texture;
        release.layout = sourceLayout;
        release.qf = sourceQueueFamily;
        pl_vulkan_release_ex(vulkan_->gpu, &release);

        if (texture->params.format->num_planes == 0
            && texture->params.format->num_components >= 3) {
            frame.num_planes = 1;
            initializePlane(
                frame.planes[0],
                texture,
                texture->params.format->num_components,
                0);
        } else if (texture->params.format->num_planes == 2
            && texture->planes[0] && texture->planes[1]) {
            frame.num_planes = 2;
            initializePlane(frame.planes[0], texture->planes[0], 1, 0);
            initializePlane(frame.planes[1], texture->planes[1], 2, 1);
        } else if (texture->params.format->num_planes == 3
                   && texture->planes[0]
                   && texture->planes[1]
                   && texture->planes[2]) {
            frame.num_planes = 3;
            initializePlane(frame.planes[0], texture->planes[0], 1, 0);
            initializePlane(frame.planes[1], texture->planes[1], 1, 1);
            initializePlane(frame.planes[2], texture->planes[2], 1, 2);
        } else {
            error =
                "The imported MediaCodec Vulkan format has no libplacebo YUV plane mapping";
            return false;
        }
        const bool tenBit = sourceFormat
                == VK_FORMAT_G10X6_B10X6R10X6_2PLANE_420_UNORM_3PACK16
            || sourceFormat
                == VK_FORMAT_G10X6_B10X6_R10X6_3PLANE_420_UNORM_3PACK16;
        setSourceColor(source, frame);
        if (hasDovi
            && !qtav_pl_map_dovi(&frame, &dovi, native)) {
            error = "FFmpeg Dolby Vision metadata changed during frame mapping";
            return false;
        }
        frame.repr.bits = externalNormalized && hasDovi
            ? pl_bit_encoding {
                doviBitDepth,
                doviBitDepth,
                0,
            }
            : externalNormalized
            ? pl_bit_encoding { 16, 16, 0 }
            : tenBit
            ? pl_bit_encoding { 16, 10, 6 }
            : pl_bit_encoding { 8, 8, 0 };
        if (!hasDovi
            && (externalNormalized
                || texture->params.format->num_planes == 0)) {
            frame.repr.sys = PL_COLOR_SYSTEM_RGB;
            frame.repr.levels = PL_COLOR_LEVELS_FULL;
            frame.repr.alpha = PL_ALPHA_INDEPENDENT;
        }
        if (externalNormalized) {
            frame.crop = {
                0.0F,
                0.0F,
                static_cast<float>(source.width()),
                static_cast<float>(source.height()),
            };
        } else {
            const VulkanNormalizedSourceRect crop =
                imported->normalizedSourceRect();
            if (!crop.isValid()) {
                error = "The imported MediaCodec image has an invalid crop";
                return false;
            }
            frame.crop = {
                crop.left * imported->width(),
                crop.top * imported->height(),
                crop.right * imported->width(),
                crop.bottom * imported->height(),
            };
        }
        return true;
    }

    bool finishTarget(
        const VulkanRenderTarget& native,
        pl_tex texture,
        std::string& error)
    {
        pl_vulkan_hold_params hold {};
        hold.tex = texture;
        hold.layout = native.finalLayout;
        hold.qf = device_.queueFamilyIndex;
        if (native.signalSemaphore) {
            hold.semaphore.sem = native.signalSemaphore;
        } else {
            hold.semaphore.sem = completionSemaphore_;
            hold.semaphore.value = nextCompletionValue();
        }
        if (!pl_vulkan_hold_ex(vulkan_->gpu, &hold)) {
            error = takeLogError(
                "libplacebo could not release the Vulkan render target");
            return false;
        }
        return true;
    }

    std::uint64_t finishHardwareSource(
        pl_tex texture,
        const std::shared_ptr<VulkanTextureFrame>& imported,
        bool externalNormalized,
        std::string& error)
    {
        const std::uint64_t holdValue = nextCompletionValue();
        pl_vulkan_hold_params hold {};
        hold.tex = texture;
        hold.layout = externalNormalized
            ? VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL
            : imported->releaseLayout();
        hold.qf = externalNormalized
            ? device_.queueFamilyIndex
            : imported->sourceQueueFamilyIndex();
        hold.semaphore.sem = completionSemaphore_;
        hold.semaphore.value = holdValue;
        if (!pl_vulkan_hold_ex(vulkan_->gpu, &hold)) {
            error = takeLogError(
                "libplacebo could not release the MediaCodec image");
            return 0;
        }
        if (externalNormalized) {
            return holdValue;
        }
        imported->releaseToProducer();

        // releaseToProducer queues the exported Android sync-fd signal on
        // this queue. Signal our timeline afterwards so the imported image
        // and its binary semaphores remain alive until both submissions have
        // completed.
        const std::uint64_t value = nextCompletionValue();
        VkTimelineSemaphoreSubmitInfo timelineInfo {
            VK_STRUCTURE_TYPE_TIMELINE_SEMAPHORE_SUBMIT_INFO,
        };
        timelineInfo.signalSemaphoreValueCount = 1;
        timelineInfo.pSignalSemaphoreValues = &value;
        VkSubmitInfo submit {
            VK_STRUCTURE_TYPE_SUBMIT_INFO,
        };
        submit.pNext = &timelineInfo;
        submit.signalSemaphoreCount = 1;
        submit.pSignalSemaphores = &completionSemaphore_;
        const VkResult result = vkQueueSubmit(
            device_.queue,
            1,
            &submit,
            VK_NULL_HANDLE);
        if (result != VK_SUCCESS) {
            error = vkResult(
                "vkQueueSubmit(MediaCodec lifetime)",
                result);
            return 0;
        }
        return value;
    }

    void connectHardwareInterop()
    {
        if (!hardwareInterop_) {
            return;
        }
        hardwareInterop_->setFrameAvailableCallback([this] {
            notify(VideoRenderEventType::RedrawRequested, {});
        });
    }

    BorrowedVulkanDevice device_;
    mutable std::mutex stateMutex_;
    std::mutex renderMutex_;
    std::mutex logMutex_;
    VideoRenderConfig config_;
    VulkanCurrentTargetCallback currentTarget_;
    std::shared_ptr<VulkanHardwareFrameInterop> hardwareInterop_;
#if defined(__ANDROID__)
    ExternalImageNormalizer externalNormalizer_;
#endif
    EventCallback eventCallback_;
    bool open_ = false;
    std::string lastLogError_;
    pl_log log_ = nullptr;
    pl_vulkan vulkan_ = nullptr;
    pl_renderer renderer_ = nullptr;
    std::array<pl_tex, 4> uploadTextures_ {};
    VkSemaphore completionSemaphore_ = VK_NULL_HANDLE;
    std::uint64_t completionValue_ = 0;
    PFN_vkGetSemaphoreCounterValue getSemaphoreCounterValue_ = nullptr;
    std::deque<RetainedHardwareFrame> retainedHardwareFrames_;
};

VulkanVideoRenderer::VulkanVideoRenderer(
    BorrowedVulkanDevice device,
    VulkanCurrentTargetCallback currentTarget,
    std::shared_ptr<VulkanHardwareFrameInterop> hardwareInterop)
    : impl_(std::make_unique<Impl>(
          device,
          std::move(currentTarget),
          std::move(hardwareInterop)))
{
}

VulkanVideoRenderer::~VulkanVideoRenderer() = default;
VulkanVideoRenderer::VulkanVideoRenderer(
    VulkanVideoRenderer&&) noexcept = default;
VulkanVideoRenderer& VulkanVideoRenderer::operator=(
    VulkanVideoRenderer&&) noexcept = default;

VideoRenderCapabilities VulkanVideoRenderer::capabilities() const
{
    VideoRenderCapabilities result;
    result.softwareFormats = {
        PixelFormat::YUV420P,
        PixelFormat::YUV422P,
        PixelFormat::YUV444P,
        PixelFormat::YUV420P10,
        PixelFormat::NV12,
        PixelFormat::NV21,
        PixelFormat::P010,
        PixelFormat::RGB24,
        PixelFormat::BGR24,
        PixelFormat::RGBA,
        PixelFormat::BGRA,
        PixelFormat::ARGB,
        PixelFormat::Gray8,
    };
    result.customViewport = true;
    result.rotation = true;
    if (impl_) {
        std::lock_guard<std::mutex> lock(impl_->stateMutex_);
        if (impl_->hardwareInterop_
            && sameDeviceContext(
                impl_->hardwareInterop_->device(),
                impl_->device_)) {
            result.hardwareDevices =
                impl_->hardwareInterop_->capabilities().sourceDevices;
        }
    }
    return result;
}

void VulkanVideoRenderer::setEventCallback(EventCallback callback)
{
    if (!impl_) {
        return;
    }
    std::lock_guard<std::mutex> lock(impl_->stateMutex_);
    impl_->eventCallback_ = std::move(callback);
}

bool VulkanVideoRenderer::open(const VideoRenderConfig& config)
{
    if (!impl_) {
        return false;
    }
    std::string error;
    bool opened = false;
    {
        std::lock_guard<std::mutex> renderLock(impl_->renderMutex_);
        std::lock_guard<std::mutex> stateLock(impl_->stateMutex_);
        if (!impl_->device_.isValid()) {
            error =
                "The Vulkan renderer requires a borrowed physical device, logical device, and queue";
        } else if (!impl_->currentTarget_) {
            error = "The Vulkan renderer requires a current-target callback";
        } else if (!supportedConfig(config)) {
            error =
                "The Vulkan renderer requires a valid borrowed surface configuration";
        } else if (impl_->open_) {
            impl_->config_ = config;
            opened = true;
        } else if (impl_->createCommon(error)) {
            impl_->config_ = config;
            impl_->open_ = true;
            opened = true;
        }
    }
    if (!opened) {
        impl_->notify(VideoRenderEventType::Error, std::move(error));
    }
    return opened;
}

bool VulkanVideoRenderer::configure(const VideoRenderConfig& config)
{
    if (!impl_) {
        return false;
    }
    bool configured = false;
    {
        std::lock_guard<std::mutex> lock(impl_->stateMutex_);
        configured = impl_->open_ && supportedConfig(config);
        if (configured) {
            impl_->config_ = config;
        }
    }
    if (configured) {
        impl_->notify(VideoRenderEventType::RedrawRequested, {});
    } else {
        impl_->notify(
            VideoRenderEventType::Error,
            "The Vulkan renderer is closed or the configuration is invalid");
    }
    return configured;
}

bool VulkanVideoRenderer::render(const VideoFrame& frame)
{
    if (!impl_ || !frame) {
        return false;
    }
    std::lock_guard<std::mutex> renderLock(impl_->renderMutex_);
    VideoRenderConfig config;
    VulkanCurrentTargetCallback currentTarget;
    std::shared_ptr<VulkanHardwareFrameInterop> hardwareInterop;
    {
        std::lock_guard<std::mutex> stateLock(impl_->stateMutex_);
        if (impl_->open_) {
            config = impl_->config_;
            currentTarget = impl_->currentTarget_;
            hardwareInterop = impl_->hardwareInterop_;
        }
    }
    if (!config.surfaceSize.isValid()) {
        impl_->notify(VideoRenderEventType::Error, "The Vulkan renderer is not open");
        return false;
    }
    impl_->retireCompletedHardwareFrames();

    std::string error;
    if (frame.hasHardwareFrame()) {
        if (!hardwareInterop
            || !sameDeviceContext(hardwareInterop->device(), impl_->device_)
            || !hardwareInterop->supports(frame.hardwareFrame())) {
            impl_->notify(
                VideoRenderEventType::Error,
                "The Vulkan renderer has no compatible interop for this hardware frame");
            return false;
        }
        const VulkanHardwareImportStatus status =
            hardwareInterop->prepareFrame(frame, error);
        if (status == VulkanHardwareImportStatus::Pending) {
            return false;
        }
        if (status != VulkanHardwareImportStatus::Ready) {
            impl_->notify(
                VideoRenderEventType::Error,
                error.empty()
                    ? "The Vulkan hardware frame preparation failed"
                    : std::move(error));
            return false;
        }
    }

    const VulkanRenderTarget nativeTarget =
        currentTarget ? currentTarget() : VulkanRenderTarget {};
    if (!nativeTarget.isValid()) {
        impl_->notify(
            VideoRenderEventType::SurfaceLost,
            "The current Vulkan target is unavailable");
        return false;
    }
    if (nativeTarget.extent.width
            != static_cast<std::uint32_t>(config.surfaceSize.width)
        || nativeTarget.extent.height
            != static_cast<std::uint32_t>(config.surfaceSize.height)) {
        impl_->notify(
            VideoRenderEventType::Error,
            "The current Vulkan target extent does not match the configured surface");
        return false;
    }

    pl_tex targetTexture = nullptr;
    pl_frame target {};
    if (!impl_->wrapTarget(nativeTarget, targetTexture, target, error)) {
        if (targetTexture) {
            pl_tex_destroy(impl_->vulkan_->gpu, &targetTexture);
        }
        impl_->notify(VideoRenderEventType::Error, std::move(error));
        return false;
    }

    pl_frame image {};
    pl_tex hardwareTexture = nullptr;
    std::shared_ptr<VulkanTextureFrame> imported;
    pl_dovi_metadata dovi {};
    bool externalNormalized = false;
    bool mappedSoftware = false;
    if (frame.hasHardwareFrame()) {
        VulkanHardwareImportResult importedResult =
            hardwareInterop->importFrame(frame);
        if (!importedResult) {
            error = importedResult.detail.empty()
                ? "The prepared Vulkan hardware frame import failed"
                : std::move(importedResult.detail);
        } else {
            imported = std::move(importedResult.texture);
            if (imported->width() != frame.width()
                || imported->height() != frame.height()) {
                error =
                    "The imported Vulkan image dimensions do not match the decoded frame";
            } else {
                impl_->wrapHardwareSource(
                    frame,
                    imported,
                    hardwareTexture,
                    image,
                    dovi,
                    externalNormalized,
                    error);
            }
        }
    } else {
        const AVFrame* native =
            detail::FrameFactory::nativeVideoFrame(frame);
        if (!native
            || !qtav_pl_map_avframe(
                impl_->vulkan_->gpu,
                &image,
                impl_->uploadTextures_.data(),
                native)) {
            error = impl_->takeLogError(
                "libplacebo could not map the decoded software frame");
        } else {
            mappedSoftware = true;
        }
    }

    bool rendered = error.empty();
    if (rendered) {
        applyGeometry(image, target, config);
        rendered = pl_render_image(
            impl_->renderer_,
            &image,
            &target,
            &pl_render_default_params);
        if (!rendered) {
            error = impl_->takeLogError(
                "libplacebo could not render the video frame");
        }
    }
    if (mappedSoftware) {
        qtav_pl_unmap_avframe(impl_->vulkan_->gpu, &image);
    }

    // Always return the borrowed target to the application, even when source
    // import or rendering failed after the target was released to libplacebo.
    // Swapchain adapters rely on the final layout and signal semaphore to
    // retire the acquired image without stalling the next frame.
    const bool targetFinished = impl_->finishTarget(
        nativeTarget,
        targetTexture,
        error);
    std::uint64_t hardwareCompletion = 0;
    if (hardwareTexture && imported) {
        hardwareCompletion = impl_->finishHardwareSource(
            hardwareTexture,
            imported,
            externalNormalized,
            error);
        if (!hardwareCompletion) {
            rendered = false;
        }
    }
    if (hardwareTexture) {
        pl_tex_destroy(impl_->vulkan_->gpu, &hardwareTexture);
    }
    if (targetTexture) {
        pl_tex_destroy(impl_->vulkan_->gpu, &targetTexture);
    }
    if (hardwareCompletion && imported) {
        impl_->retainedHardwareFrames_.push_back({
            hardwareCompletion,
            frame,
            std::move(imported),
        });
    }
    if (nativeTarget.waitUntilCompleted && targetFinished) {
        pl_gpu_finish(impl_->vulkan_->gpu);
        impl_->retireCompletedHardwareFrames();
    }
    if (!rendered || !targetFinished) {
        impl_->notify(
            VideoRenderEventType::Error,
            error.empty()
                ? "libplacebo rendering failed"
                : std::move(error));
        return false;
    }
    return true;
}

void VulkanVideoRenderer::close() noexcept
{
    if (impl_) {
        impl_->close();
    }
}

BorrowedVulkanDevice VulkanVideoRenderer::device() const noexcept
{
    return impl_ ? impl_->device_ : BorrowedVulkanDevice {};
}

void VulkanVideoRenderer::setCurrentTargetCallback(
    VulkanCurrentTargetCallback callback)
{
    if (!impl_) {
        return;
    }
    std::lock_guard<std::mutex> lock(impl_->stateMutex_);
    impl_->currentTarget_ = std::move(callback);
}

VulkanHardwareImportStatus
VulkanVideoRenderer::prepareHardwareFrame(
    const VideoFrame& frame,
    std::string* detail)
{
    if (!impl_ || !frame || !frame.hasHardwareFrame()) {
        if (detail) {
            *detail = "The frame is not a Vulkan-interoperable hardware frame";
        }
        return VulkanHardwareImportStatus::Unsupported;
    }
    std::lock_guard<std::mutex> renderLock(impl_->renderMutex_);
    std::shared_ptr<VulkanHardwareFrameInterop> interop;
    {
        std::lock_guard<std::mutex> stateLock(impl_->stateMutex_);
        interop = impl_->hardwareInterop_;
    }
    if (!interop
        || !sameDeviceContext(interop->device(), impl_->device_)
        || !interop->supports(frame.hardwareFrame())) {
        if (detail) {
            *detail =
                "The Vulkan renderer has no compatible interop for this hardware frame";
        }
        return VulkanHardwareImportStatus::Unsupported;
    }
    std::string localDetail;
    const VulkanHardwareImportStatus status =
        interop->prepareFrame(frame, localDetail);
    if (detail) {
        *detail = std::move(localDetail);
    }
    return status;
}

void VulkanVideoRenderer::setHardwareFrameInterop(
    std::shared_ptr<VulkanHardwareFrameInterop> hardwareInterop)
{
    if (!impl_) {
        return;
    }
    std::lock_guard<std::mutex> renderLock(impl_->renderMutex_);
    std::shared_ptr<VulkanHardwareFrameInterop> previous;
    {
        std::lock_guard<std::mutex> stateLock(impl_->stateMutex_);
        previous = std::move(impl_->hardwareInterop_);
        impl_->hardwareInterop_ = std::move(hardwareInterop);
    }
    if (previous) {
        previous->setFrameAvailableCallback({});
    }
    impl_->connectHardwareInterop();
}

std::shared_ptr<VulkanHardwareFrameInterop>
VulkanVideoRenderer::hardwareFrameInterop() const noexcept
{
    if (!impl_) {
        return {};
    }
    std::lock_guard<std::mutex> lock(impl_->stateMutex_);
    return impl_->hardwareInterop_;
}

} // namespace qtav
