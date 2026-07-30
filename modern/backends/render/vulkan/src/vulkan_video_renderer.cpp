// SPDX-License-Identifier: LGPL-2.1-or-later

#include <qtav/vulkan_video_renderer.h>

#include <algorithm>
#include <array>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <limits>
#include <mutex>
#include <string>
#include <utility>
#include <vector>

#include "video_frag_spv.inc"
#include "video_vert_spv.inc"

namespace qtav {
namespace {

enum class ShaderPixelFormat : std::uint32_t {
    YUV420P,
    YUV422P,
    YUV444P,
    NV12,
    NV21,
    P010,
    RGB24,
    BGR24,
    RGBA,
    BGRA,
    ARGB,
    Gray8,
};

enum class ShaderColorMatrix : std::uint32_t {
    BT709,
    BT601,
    BT2020,
};

enum class ShaderColorTransfer : std::uint32_t {
    SDR,
    PQ,
    HLG,
    Linear,
};

enum class ShaderColorPrimaries : std::uint32_t {
    BT709,
    BT2020,
    DisplayP3,
};

struct alignas(16) ShaderParameters {
    std::array<std::uint32_t, 4> source {};
    std::array<std::uint32_t, 4> strides {};
    std::array<std::uint32_t, 4> offsets {};
    std::array<std::uint32_t, 4> surface {};
    std::array<std::uint32_t, 4> viewport {};
    std::array<std::uint32_t, 4> presentation {};
    std::array<float, 4> luminance {};
};

static_assert(sizeof(ShaderParameters) == 112);

struct PackedFrame {
    std::vector<std::uint8_t> bytes;
    ShaderParameters parameters;
};

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

bool supportedTargetFormat(VkFormat format) noexcept
{
    return format == VK_FORMAT_B8G8R8A8_UNORM
        || format == VK_FORMAT_B8G8R8A8_SRGB
        || format == VK_FORMAT_R8G8B8A8_UNORM
        || format == VK_FORMAT_R8G8B8A8_SRGB;
}

bool checkedUint(std::size_t value, std::uint32_t& result) noexcept
{
    if (value > std::numeric_limits<std::uint32_t>::max()) {
        return false;
    }
    result = static_cast<std::uint32_t>(value);
    return true;
}

bool pixelFormat(
    PixelFormat source,
    ShaderPixelFormat& destination,
    int& planeCount,
    std::array<int, 3>& horizontalDivisors,
    std::array<int, 3>& verticalDivisors,
    std::array<int, 3>& bytesPerElement) noexcept
{
    horizontalDivisors = { 1, 1, 1 };
    verticalDivisors = { 1, 1, 1 };
    bytesPerElement = { 1, 1, 1 };
    switch (source) {
    case PixelFormat::YUV420P:
        destination = ShaderPixelFormat::YUV420P;
        planeCount = 3;
        horizontalDivisors = { 1, 2, 2 };
        verticalDivisors = { 1, 2, 2 };
        return true;
    case PixelFormat::YUV422P:
        destination = ShaderPixelFormat::YUV422P;
        planeCount = 3;
        horizontalDivisors = { 1, 2, 2 };
        return true;
    case PixelFormat::YUV444P:
        destination = ShaderPixelFormat::YUV444P;
        planeCount = 3;
        return true;
    case PixelFormat::NV12:
        destination = ShaderPixelFormat::NV12;
        planeCount = 2;
        horizontalDivisors = { 1, 2, 1 };
        verticalDivisors = { 1, 2, 1 };
        bytesPerElement = { 1, 2, 1 };
        return true;
    case PixelFormat::NV21:
        destination = ShaderPixelFormat::NV21;
        planeCount = 2;
        horizontalDivisors = { 1, 2, 1 };
        verticalDivisors = { 1, 2, 1 };
        bytesPerElement = { 1, 2, 1 };
        return true;
    case PixelFormat::P010:
        destination = ShaderPixelFormat::P010;
        planeCount = 2;
        horizontalDivisors = { 1, 2, 1 };
        verticalDivisors = { 1, 2, 1 };
        bytesPerElement = { 2, 4, 1 };
        return true;
    case PixelFormat::RGB24:
        destination = ShaderPixelFormat::RGB24;
        planeCount = 1;
        bytesPerElement[0] = 3;
        return true;
    case PixelFormat::BGR24:
        destination = ShaderPixelFormat::BGR24;
        planeCount = 1;
        bytesPerElement[0] = 3;
        return true;
    case PixelFormat::RGBA:
        destination = ShaderPixelFormat::RGBA;
        planeCount = 1;
        bytesPerElement[0] = 4;
        return true;
    case PixelFormat::BGRA:
        destination = ShaderPixelFormat::BGRA;
        planeCount = 1;
        bytesPerElement[0] = 4;
        return true;
    case PixelFormat::ARGB:
        destination = ShaderPixelFormat::ARGB;
        planeCount = 1;
        bytesPerElement[0] = 4;
        return true;
    case PixelFormat::Gray8:
        destination = ShaderPixelFormat::Gray8;
        planeCount = 1;
        return true;
    default:
        return false;
    }
}

bool appendPlane(
    const std::uint8_t* data,
    int lineSize,
    int rowBytes,
    int height,
    std::vector<std::uint8_t>& destination,
    std::uint32_t& offset) noexcept
{
    if (!data || lineSize == 0 || rowBytes <= 0 || height <= 0
        || std::abs(static_cast<long long>(lineSize)) < rowBytes
        || !checkedUint(destination.size(), offset)) {
        return false;
    }
    const std::size_t originalSize = destination.size();
    const std::size_t planeSize =
        static_cast<std::size_t>(rowBytes) * static_cast<std::size_t>(height);
    if (planeSize > std::numeric_limits<std::uint32_t>::max()
        || originalSize
            > std::numeric_limits<std::uint32_t>::max() - planeSize) {
        return false;
    }
    destination.resize(originalSize + planeSize);
    for (int row = 0; row < height; ++row) {
        std::memcpy(
            destination.data() + originalSize
                + static_cast<std::size_t>(row) * rowBytes,
            data + static_cast<std::ptrdiff_t>(row) * lineSize,
            static_cast<std::size_t>(rowBytes));
    }
    return true;
}

ShaderColorMatrix shaderColorMatrix(ColorMatrix matrix) noexcept
{
    switch (matrix) {
    case ColorMatrix::BT709:
        return ShaderColorMatrix::BT709;
    case ColorMatrix::BT2020NCL:
    case ColorMatrix::BT2020CL:
        return ShaderColorMatrix::BT2020;
    default:
        return ShaderColorMatrix::BT601;
    }
}

ShaderColorTransfer shaderColorTransfer(ColorTransfer transfer) noexcept
{
    switch (transfer) {
    case ColorTransfer::PQ:
        return ShaderColorTransfer::PQ;
    case ColorTransfer::HLG:
        return ShaderColorTransfer::HLG;
    case ColorTransfer::Linear:
        return ShaderColorTransfer::Linear;
    default:
        return ShaderColorTransfer::SDR;
    }
}

ShaderColorPrimaries shaderColorPrimaries(ColorPrimaries primaries) noexcept
{
    switch (primaries) {
    case ColorPrimaries::BT2020:
        return ShaderColorPrimaries::BT2020;
    case ColorPrimaries::SMPTE432:
        return ShaderColorPrimaries::DisplayP3;
    default:
        return ShaderColorPrimaries::BT709;
    }
}

float maximumLuminance(const VideoFrame& frame) noexcept
{
    const MasteringDisplayMetadata mastering =
        frame.masteringDisplayMetadata();
    if (mastering.hasLuminance && mastering.maximumLuminance > 0.0) {
        return static_cast<float>(mastering.maximumLuminance);
    }
    const ContentLightMetadata content = frame.contentLightMetadata();
    if (content.maximumContentLightLevel > 0) {
        return static_cast<float>(content.maximumContentLightLevel);
    }
    return frame.colorSpaceInfo().isHdr() ? 1000.0F : 100.0F;
}

bool packFrame(
    const VideoFrame& frame,
    const VideoRenderConfig& config,
    PackedFrame& result,
    std::string& error)
{
    ShaderPixelFormat format {};
    int planeCount = 0;
    std::array<int, 3> horizontalDivisors {};
    std::array<int, 3> verticalDivisors {};
    std::array<int, 3> bytesPerElement {};
    if (!frame
        || !pixelFormat(
            frame.format(),
            format,
            planeCount,
            horizontalDivisors,
            verticalDivisors,
            bytesPerElement)) {
        error = "The Vulkan renderer does not support this software pixel format";
        return false;
    }
    if (frame.format() == PixelFormat::P010
        && frame.formatName().find("p010le") == std::string::npos) {
        error = "The Vulkan renderer currently supports little-endian P010";
        return false;
    }

    result.parameters.source = {
        static_cast<std::uint32_t>(frame.width()),
        static_cast<std::uint32_t>(frame.height()),
        static_cast<std::uint32_t>(format),
        0U,
    };
    const VideoViewport viewport = effectiveViewport(config);
    const VideoColorSpace color = frame.colorSpaceInfo();
    result.parameters.surface = {
        static_cast<std::uint32_t>(config.surfaceSize.width),
        static_cast<std::uint32_t>(config.surfaceSize.height),
        static_cast<std::uint32_t>(shaderColorTransfer(color.transfer)),
        static_cast<std::uint32_t>(shaderColorPrimaries(color.primaries)),
    };
    result.parameters.viewport = {
        static_cast<std::uint32_t>(viewport.x),
        static_cast<std::uint32_t>(viewport.y),
        static_cast<std::uint32_t>(viewport.width),
        static_cast<std::uint32_t>(viewport.height),
    };
    result.parameters.presentation = {
        static_cast<std::uint32_t>(config.rotation),
        static_cast<std::uint32_t>(config.aspectRatio),
        static_cast<std::uint32_t>(shaderColorMatrix(color.matrix)),
        color.range == ColorRange::Full ? 1U : 0U,
    };
    result.parameters.luminance = {
        100.0F,
        maximumLuminance(frame),
        0.0F,
        0.0F,
    };

    for (int plane = 0; plane < planeCount; ++plane) {
        const int width =
            (frame.width() + horizontalDivisors[plane] - 1)
            / horizontalDivisors[plane];
        const int height =
            (frame.height() + verticalDivisors[plane] - 1)
            / verticalDivisors[plane];
        const int rowBytes = width * bytesPerElement[plane];
        result.parameters.strides[plane] =
            static_cast<std::uint32_t>(rowBytes);
        if (!appendPlane(
                frame.data(plane),
                frame.lineSize(plane),
                rowBytes,
                height,
                result.bytes,
                result.parameters.offsets[plane])) {
            error = "The Vulkan renderer could not copy a software frame plane";
            return false;
        }
    }
    while ((result.bytes.size() & 3U) != 0U) {
        result.bytes.push_back(0);
    }
    return true;
}

const char* resultName(VkResult result) noexcept
{
    switch (result) {
    case VK_SUCCESS:
        return "VK_SUCCESS";
    case VK_ERROR_OUT_OF_HOST_MEMORY:
        return "VK_ERROR_OUT_OF_HOST_MEMORY";
    case VK_ERROR_OUT_OF_DEVICE_MEMORY:
        return "VK_ERROR_OUT_OF_DEVICE_MEMORY";
    case VK_ERROR_DEVICE_LOST:
        return "VK_ERROR_DEVICE_LOST";
    case VK_ERROR_INITIALIZATION_FAILED:
        return "VK_ERROR_INITIALIZATION_FAILED";
    case VK_ERROR_FORMAT_NOT_SUPPORTED:
        return "VK_ERROR_FORMAT_NOT_SUPPORTED";
    default:
        return "unknown VkResult";
    }
}

std::string resultError(const char* operation, VkResult result)
{
    return std::string(operation) + " failed: " + resultName(result)
        + " (" + std::to_string(static_cast<int>(result)) + ')';
}

} // namespace

bool BorrowedVulkanDevice::isValid() const noexcept
{
    return physicalDevice != VK_NULL_HANDLE
        && device != VK_NULL_HANDLE
        && queue != VK_NULL_HANDLE;
}

bool VulkanRenderTarget::isValid() const noexcept
{
    return image != VK_NULL_HANDLE
        && imageView != VK_NULL_HANDLE
        && supportedTargetFormat(format)
        && extent.width > 0
        && extent.height > 0;
}

class VulkanVideoRenderer::Impl {
public:
    struct FrameResources {
        VkCommandBuffer commandBuffer = VK_NULL_HANDLE;
        VkFence submitFence = VK_NULL_HANDLE;
        VkBuffer frameBuffer = VK_NULL_HANDLE;
        VkDeviceMemory frameMemory = VK_NULL_HANDLE;
        VkBuffer parameterBuffer = VK_NULL_HANDLE;
        VkDeviceMemory parameterMemory = VK_NULL_HANDLE;
        VkDescriptorPool descriptorPool = VK_NULL_HANDLE;
        VkDescriptorSet descriptorSet = VK_NULL_HANDLE;
        VkFramebuffer framebuffer = VK_NULL_HANDLE;
        VideoFrame retainedFrame;
        bool submissionPending = false;
    };

    Impl(
        BorrowedVulkanDevice device,
        VulkanCurrentTargetCallback currentTarget)
        : device_(device)
        , currentTarget_(std::move(currentTarget))
    {
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

    bool createBuffer(
        VkDeviceSize size,
        VkBufferUsageFlags usage,
        const void* contents,
        VkBuffer& buffer,
        VkDeviceMemory& memory,
        std::string& error)
    {
        VkBufferCreateInfo bufferInfo {
            VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO,
        };
        bufferInfo.size = size;
        bufferInfo.usage = usage;
        bufferInfo.sharingMode = VK_SHARING_MODE_EXCLUSIVE;
        VkResult result =
            vkCreateBuffer(device_.device, &bufferInfo, nullptr, &buffer);
        if (result != VK_SUCCESS) {
            error = resultError("vkCreateBuffer", result);
            return false;
        }

        VkMemoryRequirements requirements {};
        vkGetBufferMemoryRequirements(
            device_.device,
            buffer,
            &requirements);
        const std::uint32_t type = memoryType(
            requirements.memoryTypeBits,
            VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT
                | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT);
        if (type == std::numeric_limits<std::uint32_t>::max()) {
            error = "Vulkan has no coherent host-visible upload memory";
            return false;
        }
        VkMemoryAllocateInfo allocation {
            VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO,
        };
        allocation.allocationSize = requirements.size;
        allocation.memoryTypeIndex = type;
        result =
            vkAllocateMemory(device_.device, &allocation, nullptr, &memory);
        if (result != VK_SUCCESS) {
            error = resultError("vkAllocateMemory", result);
            return false;
        }
        result = vkBindBufferMemory(device_.device, buffer, memory, 0);
        if (result != VK_SUCCESS) {
            error = resultError("vkBindBufferMemory", result);
            return false;
        }
        void* mapped = nullptr;
        result = vkMapMemory(
            device_.device,
            memory,
            0,
            size,
            0,
            &mapped);
        if (result != VK_SUCCESS) {
            error = resultError("vkMapMemory", result);
            return false;
        }
        std::memcpy(mapped, contents, static_cast<std::size_t>(size));
        vkUnmapMemory(device_.device, memory);
        return true;
    }

    void destroyFrameResources(FrameResources& frame) noexcept
    {
        if (!device_.device) {
            frame.frameBuffer = VK_NULL_HANDLE;
            frame.frameMemory = VK_NULL_HANDLE;
            frame.parameterBuffer = VK_NULL_HANDLE;
            frame.parameterMemory = VK_NULL_HANDLE;
            frame.descriptorPool = VK_NULL_HANDLE;
            frame.descriptorSet = VK_NULL_HANDLE;
            frame.framebuffer = VK_NULL_HANDLE;
            frame.retainedFrame = {};
            frame.submissionPending = false;
            return;
        }
        if (frame.framebuffer) {
            vkDestroyFramebuffer(
                device_.device,
                frame.framebuffer,
                nullptr);
        }
        if (frame.descriptorPool) {
            vkDestroyDescriptorPool(
                device_.device,
                frame.descriptorPool,
                nullptr);
        }
        if (frame.parameterBuffer) {
            vkDestroyBuffer(
                device_.device,
                frame.parameterBuffer,
                nullptr);
        }
        if (frame.parameterMemory) {
            vkFreeMemory(
                device_.device,
                frame.parameterMemory,
                nullptr);
        }
        if (frame.frameBuffer) {
            vkDestroyBuffer(device_.device, frame.frameBuffer, nullptr);
        }
        if (frame.frameMemory) {
            vkFreeMemory(device_.device, frame.frameMemory, nullptr);
        }
        frame.frameBuffer = VK_NULL_HANDLE;
        frame.frameMemory = VK_NULL_HANDLE;
        frame.parameterBuffer = VK_NULL_HANDLE;
        frame.parameterMemory = VK_NULL_HANDLE;
        frame.descriptorPool = VK_NULL_HANDLE;
        frame.descriptorSet = VK_NULL_HANDLE;
        frame.framebuffer = VK_NULL_HANDLE;
        frame.retainedFrame = {};
        frame.submissionPending = false;
    }

    bool waitForSubmission(
        FrameResources& frame,
        std::string& error)
    {
        if (!frame.submissionPending) {
            return true;
        }
        const VkResult result = vkWaitForFences(
            device_.device,
            1,
            &frame.submitFence,
            VK_TRUE,
            std::numeric_limits<std::uint64_t>::max());
        if (result != VK_SUCCESS) {
            error = resultError("vkWaitForFences", result);
            return false;
        }
        frame.submissionPending = false;
        destroyFrameResources(frame);
        return true;
    }

    bool waitForAllSubmissions(std::string& error)
    {
        for (auto& frame : frames_) {
            if (!waitForSubmission(frame, error)) {
                return false;
            }
        }
        return true;
    }

    void destroyPipeline() noexcept
    {
        if (!device_.device) {
            return;
        }
        if (pipeline_) {
            vkDestroyPipeline(device_.device, pipeline_, nullptr);
        }
        if (renderPass_) {
            vkDestroyRenderPass(device_.device, renderPass_, nullptr);
        }
        pipeline_ = VK_NULL_HANDLE;
        renderPass_ = VK_NULL_HANDLE;
        pipelineFormat_ = VK_FORMAT_UNDEFINED;
        pipelineFinalLayout_ = VK_IMAGE_LAYOUT_UNDEFINED;
    }

    bool createCommon(std::string& error)
    {
        std::array<VkDescriptorSetLayoutBinding, 2> bindings {};
        bindings[0].binding = 0;
        bindings[0].descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_BUFFER;
        bindings[0].descriptorCount = 1;
        bindings[0].stageFlags = VK_SHADER_STAGE_FRAGMENT_BIT;
        bindings[1].binding = 1;
        bindings[1].descriptorType = VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER;
        bindings[1].descriptorCount = 1;
        bindings[1].stageFlags = VK_SHADER_STAGE_FRAGMENT_BIT;
        VkDescriptorSetLayoutCreateInfo descriptorInfo {
            VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO,
        };
        descriptorInfo.bindingCount =
            static_cast<std::uint32_t>(bindings.size());
        descriptorInfo.pBindings = bindings.data();
        VkResult result = vkCreateDescriptorSetLayout(
            device_.device,
            &descriptorInfo,
            nullptr,
            &descriptorSetLayout_);
        if (result != VK_SUCCESS) {
            error = resultError("vkCreateDescriptorSetLayout", result);
            return false;
        }

        VkPipelineLayoutCreateInfo pipelineLayoutInfo {
            VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO,
        };
        pipelineLayoutInfo.setLayoutCount = 1;
        pipelineLayoutInfo.pSetLayouts = &descriptorSetLayout_;
        result = vkCreatePipelineLayout(
            device_.device,
            &pipelineLayoutInfo,
            nullptr,
            &pipelineLayout_);
        if (result != VK_SUCCESS) {
            error = resultError("vkCreatePipelineLayout", result);
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
        std::array<VkCommandBuffer, FramesInFlight> commandBuffers {};
        VkCommandBufferAllocateInfo commandInfo {
            VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO,
        };
        commandInfo.commandPool = commandPool_;
        commandInfo.level = VK_COMMAND_BUFFER_LEVEL_PRIMARY;
        commandInfo.commandBufferCount =
            static_cast<std::uint32_t>(commandBuffers.size());
        result = vkAllocateCommandBuffers(
            device_.device,
            &commandInfo,
            commandBuffers.data());
        if (result != VK_SUCCESS) {
            error = resultError("vkAllocateCommandBuffers", result);
            return false;
        }

        for (std::size_t index = 0; index < frames_.size(); ++index) {
            frames_[index].commandBuffer = commandBuffers[index];
            VkFenceCreateInfo fenceInfo {
                VK_STRUCTURE_TYPE_FENCE_CREATE_INFO,
            };
            fenceInfo.flags = VK_FENCE_CREATE_SIGNALED_BIT;
            result = vkCreateFence(
                device_.device,
                &fenceInfo,
                nullptr,
                &frames_[index].submitFence);
            if (result != VK_SUCCESS) {
                error = resultError("vkCreateFence", result);
                return false;
            }
        }
        return true;
    }

    bool createPipeline(
        VkFormat format,
        VkImageLayout finalLayout,
        std::string& error)
    {
        if (pipeline_ && pipelineFormat_ == format
            && pipelineFinalLayout_ == finalLayout) {
            return true;
        }
        if (!waitForAllSubmissions(error)) {
            return false;
        }
        destroyPipeline();

        VkAttachmentDescription attachment {};
        attachment.format = format;
        attachment.samples = VK_SAMPLE_COUNT_1_BIT;
        attachment.loadOp = VK_ATTACHMENT_LOAD_OP_CLEAR;
        attachment.storeOp = VK_ATTACHMENT_STORE_OP_STORE;
        attachment.stencilLoadOp = VK_ATTACHMENT_LOAD_OP_DONT_CARE;
        attachment.stencilStoreOp = VK_ATTACHMENT_STORE_OP_DONT_CARE;
        attachment.initialLayout = VK_IMAGE_LAYOUT_UNDEFINED;
        attachment.finalLayout = finalLayout;
        VkAttachmentReference colorReference {
            0,
            VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL,
        };
        VkSubpassDescription subpass {};
        subpass.pipelineBindPoint = VK_PIPELINE_BIND_POINT_GRAPHICS;
        subpass.colorAttachmentCount = 1;
        subpass.pColorAttachments = &colorReference;
        VkSubpassDependency dependency {};
        dependency.srcSubpass = VK_SUBPASS_EXTERNAL;
        dependency.dstSubpass = 0;
        dependency.srcStageMask =
            VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT;
        dependency.dstStageMask =
            VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT;
        dependency.dstAccessMask =
            VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT;
        VkRenderPassCreateInfo renderPassInfo {
            VK_STRUCTURE_TYPE_RENDER_PASS_CREATE_INFO,
        };
        renderPassInfo.attachmentCount = 1;
        renderPassInfo.pAttachments = &attachment;
        renderPassInfo.subpassCount = 1;
        renderPassInfo.pSubpasses = &subpass;
        renderPassInfo.dependencyCount = 1;
        renderPassInfo.pDependencies = &dependency;
        VkResult result = vkCreateRenderPass(
            device_.device,
            &renderPassInfo,
            nullptr,
            &renderPass_);
        if (result != VK_SUCCESS) {
            error = resultError("vkCreateRenderPass", result);
            return false;
        }

        const auto createShader =
            [&](const unsigned char* bytes,
                std::size_t size,
                VkShaderModule& module) {
                VkShaderModuleCreateInfo info {
                    VK_STRUCTURE_TYPE_SHADER_MODULE_CREATE_INFO,
                };
                info.codeSize = size;
                info.pCode =
                    reinterpret_cast<const std::uint32_t*>(bytes);
                return vkCreateShaderModule(
                    device_.device,
                    &info,
                    nullptr,
                    &module);
            };
        VkShaderModule vertex = VK_NULL_HANDLE;
        VkShaderModule fragment = VK_NULL_HANDLE;
        result = createShader(
            video_vert_spv,
            sizeof(video_vert_spv),
            vertex);
        if (result == VK_SUCCESS) {
            result = createShader(
                video_frag_spv,
                sizeof(video_frag_spv),
                fragment);
        }
        if (result != VK_SUCCESS) {
            if (vertex) {
                vkDestroyShaderModule(device_.device, vertex, nullptr);
            }
            error = resultError("vkCreateShaderModule", result);
            return false;
        }

        std::array<VkPipelineShaderStageCreateInfo, 2> stages {};
        stages[0].sType =
            VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO;
        stages[0].stage = VK_SHADER_STAGE_VERTEX_BIT;
        stages[0].module = vertex;
        stages[0].pName = "main";
        stages[1].sType =
            VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO;
        stages[1].stage = VK_SHADER_STAGE_FRAGMENT_BIT;
        stages[1].module = fragment;
        stages[1].pName = "main";

        VkPipelineVertexInputStateCreateInfo vertexInput {
            VK_STRUCTURE_TYPE_PIPELINE_VERTEX_INPUT_STATE_CREATE_INFO,
        };
        VkPipelineInputAssemblyStateCreateInfo inputAssembly {
            VK_STRUCTURE_TYPE_PIPELINE_INPUT_ASSEMBLY_STATE_CREATE_INFO,
        };
        inputAssembly.topology = VK_PRIMITIVE_TOPOLOGY_TRIANGLE_LIST;
        VkPipelineViewportStateCreateInfo viewportState {
            VK_STRUCTURE_TYPE_PIPELINE_VIEWPORT_STATE_CREATE_INFO,
        };
        viewportState.viewportCount = 1;
        viewportState.scissorCount = 1;
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
        blendAttachment.colorWriteMask =
            VK_COLOR_COMPONENT_R_BIT
            | VK_COLOR_COMPONENT_G_BIT
            | VK_COLOR_COMPONENT_B_BIT
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
        VkGraphicsPipelineCreateInfo pipelineInfo {
            VK_STRUCTURE_TYPE_GRAPHICS_PIPELINE_CREATE_INFO,
        };
        pipelineInfo.stageCount =
            static_cast<std::uint32_t>(stages.size());
        pipelineInfo.pStages = stages.data();
        pipelineInfo.pVertexInputState = &vertexInput;
        pipelineInfo.pInputAssemblyState = &inputAssembly;
        pipelineInfo.pViewportState = &viewportState;
        pipelineInfo.pRasterizationState = &rasterization;
        pipelineInfo.pMultisampleState = &multisample;
        pipelineInfo.pColorBlendState = &blend;
        pipelineInfo.pDynamicState = &dynamic;
        pipelineInfo.layout = pipelineLayout_;
        pipelineInfo.renderPass = renderPass_;
        result = vkCreateGraphicsPipelines(
            device_.device,
            VK_NULL_HANDLE,
            1,
            &pipelineInfo,
            nullptr,
            &pipeline_);
        vkDestroyShaderModule(device_.device, fragment, nullptr);
        vkDestroyShaderModule(device_.device, vertex, nullptr);
        if (result != VK_SUCCESS) {
            error = resultError("vkCreateGraphicsPipelines", result);
            return false;
        }
        pipelineFormat_ = format;
        pipelineFinalLayout_ = finalLayout;
        return true;
    }

    bool prepareFrame(
        const PackedFrame& packed,
        const VulkanRenderTarget& target,
        const VideoFrame& retainedFrame,
        FrameResources& frame,
        std::string& error)
    {
        destroyFrameResources(frame);
        if (!createBuffer(
                packed.bytes.size(),
                VK_BUFFER_USAGE_STORAGE_BUFFER_BIT,
                packed.bytes.data(),
                frame.frameBuffer,
                frame.frameMemory,
                error)
            || !createBuffer(
                sizeof(packed.parameters),
                VK_BUFFER_USAGE_UNIFORM_BUFFER_BIT,
                &packed.parameters,
                frame.parameterBuffer,
                frame.parameterMemory,
                error)) {
            return false;
        }

        const std::array<VkDescriptorPoolSize, 2> poolSizes {
            VkDescriptorPoolSize {
                VK_DESCRIPTOR_TYPE_STORAGE_BUFFER,
                1,
            },
            VkDescriptorPoolSize {
                VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER,
                1,
            },
        };
        VkDescriptorPoolCreateInfo poolInfo {
            VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO,
        };
        poolInfo.maxSets = 1;
        poolInfo.poolSizeCount =
            static_cast<std::uint32_t>(poolSizes.size());
        poolInfo.pPoolSizes = poolSizes.data();
        VkResult result = vkCreateDescriptorPool(
            device_.device,
            &poolInfo,
            nullptr,
            &frame.descriptorPool);
        if (result != VK_SUCCESS) {
            error = resultError("vkCreateDescriptorPool", result);
            return false;
        }
        VkDescriptorSetAllocateInfo descriptorInfo {
            VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO,
        };
        descriptorInfo.descriptorPool = frame.descriptorPool;
        descriptorInfo.descriptorSetCount = 1;
        descriptorInfo.pSetLayouts = &descriptorSetLayout_;
        result = vkAllocateDescriptorSets(
            device_.device,
            &descriptorInfo,
            &frame.descriptorSet);
        if (result != VK_SUCCESS) {
            error = resultError("vkAllocateDescriptorSets", result);
            return false;
        }
        const VkDescriptorBufferInfo frameInfo {
            frame.frameBuffer,
            0,
            packed.bytes.size(),
        };
        const VkDescriptorBufferInfo parameterInfo {
            frame.parameterBuffer,
            0,
            sizeof(packed.parameters),
        };
        std::array<VkWriteDescriptorSet, 2> writes {};
        writes[0].sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET;
        writes[0].dstSet = frame.descriptorSet;
        writes[0].dstBinding = 0;
        writes[0].descriptorCount = 1;
        writes[0].descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_BUFFER;
        writes[0].pBufferInfo = &frameInfo;
        writes[1].sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET;
        writes[1].dstSet = frame.descriptorSet;
        writes[1].dstBinding = 1;
        writes[1].descriptorCount = 1;
        writes[1].descriptorType = VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER;
        writes[1].pBufferInfo = &parameterInfo;
        vkUpdateDescriptorSets(
            device_.device,
            static_cast<std::uint32_t>(writes.size()),
            writes.data(),
            0,
            nullptr);

        VkFramebufferCreateInfo framebufferInfo {
            VK_STRUCTURE_TYPE_FRAMEBUFFER_CREATE_INFO,
        };
        framebufferInfo.renderPass = renderPass_;
        framebufferInfo.attachmentCount = 1;
        framebufferInfo.pAttachments = &target.imageView;
        framebufferInfo.width = target.extent.width;
        framebufferInfo.height = target.extent.height;
        framebufferInfo.layers = 1;
        result = vkCreateFramebuffer(
            device_.device,
            &framebufferInfo,
            nullptr,
            &frame.framebuffer);
        if (result != VK_SUCCESS) {
            error = resultError("vkCreateFramebuffer", result);
            return false;
        }
        frame.retainedFrame = retainedFrame;
        return true;
    }

    bool submit(
        const VulkanRenderTarget& target,
        FrameResources& frame,
        std::string& error)
    {
        VkResult result =
            vkResetCommandBuffer(frame.commandBuffer, 0);
        if (result != VK_SUCCESS) {
            error = resultError("vkResetCommandBuffer", result);
            return false;
        }
        VkCommandBufferBeginInfo beginInfo {
            VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO,
        };
        beginInfo.flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT;
        result =
            vkBeginCommandBuffer(frame.commandBuffer, &beginInfo);
        if (result != VK_SUCCESS) {
            error = resultError("vkBeginCommandBuffer", result);
            return false;
        }
        const VkClearValue clear { { { 0.0F, 0.0F, 0.0F, 1.0F } } };
        VkRenderPassBeginInfo passInfo {
            VK_STRUCTURE_TYPE_RENDER_PASS_BEGIN_INFO,
        };
        passInfo.renderPass = renderPass_;
        passInfo.framebuffer = frame.framebuffer;
        passInfo.renderArea.extent = target.extent;
        passInfo.clearValueCount = 1;
        passInfo.pClearValues = &clear;
        vkCmdBeginRenderPass(
            frame.commandBuffer,
            &passInfo,
            VK_SUBPASS_CONTENTS_INLINE);
        vkCmdBindPipeline(
            frame.commandBuffer,
            VK_PIPELINE_BIND_POINT_GRAPHICS,
            pipeline_);
        vkCmdBindDescriptorSets(
            frame.commandBuffer,
            VK_PIPELINE_BIND_POINT_GRAPHICS,
            pipelineLayout_,
            0,
            1,
            &frame.descriptorSet,
            0,
            nullptr);
        const VkViewport viewport {
            0.0F,
            0.0F,
            static_cast<float>(target.extent.width),
            static_cast<float>(target.extent.height),
            0.0F,
            1.0F,
        };
        const VkRect2D scissor { { 0, 0 }, target.extent };
        vkCmdSetViewport(frame.commandBuffer, 0, 1, &viewport);
        vkCmdSetScissor(frame.commandBuffer, 0, 1, &scissor);
        vkCmdDraw(frame.commandBuffer, 3, 1, 0, 0);
        vkCmdEndRenderPass(frame.commandBuffer);
        result = vkEndCommandBuffer(frame.commandBuffer);
        if (result != VK_SUCCESS) {
            error = resultError("vkEndCommandBuffer", result);
            return false;
        }

        result = vkResetFences(
            device_.device,
            1,
            &frame.submitFence);
        if (result != VK_SUCCESS) {
            error = resultError("vkResetFences", result);
            return false;
        }
        VkSubmitInfo submitInfo { VK_STRUCTURE_TYPE_SUBMIT_INFO };
        if (target.waitSemaphore) {
            submitInfo.waitSemaphoreCount = 1;
            submitInfo.pWaitSemaphores = &target.waitSemaphore;
            submitInfo.pWaitDstStageMask = &target.waitStage;
        }
        submitInfo.commandBufferCount = 1;
        submitInfo.pCommandBuffers = &frame.commandBuffer;
        if (target.signalSemaphore) {
            submitInfo.signalSemaphoreCount = 1;
            submitInfo.pSignalSemaphores = &target.signalSemaphore;
        }
        result = vkQueueSubmit(
            device_.queue,
            1,
            &submitInfo,
            frame.submitFence);
        if (result != VK_SUCCESS) {
            error = resultError("vkQueueSubmit", result);
            return false;
        }
        frame.submissionPending = true;
        if (target.waitUntilCompleted
            && !waitForSubmission(frame, error)) {
            return false;
        }
        return true;
    }

    void close() noexcept
    {
        std::lock_guard<std::mutex> renderLock(renderMutex_);
        if (device_.device) {
            std::string ignored;
            waitForAllSubmissions(ignored);
            for (auto& frame : frames_) {
                destroyFrameResources(frame);
            }
            destroyPipeline();
            for (auto& frame : frames_) {
                if (frame.submitFence) {
                    vkDestroyFence(
                        device_.device,
                        frame.submitFence,
                        nullptr);
                }
            }
            if (commandPool_) {
                vkDestroyCommandPool(
                    device_.device,
                    commandPool_,
                    nullptr);
            }
            if (pipelineLayout_) {
                vkDestroyPipelineLayout(
                    device_.device,
                    pipelineLayout_,
                    nullptr);
            }
            if (descriptorSetLayout_) {
                vkDestroyDescriptorSetLayout(
                    device_.device,
                    descriptorSetLayout_,
                    nullptr);
            }
        }
        commandPool_ = VK_NULL_HANDLE;
        for (auto& frame : frames_) {
            frame.commandBuffer = VK_NULL_HANDLE;
            frame.submitFence = VK_NULL_HANDLE;
            frame.submissionPending = false;
        }
        nextFrame_ = 0;
        pipelineLayout_ = VK_NULL_HANDLE;
        descriptorSetLayout_ = VK_NULL_HANDLE;
        std::lock_guard<std::mutex> stateLock(stateMutex_);
        open_ = false;
    }

    mutable std::mutex stateMutex_;
    std::mutex renderMutex_;
    BorrowedVulkanDevice device_;
    VulkanCurrentTargetCallback currentTarget_;
    EventCallback eventCallback_;
    VideoRenderConfig config_;
    bool open_ = false;

    VkDescriptorSetLayout descriptorSetLayout_ = VK_NULL_HANDLE;
    VkPipelineLayout pipelineLayout_ = VK_NULL_HANDLE;
    VkCommandPool commandPool_ = VK_NULL_HANDLE;
    VkRenderPass renderPass_ = VK_NULL_HANDLE;
    VkPipeline pipeline_ = VK_NULL_HANDLE;
    VkFormat pipelineFormat_ = VK_FORMAT_UNDEFINED;
    VkImageLayout pipelineFinalLayout_ = VK_IMAGE_LAYOUT_UNDEFINED;
    std::array<FrameResources, FramesInFlight> frames_;
    std::size_t nextFrame_ = 0;
};

VulkanVideoRenderer::VulkanVideoRenderer(
    BorrowedVulkanDevice device,
    VulkanCurrentTargetCallback currentTarget)
    : impl_(std::make_unique<Impl>(
          device,
          std::move(currentTarget)))
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
    if (!impl_) {
        return false;
    }
    std::lock_guard<std::mutex> renderLock(impl_->renderMutex_);

    VideoRenderConfig config;
    VulkanCurrentTargetCallback currentTarget;
    {
        std::lock_guard<std::mutex> stateLock(impl_->stateMutex_);
        if (!impl_->open_) {
            config = {};
        } else {
            config = impl_->config_;
            currentTarget = impl_->currentTarget_;
        }
    }
    if (!config.surfaceSize.isValid()) {
        impl_->notify(
            VideoRenderEventType::Error,
            "The Vulkan renderer is not open");
        return false;
    }
    auto& frameResources = impl_->frames_[impl_->nextFrame_];
    std::string error;
    if (!impl_->waitForSubmission(frameResources, error)) {
        impl_->notify(VideoRenderEventType::Error, std::move(error));
        return false;
    }
    const VulkanRenderTarget target =
        currentTarget ? currentTarget() : VulkanRenderTarget {};
    if (!target.isValid()) {
        impl_->notify(
            VideoRenderEventType::SurfaceLost,
            "The current Vulkan target is unavailable");
        return false;
    }
    if (target.extent.width
            != static_cast<std::uint32_t>(config.surfaceSize.width)
        || target.extent.height
            != static_cast<std::uint32_t>(config.surfaceSize.height)) {
        impl_->notify(
            VideoRenderEventType::Error,
            "The current Vulkan target extent does not match the configured surface");
        return false;
    }
    if (frame.hasHardwareFrame()) {
        impl_->notify(
            VideoRenderEventType::Error,
            "The Vulkan renderer has no interop for this hardware frame");
        return false;
    }

    PackedFrame packed;
    if (!packFrame(frame, config, packed, error)
        || !impl_->createPipeline(
            target.format,
            target.finalLayout,
            error)
        || !impl_->prepareFrame(
            packed,
            target,
            frame,
            frameResources,
            error)
        || !impl_->submit(target, frameResources, error)) {
        impl_->notify(VideoRenderEventType::Error, std::move(error));
        return false;
    }
    impl_->nextFrame_ =
        (impl_->nextFrame_ + 1) % impl_->frames_.size();
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

} // namespace qtav
