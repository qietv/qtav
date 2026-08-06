// SPDX-License-Identifier: LGPL-2.1-or-later

#include <qtav/ohcodec_vulkan_interop.h>

#include <native_buffer/native_buffer.h>
#include <native_image/native_image.h>
#include <vulkan/vulkan_ohos.h>

#include <atomic>
#include <cstdlib>
#include <limits>
#include <mutex>
#include <utility>
#include <unistd.h>

namespace qtav {
namespace {

const char* resultName(VkResult result) noexcept
{
    switch (result) {
    case VK_SUCCESS:
        return "VK_SUCCESS";
    case VK_ERROR_OUT_OF_HOST_MEMORY:
        return "VK_ERROR_OUT_OF_HOST_MEMORY";
    case VK_ERROR_OUT_OF_DEVICE_MEMORY:
        return "VK_ERROR_OUT_OF_DEVICE_MEMORY";
    case VK_ERROR_FORMAT_NOT_SUPPORTED:
        return "VK_ERROR_FORMAT_NOT_SUPPORTED";
    case VK_ERROR_INVALID_EXTERNAL_HANDLE:
        return "VK_ERROR_INVALID_EXTERNAL_HANDLE";
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

std::uint32_t firstMemoryType(std::uint32_t bits) noexcept
{
    for (std::uint32_t index = 0; index < 32; ++index) {
        if ((bits & (1U << index)) != 0U) {
            return index;
        }
    }
    return std::numeric_limits<std::uint32_t>::max();
}

bool directPlaneFormat(VkFormat format) noexcept
{
    switch (format) {
    case VK_FORMAT_G8_B8R8_2PLANE_420_UNORM:
    case VK_FORMAT_G8_B8_R8_3PLANE_420_UNORM:
    case VK_FORMAT_G10X6_B10X6R10X6_2PLANE_420_UNORM_3PACK16:
    case VK_FORMAT_G10X6_B10X6_R10X6_3PLANE_420_UNORM_3PACK16:
        return true;
    default:
        return false;
    }
}

bool compatibleNativeFormat(std::int32_t format) noexcept
{
    return format == NATIVEBUFFER_PIXEL_FMT_YCBCR_420_SP
        || format == NATIVEBUFFER_PIXEL_FMT_YCBCR_420_P
        || format == NATIVEBUFFER_PIXEL_FMT_YCBCR_P010;
}

VkFormat explicitFormatFromOpaqueId(std::uint64_t externalFormat) noexcept
{
    switch (externalFormat) {
    case static_cast<std::uint64_t>(
        VK_FORMAT_G8_B8R8_2PLANE_420_UNORM):
        return VK_FORMAT_G8_B8R8_2PLANE_420_UNORM;
    case static_cast<std::uint64_t>(
        VK_FORMAT_G10X6_B10X6R10X6_2PLANE_420_UNORM_3PACK16):
        return VK_FORMAT_G10X6_B10X6R10X6_2PLANE_420_UNORM_3PACK16;
    default:
        return VK_FORMAT_UNDEFINED;
    }
}

bool probeOpaqueExternalFormatObjects(
    const BorrowedVulkanDevice& device,
    OH_NativeBuffer* nativeBuffer,
    const OH_NativeBuffer_Config& nativeConfig,
    const VkNativeBufferPropertiesOHOS& properties,
    const VkNativeBufferFormatPropertiesOHOS& formatProperties,
    bool samplerYcbcrConversionEnabled,
    std::string& detail)
{
    if (!samplerYcbcrConversionEnabled) {
        detail =
            "opaque direct probe stopped: samplerYcbcrConversion was not enabled on VkDevice";
        return false;
    }
    if (!nativeBuffer || formatProperties.format != VK_FORMAT_UNDEFINED
        || formatProperties.externalFormat == 0) {
        detail =
            "opaque direct probe stopped: NativeBuffer has no opaque externalFormat";
        return false;
    }

    VkSamplerYcbcrConversion conversion = VK_NULL_HANDLE;
    VkSampler sampler = VK_NULL_HANDLE;
    VkImage image = VK_NULL_HANDLE;
    VkDeviceMemory memory = VK_NULL_HANDLE;
    VkImageView imageView = VK_NULL_HANDLE;
    VkDescriptorSetLayout descriptorSetLayout = VK_NULL_HANDLE;
    const auto cleanup = [&] {
        if (descriptorSetLayout) {
            vkDestroyDescriptorSetLayout(
                device.device,
                descriptorSetLayout,
                nullptr);
        }
        if (imageView) {
            vkDestroyImageView(device.device, imageView, nullptr);
        }
        if (sampler) {
            vkDestroySampler(device.device, sampler, nullptr);
        }
        if (conversion) {
            vkDestroySamplerYcbcrConversion(
                device.device,
                conversion,
                nullptr);
        }
        if (image) {
            vkDestroyImage(device.device, image, nullptr);
        }
        if (memory) {
            vkFreeMemory(device.device, memory, nullptr);
        }
    };
    const auto failed = [&](const char* operation, VkResult result) {
        detail = "opaque direct probe stopped at "
            + resultError(operation, result);
        cleanup();
        return false;
    };

    VkExternalFormatOHOS conversionExternalFormat {
        VK_STRUCTURE_TYPE_EXTERNAL_FORMAT_OHOS,
    };
    conversionExternalFormat.externalFormat =
        formatProperties.externalFormat;
    VkSamplerYcbcrConversionCreateInfo conversionInfo {
        VK_STRUCTURE_TYPE_SAMPLER_YCBCR_CONVERSION_CREATE_INFO,
    };
    conversionInfo.pNext = &conversionExternalFormat;
    conversionInfo.format = VK_FORMAT_UNDEFINED;
    conversionInfo.ycbcrModel = formatProperties.suggestedYcbcrModel;
    conversionInfo.ycbcrRange = formatProperties.suggestedYcbcrRange;
    conversionInfo.components =
        formatProperties.samplerYcbcrConversionComponents;
    conversionInfo.xChromaOffset =
        formatProperties.suggestedXChromaOffset;
    conversionInfo.yChromaOffset =
        formatProperties.suggestedYChromaOffset;
    conversionInfo.chromaFilter =
        (formatProperties.formatFeatures
            & VK_FORMAT_FEATURE_SAMPLED_IMAGE_YCBCR_CONVERSION_LINEAR_FILTER_BIT)
            != 0U
        ? VK_FILTER_LINEAR
        : VK_FILTER_NEAREST;
    VkResult result = vkCreateSamplerYcbcrConversion(
        device.device,
        &conversionInfo,
        nullptr,
        &conversion);
    if (result != VK_SUCCESS) {
        return failed("vkCreateSamplerYcbcrConversion", result);
    }

    VkSamplerYcbcrConversionInfo samplerConversion {
        VK_STRUCTURE_TYPE_SAMPLER_YCBCR_CONVERSION_INFO,
    };
    samplerConversion.conversion = conversion;
    VkSamplerCreateInfo samplerInfo {
        VK_STRUCTURE_TYPE_SAMPLER_CREATE_INFO,
    };
    samplerInfo.pNext = &samplerConversion;
    samplerInfo.magFilter = conversionInfo.chromaFilter;
    samplerInfo.minFilter = conversionInfo.chromaFilter;
    samplerInfo.mipmapMode = VK_SAMPLER_MIPMAP_MODE_NEAREST;
    samplerInfo.addressModeU = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE;
    samplerInfo.addressModeV = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE;
    samplerInfo.addressModeW = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE;
    samplerInfo.maxLod = 1.0F;
    result = vkCreateSampler(
        device.device,
        &samplerInfo,
        nullptr,
        &sampler);
    if (result != VK_SUCCESS) {
        return failed("vkCreateSampler(YCbCr)", result);
    }

    VkExternalFormatOHOS imageExternalFormat {
        VK_STRUCTURE_TYPE_EXTERNAL_FORMAT_OHOS,
    };
    imageExternalFormat.externalFormat = formatProperties.externalFormat;
    VkExternalMemoryImageCreateInfo externalMemory {
        VK_STRUCTURE_TYPE_EXTERNAL_MEMORY_IMAGE_CREATE_INFO,
    };
    externalMemory.pNext = &imageExternalFormat;
    externalMemory.handleTypes =
        VK_EXTERNAL_MEMORY_HANDLE_TYPE_OH_NATIVE_BUFFER_BIT_OHOS;
    VkImageCreateInfo imageInfo {
        VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO,
    };
    imageInfo.pNext = &externalMemory;
    imageInfo.imageType = VK_IMAGE_TYPE_2D;
    imageInfo.format = VK_FORMAT_UNDEFINED;
    imageInfo.extent = {
        static_cast<std::uint32_t>(nativeConfig.width),
        static_cast<std::uint32_t>(nativeConfig.height),
        1,
    };
    imageInfo.mipLevels = 1;
    imageInfo.arrayLayers = 1;
    imageInfo.samples = VK_SAMPLE_COUNT_1_BIT;
    imageInfo.tiling = VK_IMAGE_TILING_OPTIMAL;
    imageInfo.usage = VK_IMAGE_USAGE_SAMPLED_BIT;
    imageInfo.sharingMode = VK_SHARING_MODE_EXCLUSIVE;
    imageInfo.initialLayout = VK_IMAGE_LAYOUT_UNDEFINED;
    result = vkCreateImage(
        device.device,
        &imageInfo,
        nullptr,
        &image);
    if (result != VK_SUCCESS) {
        return failed("vkCreateImage(opaque OH_NativeBuffer)", result);
    }

    VkMemoryRequirements requirements {};
    vkGetImageMemoryRequirements(
        device.device,
        image,
        &requirements);
    const std::uint32_t memoryType = firstMemoryType(
        properties.memoryTypeBits & requirements.memoryTypeBits);
    if (memoryType == std::numeric_limits<std::uint32_t>::max()) {
        detail =
            "opaque direct probe stopped: no compatible Vulkan memory type";
        cleanup();
        return false;
    }
    VkMemoryDedicatedAllocateInfo dedicated {
        VK_STRUCTURE_TYPE_MEMORY_DEDICATED_ALLOCATE_INFO,
    };
    dedicated.image = image;
    VkImportNativeBufferInfoOHOS import {
        VK_STRUCTURE_TYPE_IMPORT_NATIVE_BUFFER_INFO_OHOS,
    };
    import.pNext = &dedicated;
    import.buffer = nativeBuffer;
    VkMemoryAllocateInfo allocation {
        VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO,
    };
    allocation.pNext = &import;
    allocation.allocationSize = properties.allocationSize;
    allocation.memoryTypeIndex = memoryType;
    result = vkAllocateMemory(
        device.device,
        &allocation,
        nullptr,
        &memory);
    if (result != VK_SUCCESS) {
        return failed("vkAllocateMemory(opaque OH_NativeBuffer)", result);
    }
    result = vkBindImageMemory(device.device, image, memory, 0);
    if (result != VK_SUCCESS) {
        return failed("vkBindImageMemory(opaque OH_NativeBuffer)", result);
    }

    VkSamplerYcbcrConversionInfo viewConversion {
        VK_STRUCTURE_TYPE_SAMPLER_YCBCR_CONVERSION_INFO,
    };
    viewConversion.conversion = conversion;
    VkImageViewCreateInfo viewInfo {
        VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO,
    };
    viewInfo.pNext = &viewConversion;
    viewInfo.image = image;
    viewInfo.viewType = VK_IMAGE_VIEW_TYPE_2D;
    viewInfo.format = VK_FORMAT_UNDEFINED;
    viewInfo.components = {
        VK_COMPONENT_SWIZZLE_IDENTITY,
        VK_COMPONENT_SWIZZLE_IDENTITY,
        VK_COMPONENT_SWIZZLE_IDENTITY,
        VK_COMPONENT_SWIZZLE_IDENTITY,
    };
    viewInfo.subresourceRange.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
    viewInfo.subresourceRange.levelCount = 1;
    viewInfo.subresourceRange.layerCount = 1;
    result = vkCreateImageView(
        device.device,
        &viewInfo,
        nullptr,
        &imageView);
    if (result != VK_SUCCESS) {
        return failed("vkCreateImageView(opaque OH_NativeBuffer)", result);
    }

    VkDescriptorSetLayoutBinding binding {};
    binding.binding = 0;
    binding.descriptorType = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
    binding.descriptorCount = 1;
    binding.stageFlags = VK_SHADER_STAGE_FRAGMENT_BIT;
    binding.pImmutableSamplers = &sampler;
    VkDescriptorSetLayoutCreateInfo layoutInfo {
        VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO,
    };
    layoutInfo.bindingCount = 1;
    layoutInfo.pBindings = &binding;
    result = vkCreateDescriptorSetLayout(
        device.device,
        &layoutInfo,
        nullptr,
        &descriptorSetLayout);
    if (result != VK_SUCCESS) {
        return failed(
            "vkCreateDescriptorSetLayout(opaque OH_NativeBuffer)",
            result);
    }

    detail =
        "opaque direct probe created VkSamplerYcbcrConversion, VkSampler, imported VkImage memory, VkImageView, and immutable-sampler descriptor layout; shader sampling was not attempted";
    cleanup();
    return true;
}

struct AtomicStatistics {
    std::atomic<std::uint64_t> nativeBuffersAcquired { 0 };
    std::atomic<std::uint64_t> nativeBuffersImported { 0 };
    std::atomic<std::uint64_t> directPlaneImports { 0 };
    std::atomic<std::uint64_t> opaqueExternalImports { 0 };
    std::atomic<std::uint64_t> forcedVkFormatImports { 0 };
    std::atomic<std::uint64_t> forcedVkFormatNativeSamples { 0 };
    std::atomic<std::uint64_t> forcedVkFormatLibplaceboImports { 0 };
    std::atomic<std::uint64_t> codecOutputsQueued { 0 };
    std::atomic<std::uint64_t> frameAvailableCallbacks { 0 };
    std::atomic<std::uint64_t> acquireFencesImported { 0 };
    std::atomic<std::uint64_t> opaqueExternalObjectProbes { 0 };
    std::atomic<std::uint64_t> opaqueExternalObjectProbeSuccesses { 0 };
    std::atomic<std::uint64_t> opaqueFormatsRejected { 0 };
    std::atomic<std::uint64_t> unsupportedFormatsRejected { 0 };
    std::atomic<std::uint64_t> outputsReleasedAfterGpu { 0 };
    std::atomic<std::uint64_t> normalizationPasses { 0 };
    std::atomic<std::int32_t> lastNativeFormat { 0 };
    std::atomic<std::int32_t> lastVulkanFormat {
        static_cast<std::int32_t>(VK_FORMAT_UNDEFINED),
    };
    std::atomic<std::int32_t> lastForcedVulkanFormat {
        static_cast<std::int32_t>(VK_FORMAT_UNDEFINED),
    };
    std::atomic<std::uint64_t> lastExternalFormat { 0 };
};

struct FrameKey {
    std::uintptr_t output = 0;
    std::uint32_t generation = 0;
    std::int64_t timestampMilliseconds = 0;

    explicit operator bool() const noexcept
    {
        return output != 0 && generation != 0;
    }
};

bool operator==(const FrameKey& left, const FrameKey& right) noexcept
{
    return left.output == right.output
        && left.generation == right.generation
        && left.timestampMilliseconds == right.timestampMilliseconds;
}

FrameKey frameKey(const VideoFrame& frame) noexcept
{
    FrameKey result;
    if (!frame || !frame.hasHardwareFrame()) {
        return result;
    }
    const NativeHandle output = frame.hardwareFrame().nativeHandle(
        HardwareHandleType::Frame);
    result.output = output.value;
    result.generation = output.subresource;
    result.timestampMilliseconds = frame.timestamp();
    return result;
}

struct SharedState {
    ~SharedState()
    {
        if (consumerSurface) {
            OH_NativeImage_UnsetOnFrameAvailableListener(consumerSurface);
        }
        // OHCodecSurface owns an extra producer-window reference. Release it
        // before destroying the consumer surface and its original owner.
        surface = {};
        if (consumerSurface) {
            OH_NativeImage_Destroy(&consumerSurface);
        }
    }

    BorrowedVulkanDevice device;
    OHCodecSurface surface;
    OH_NativeImage* consumerSurface = nullptr;
    PFN_vkGetNativeBufferPropertiesOHOS getNativeBufferProperties = nullptr;
    PFN_vkImportSemaphoreFdKHR importSemaphoreFd = nullptr;
    bool samplerYcbcrConversionEnabled = false;
    OHCodecVulkanExternalFormatProbeMode externalFormatProbeMode =
        OHCodecVulkanExternalFormatProbeMode::Disabled;
    std::mutex mutex;
    FrameKey queuedFrame;
    bool frameAvailable = false;
    VulkanHardwareFrameInterop::FrameAvailableCallback callback;
    AtomicStatistics statistics;
};

void closeDescriptor(int& descriptor) noexcept
{
    if (descriptor >= 0) {
        close(descriptor);
        descriptor = -1;
    }
}

void onFrameAvailable(void* context)
{
    auto* state = static_cast<SharedState*>(context);
    if (!state) {
        return;
    }
    VulkanHardwareFrameInterop::FrameAvailableCallback callback;
    {
        std::lock_guard<std::mutex> lock(state->mutex);
        state->frameAvailable = true;
        callback = state->callback;
    }
    state->statistics.frameAvailableCallbacks.fetch_add(
        1,
        std::memory_order_relaxed);
    if (callback) {
        callback();
    }
}

class OHOSNativeBufferTexture final : public VulkanTextureFrame {
public:
    static std::shared_ptr<OHOSNativeBufferTexture> create(
        std::shared_ptr<SharedState> state,
        OHNativeWindowBuffer* windowBuffer,
        OH_NativeBuffer* nativeBuffer,
        int acquireFence,
        int frameWidth,
        int frameHeight,
        VulkanHardwareImportStatus& status,
        std::string& error)
    {
        auto result = std::shared_ptr<OHOSNativeBufferTexture>(
            new OHOSNativeBufferTexture(
                std::move(state),
                windowBuffer,
                nativeBuffer,
                acquireFence,
                frameWidth,
                frameHeight));
        if (!result->initialize(status, error)) {
            return {};
        }
        return result;
    }

    ~OHOSNativeBufferTexture() override
    {
        if (!submitted_ && acquireSemaphore_
            && !producerWaitQueued_.load(std::memory_order_acquire)) {
            VkPipelineStageFlags stage =
                VK_PIPELINE_STAGE_ALL_COMMANDS_BIT;
            VkSubmitInfo submit {
                VK_STRUCTURE_TYPE_SUBMIT_INFO,
            };
            submit.waitSemaphoreCount = 1;
            submit.pWaitSemaphores = &acquireSemaphore_;
            submit.pWaitDstStageMask = &stage;
            if (vkQueueSubmit(
                    state_->device.queue,
                    1,
                    &submit,
                    VK_NULL_HANDLE)
                == VK_SUCCESS) {
                producerWaitQueued_.store(
                    true,
                    std::memory_order_release);
                vkQueueWaitIdle(state_->device.queue);
            }
        }
        if (imageView_) {
            vkDestroyImageView(state_->device.device, imageView_, nullptr);
            imageView_ = VK_NULL_HANDLE;
        }
        if (sampler_) {
            vkDestroySampler(state_->device.device, sampler_, nullptr);
            sampler_ = VK_NULL_HANDLE;
        }
        if (conversion_) {
            vkDestroySamplerYcbcrConversion(
                state_->device.device,
                conversion_,
                nullptr);
            conversion_ = VK_NULL_HANDLE;
        }
        if (image_) {
            vkDestroyImage(state_->device.device, image_, nullptr);
            image_ = VK_NULL_HANDLE;
        }
        if (memory_) {
            vkFreeMemory(state_->device.device, memory_, nullptr);
            memory_ = VK_NULL_HANDLE;
        }
        if (acquireSemaphore_) {
            vkDestroySemaphore(
                state_->device.device,
                acquireSemaphore_,
                nullptr);
        }
        int releaseFence = acquireFence_;
        acquireFence_ = -1;
        const bool released = windowBuffer_
            && OH_NativeImage_ReleaseNativeWindowBuffer(
                   state_->consumerSurface,
                   windowBuffer_,
                   releaseFence)
                == 0;
        if (!released) {
            closeDescriptor(releaseFence);
        }
        if (submitted_ && released) {
            state_->statistics.outputsReleasedAfterGpu.fetch_add(
                1,
                std::memory_order_relaxed);
        }
        if (nativeBuffer_) {
            OH_NativeBuffer_Unreference(nativeBuffer_);
        }
        if (windowBuffer_) {
            OH_NativeWindow_NativeObjectUnreference(windowBuffer_);
        }
    }

    int width() const noexcept override
    {
        return width_;
    }

    int height() const noexcept override
    {
        return height_;
    }

    VkImage image() const noexcept override
    {
        return image_;
    }

    VkImageView imageView() const noexcept override
    {
        return imageView_;
    }

    VkSampler sampler() const noexcept override
    {
        return sampler_;
    }

    VkFormat format() const noexcept override
    {
        return format_;
    }

    VkImageUsageFlags usage() const noexcept override
    {
        return VK_IMAGE_USAGE_SAMPLED_BIT;
    }

    VkSemaphore acquireSemaphore() const noexcept override
    {
        return acquireSemaphore_;
    }

    VkSemaphore releaseSemaphore() const noexcept override
    {
        return VK_NULL_HANDLE;
    }

    VulkanNormalizedSourceRect normalizedSourceRect() const noexcept override
    {
        return sourceRect_;
    }

    void releaseToProducer() noexcept override
    {
        // VulkanVideoRenderer retains this object behind its completion
        // timeline. Destruction therefore happens only after sampling has
        // completed. Destruction is therefore the point at which the acquired
        // consumer buffer returns to the OH_ConsumerSurface.
        if (!submitted_ && format_ == VK_FORMAT_UNDEFINED) {
            state_->statistics.normalizationPasses.fetch_add(
                1,
                std::memory_order_relaxed);
        }
        submitted_ = true;
    }

    bool waitForProducer(std::string& detail) noexcept override
    {
        if (!acquireSemaphore_
            || producerWaitQueued_.load(std::memory_order_acquire)) {
            return true;
        }
        const VkPipelineStageFlags stage =
            VK_PIPELINE_STAGE_ALL_COMMANDS_BIT;
        VkSubmitInfo submit {
            VK_STRUCTURE_TYPE_SUBMIT_INFO,
        };
        submit.waitSemaphoreCount = 1;
        submit.pWaitSemaphores = &acquireSemaphore_;
        submit.pWaitDstStageMask = &stage;
        const VkResult result = vkQueueSubmit(
            state_->device.queue,
            1,
            &submit,
            VK_NULL_HANDLE);
        if (result != VK_SUCCESS) {
            detail = resultError(
                "vkQueueSubmit(OH_ConsumerSurface acquire fence)",
                result);
            return false;
        }
        producerWaitQueued_.store(true, std::memory_order_release);
        return true;
    }

private:
    OHOSNativeBufferTexture(
        std::shared_ptr<SharedState> state,
        OHNativeWindowBuffer* windowBuffer,
        OH_NativeBuffer* nativeBuffer,
        int acquireFence,
        int frameWidth,
        int frameHeight) noexcept
        : state_(std::move(state))
        , windowBuffer_(windowBuffer)
        , nativeBuffer_(nativeBuffer)
        , acquireFence_(acquireFence)
        , frameWidth_(frameWidth)
        , frameHeight_(frameHeight)
    {
    }

    bool initialize(
        VulkanHardwareImportStatus& status,
        std::string& error)
    {
        OH_NativeBuffer_Config nativeConfig {};
        OH_NativeBuffer_GetConfig(nativeBuffer_, &nativeConfig);
        state_->statistics.lastNativeFormat.store(
            nativeConfig.format,
            std::memory_order_relaxed);
        if (nativeConfig.width <= 0 || nativeConfig.height <= 0
            || nativeConfig.width < frameWidth_
            || nativeConfig.height < frameHeight_) {
            status = VulkanHardwareImportStatus::Error;
            error = "OH_NativeBuffer reported invalid or undersized dimensions: buffer="
                + std::to_string(nativeConfig.width) + 'x'
                + std::to_string(nativeConfig.height) + " frame="
                + std::to_string(frameWidth_) + 'x'
                + std::to_string(frameHeight_);
            return false;
        }
        if ((static_cast<std::uint64_t>(nativeConfig.usage)
                & NATIVEBUFFER_USAGE_HW_TEXTURE)
            == 0U) {
            status = VulkanHardwareImportStatus::Unsupported;
            error = "The decoder-owned OH_NativeBuffer is not GPU-texture capable: usage="
                + std::to_string(nativeConfig.usage);
            state_->statistics.unsupportedFormatsRejected.fetch_add(
                1,
                std::memory_order_relaxed);
            return false;
        }
        if (!compatibleNativeFormat(nativeConfig.format)) {
            status = VulkanHardwareImportStatus::Unsupported;
            error = "The decoder-owned OH_NativeBuffer has no accepted raw Y/Cb/Cr plane contract: nativeFormat="
                + std::to_string(nativeConfig.format);
            state_->statistics.unsupportedFormatsRejected.fetch_add(
                1,
                std::memory_order_relaxed);
            return false;
        }

        if (acquireFence_ >= 0) {
            VkSemaphoreCreateInfo semaphoreInfo {
                VK_STRUCTURE_TYPE_SEMAPHORE_CREATE_INFO,
            };
            VkResult semaphoreResult = vkCreateSemaphore(
                state_->device.device,
                &semaphoreInfo,
                nullptr,
                &acquireSemaphore_);
            if (semaphoreResult != VK_SUCCESS) {
                status = VulkanHardwareImportStatus::Error;
                error = resultError(
                    "vkCreateSemaphore(OH_ConsumerSurface acquire)",
                    semaphoreResult);
                return false;
            }
            VkImportSemaphoreFdInfoKHR importInfo {
                VK_STRUCTURE_TYPE_IMPORT_SEMAPHORE_FD_INFO_KHR,
            };
            importInfo.semaphore = acquireSemaphore_;
            importInfo.flags = VK_SEMAPHORE_IMPORT_TEMPORARY_BIT;
            importInfo.handleType =
                VK_EXTERNAL_SEMAPHORE_HANDLE_TYPE_SYNC_FD_BIT;
            importInfo.fd = acquireFence_;
            semaphoreResult = state_->importSemaphoreFd(
                state_->device.device,
                &importInfo);
            if (semaphoreResult != VK_SUCCESS) {
                status = VulkanHardwareImportStatus::Error;
                error = resultError(
                    "vkImportSemaphoreFdKHR(OH_ConsumerSurface acquire)",
                    semaphoreResult);
                return false;
            }
            acquireFence_ = -1;
            state_->statistics.acquireFencesImported.fetch_add(
                1,
                std::memory_order_relaxed);
        }

        VkNativeBufferFormatPropertiesOHOS formatProperties {
            VK_STRUCTURE_TYPE_NATIVE_BUFFER_FORMAT_PROPERTIES_OHOS,
        };
        VkNativeBufferPropertiesOHOS properties {
            VK_STRUCTURE_TYPE_NATIVE_BUFFER_PROPERTIES_OHOS,
        };
        properties.pNext = &formatProperties;
        const VkResult queryResult = state_->getNativeBufferProperties(
            state_->device.device,
            nativeBuffer_,
            &properties);
        if (queryResult != VK_SUCCESS) {
            status = queryResult == VK_ERROR_FORMAT_NOT_SUPPORTED
                ? VulkanHardwareImportStatus::Unsupported
                : VulkanHardwareImportStatus::Error;
            error = resultError(
                "vkGetNativeBufferPropertiesOHOS",
                queryResult);
            return false;
        }
        state_->statistics.lastVulkanFormat.store(
            static_cast<std::int32_t>(formatProperties.format),
            std::memory_order_relaxed);
        state_->statistics.lastExternalFormat.store(
            formatProperties.externalFormat,
            std::memory_order_relaxed);
        const bool queriedOpaqueExternal =
            formatProperties.format == VK_FORMAT_UNDEFINED;
        const bool forceVkFormat = queriedOpaqueExternal
            && state_->externalFormatProbeMode
                != OHCodecVulkanExternalFormatProbeMode::Disabled;
        const bool forcedNativeSampling = forceVkFormat
            && state_->externalFormatProbeMode
                == OHCodecVulkanExternalFormatProbeMode::ForcedVkFormatNativeSampling;
        const bool forcedLibplacebo = forceVkFormat
            && state_->externalFormatProbeMode
                == OHCodecVulkanExternalFormatProbeMode::ForcedVkFormatLibplacebo;
        VkFormat importFormat = formatProperties.format;
        if (forceVkFormat) {
            importFormat = explicitFormatFromOpaqueId(
                formatProperties.externalFormat);
            state_->statistics.lastForcedVulkanFormat.store(
                static_cast<std::int32_t>(importFormat),
                std::memory_order_relaxed);
            if (importFormat == VK_FORMAT_UNDEFINED) {
                status = VulkanHardwareImportStatus::Unsupported;
                error = "forced VkFormat probe has no recognized NV12/P010 mapping for externalFormat="
                    + std::to_string(formatProperties.externalFormat);
                state_->statistics.unsupportedFormatsRejected.fetch_add(
                    1,
                    std::memory_order_relaxed);
                return false;
            }
        }
        const bool useOpaqueExternalFormat =
            queriedOpaqueExternal && !forceVkFormat;
        const bool createYcbcrSampler =
            useOpaqueExternalFormat || forcedNativeSampling;
        if (createYcbcrSampler) {
            if (!state_->samplerYcbcrConversionEnabled
                || (useOpaqueExternalFormat
                    && formatProperties.externalFormat == 0)) {
                status = VulkanHardwareImportStatus::Unsupported;
                error = "OH_NativeBuffer exposes an opaque Vulkan external format, but samplerYcbcrConversion is unavailable: nativeFormat="
                    + std::to_string(nativeConfig.format)
                    + " externalFormat="
                    + std::to_string(formatProperties.externalFormat);
                state_->statistics.opaqueFormatsRejected.fetch_add(
                    1,
                    std::memory_order_relaxed);
                return false;
            }
            if (useOpaqueExternalFormat
                && state_->statistics.opaqueExternalObjectProbes.load(
                    std::memory_order_relaxed)
                == 0) {
                state_->statistics.opaqueExternalObjectProbes.fetch_add(
                    1,
                    std::memory_order_relaxed);
                std::string probeDetail;
                if (!probeOpaqueExternalFormatObjects(
                        state_->device,
                        nativeBuffer_,
                        nativeConfig,
                        properties,
                        formatProperties,
                        state_->samplerYcbcrConversionEnabled,
                        probeDetail)) {
                    status = VulkanHardwareImportStatus::Error;
                    error = probeDetail;
                    state_->statistics.opaqueFormatsRejected.fetch_add(
                        1,
                        std::memory_order_relaxed);
                    return false;
                }
                state_->statistics.opaqueExternalObjectProbeSuccesses.fetch_add(
                    1,
                    std::memory_order_relaxed);
            }
            VkExternalFormatOHOS conversionExternalFormat {
                VK_STRUCTURE_TYPE_EXTERNAL_FORMAT_OHOS,
            };
            conversionExternalFormat.externalFormat =
                formatProperties.externalFormat;
            VkSamplerYcbcrConversionCreateInfo conversionInfo {
                VK_STRUCTURE_TYPE_SAMPLER_YCBCR_CONVERSION_CREATE_INFO,
            };
            conversionInfo.pNext = useOpaqueExternalFormat
                ? &conversionExternalFormat
                : nullptr;
            conversionInfo.format = importFormat;
            conversionInfo.ycbcrModel =
                formatProperties.suggestedYcbcrModel;
            conversionInfo.ycbcrRange =
                formatProperties.suggestedYcbcrRange;
            conversionInfo.components =
                formatProperties.samplerYcbcrConversionComponents;
            conversionInfo.xChromaOffset =
                formatProperties.suggestedXChromaOffset;
            conversionInfo.yChromaOffset =
                formatProperties.suggestedYChromaOffset;
            conversionInfo.chromaFilter =
                (formatProperties.formatFeatures
                    & VK_FORMAT_FEATURE_SAMPLED_IMAGE_YCBCR_CONVERSION_LINEAR_FILTER_BIT)
                    != 0U
                ? VK_FILTER_LINEAR
                : VK_FILTER_NEAREST;
            VkResult result = vkCreateSamplerYcbcrConversion(
                state_->device.device,
                &conversionInfo,
                nullptr,
                &conversion_);
            if (result != VK_SUCCESS) {
                status = VulkanHardwareImportStatus::Error;
                error = resultError(
                    "vkCreateSamplerYcbcrConversion(OH_NativeBuffer)",
                    result);
                return false;
            }
            VkSamplerYcbcrConversionInfo samplerConversion {
                VK_STRUCTURE_TYPE_SAMPLER_YCBCR_CONVERSION_INFO,
            };
            samplerConversion.conversion = conversion_;
            VkSamplerCreateInfo samplerInfo {
                VK_STRUCTURE_TYPE_SAMPLER_CREATE_INFO,
            };
            samplerInfo.pNext = &samplerConversion;
            samplerInfo.magFilter = conversionInfo.chromaFilter;
            samplerInfo.minFilter = conversionInfo.chromaFilter;
            samplerInfo.mipmapMode = VK_SAMPLER_MIPMAP_MODE_NEAREST;
            samplerInfo.addressModeU = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE;
            samplerInfo.addressModeV = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE;
            samplerInfo.addressModeW = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE;
            samplerInfo.maxLod = 1.0F;
            result = vkCreateSampler(
                state_->device.device,
                &samplerInfo,
                nullptr,
                &sampler_);
            if (result != VK_SUCCESS) {
                status = VulkanHardwareImportStatus::Error;
                error = resultError(
                    "vkCreateSampler(YCbCr OH_NativeBuffer)",
                    result);
                return false;
            }
        } else if (!directPlaneFormat(importFormat)
            || (formatProperties.formatFeatures
                & VK_FORMAT_FEATURE_SAMPLED_IMAGE_BIT)
                == 0U) {
            status = VulkanHardwareImportStatus::Unsupported;
            error = "OH_NativeBuffer exposes no supported sampled Vulkan 4:2:0 plane format: VkFormat="
                + std::to_string(
                    static_cast<std::int32_t>(importFormat))
                + " features="
                + std::to_string(formatProperties.formatFeatures);
            state_->statistics.unsupportedFormatsRejected.fetch_add(
                1,
                std::memory_order_relaxed);
            return false;
        }

        VkExternalMemoryImageCreateInfo externalMemory {
            VK_STRUCTURE_TYPE_EXTERNAL_MEMORY_IMAGE_CREATE_INFO,
        };
        VkExternalFormatOHOS imageExternalFormat {
            VK_STRUCTURE_TYPE_EXTERNAL_FORMAT_OHOS,
        };
        imageExternalFormat.externalFormat =
            formatProperties.externalFormat;
        externalMemory.pNext = useOpaqueExternalFormat
            ? &imageExternalFormat
            : nullptr;
        externalMemory.handleTypes =
            VK_EXTERNAL_MEMORY_HANDLE_TYPE_OH_NATIVE_BUFFER_BIT_OHOS;
        VkImageCreateInfo imageInfo {
            VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO,
        };
        imageInfo.pNext = &externalMemory;
        imageInfo.imageType = VK_IMAGE_TYPE_2D;
        imageInfo.format = importFormat;
        imageInfo.extent = {
            static_cast<std::uint32_t>(nativeConfig.width),
            static_cast<std::uint32_t>(nativeConfig.height),
            1,
        };
        imageInfo.mipLevels = 1;
        imageInfo.arrayLayers = 1;
        imageInfo.samples = VK_SAMPLE_COUNT_1_BIT;
        imageInfo.tiling = VK_IMAGE_TILING_OPTIMAL;
        imageInfo.usage = VK_IMAGE_USAGE_SAMPLED_BIT;
        imageInfo.sharingMode = VK_SHARING_MODE_EXCLUSIVE;
        imageInfo.initialLayout = VK_IMAGE_LAYOUT_UNDEFINED;
        VkResult result = vkCreateImage(
            state_->device.device,
            &imageInfo,
            nullptr,
            &image_);
        if (result != VK_SUCCESS) {
            status = result == VK_ERROR_FORMAT_NOT_SUPPORTED
                ? VulkanHardwareImportStatus::Unsupported
                : VulkanHardwareImportStatus::Error;
            error = resultError(
                "vkCreateImage(OH_NativeBuffer)",
                result);
            return false;
        }

        VkMemoryRequirements requirements {};
        vkGetImageMemoryRequirements(
            state_->device.device,
            image_,
            &requirements);
        const std::uint32_t memoryType = firstMemoryType(
            properties.memoryTypeBits & requirements.memoryTypeBits);
        if (memoryType == std::numeric_limits<std::uint32_t>::max()) {
            status = VulkanHardwareImportStatus::Unsupported;
            error = "OH_NativeBuffer exposes no compatible Vulkan memory type";
            return false;
        }

        VkMemoryDedicatedAllocateInfo dedicated {
            VK_STRUCTURE_TYPE_MEMORY_DEDICATED_ALLOCATE_INFO,
        };
        dedicated.image = image_;
        VkImportNativeBufferInfoOHOS import {
            VK_STRUCTURE_TYPE_IMPORT_NATIVE_BUFFER_INFO_OHOS,
        };
        import.pNext = &dedicated;
        import.buffer = nativeBuffer_;
        VkMemoryAllocateInfo allocation {
            VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO,
        };
        allocation.pNext = &import;
        allocation.allocationSize = properties.allocationSize;
        allocation.memoryTypeIndex = memoryType;
        result = vkAllocateMemory(
            state_->device.device,
            &allocation,
            nullptr,
            &memory_);
        if (result != VK_SUCCESS) {
            status = result == VK_ERROR_INVALID_EXTERNAL_HANDLE
                    || result == VK_ERROR_FORMAT_NOT_SUPPORTED
                ? VulkanHardwareImportStatus::Unsupported
                : VulkanHardwareImportStatus::Error;
            error = resultError(
                "vkAllocateMemory(OH_NativeBuffer)",
                result);
            return false;
        }
        result = vkBindImageMemory(
            state_->device.device,
            image_,
            memory_,
            0);
        if (result != VK_SUCCESS) {
            status = VulkanHardwareImportStatus::Error;
            error = resultError(
                "vkBindImageMemory(OH_NativeBuffer)",
                result);
            return false;
        }

        if (createYcbcrSampler) {
            VkSamplerYcbcrConversionInfo viewConversion {
                VK_STRUCTURE_TYPE_SAMPLER_YCBCR_CONVERSION_INFO,
            };
            viewConversion.conversion = conversion_;
            VkImageViewCreateInfo viewInfo {
                VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO,
            };
            viewInfo.pNext = &viewConversion;
            viewInfo.image = image_;
            viewInfo.viewType = VK_IMAGE_VIEW_TYPE_2D;
            viewInfo.format = importFormat;
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
            result = vkCreateImageView(
                state_->device.device,
                &viewInfo,
                nullptr,
                &imageView_);
            if (result != VK_SUCCESS) {
                status = VulkanHardwareImportStatus::Error;
                error = resultError(
                    "vkCreateImageView(opaque OH_NativeBuffer)",
                    result);
                return false;
            }
        }

        width_ = nativeConfig.width;
        height_ = nativeConfig.height;
        format_ = forcedNativeSampling
            ? VK_FORMAT_UNDEFINED
            : importFormat;
        sourceRect_ = {
            0.0F,
            0.0F,
            static_cast<float>(frameWidth_)
                / static_cast<float>(width_),
            static_cast<float>(frameHeight_)
                / static_cast<float>(height_),
        };
        state_->statistics.nativeBuffersImported.fetch_add(
            1,
            std::memory_order_relaxed);
        if (forceVkFormat) {
            state_->statistics.forcedVkFormatImports.fetch_add(
                1,
                std::memory_order_relaxed);
            if (forcedNativeSampling) {
                state_->statistics.forcedVkFormatNativeSamples.fetch_add(
                    1,
                    std::memory_order_relaxed);
                state_->statistics.opaqueExternalImports.fetch_add(
                    1,
                    std::memory_order_relaxed);
            } else if (forcedLibplacebo) {
                state_->statistics.forcedVkFormatLibplaceboImports.fetch_add(
                    1,
                    std::memory_order_relaxed);
                state_->statistics.directPlaneImports.fetch_add(
                    1,
                    std::memory_order_relaxed);
            }
        } else if (useOpaqueExternalFormat) {
            state_->statistics.opaqueExternalImports.fetch_add(
                1,
                std::memory_order_relaxed);
        } else {
            state_->statistics.directPlaneImports.fetch_add(
                1,
                std::memory_order_relaxed);
        }
        status = VulkanHardwareImportStatus::Ready;
        return true;
    }

    std::shared_ptr<SharedState> state_;
    OHNativeWindowBuffer* windowBuffer_ = nullptr;
    OH_NativeBuffer* nativeBuffer_ = nullptr;
    int acquireFence_ = -1;
    int frameWidth_ = 0;
    int frameHeight_ = 0;
    int width_ = 0;
    int height_ = 0;
    VkImage image_ = VK_NULL_HANDLE;
    VkDeviceMemory memory_ = VK_NULL_HANDLE;
    VkImageView imageView_ = VK_NULL_HANDLE;
    VkSamplerYcbcrConversion conversion_ = VK_NULL_HANDLE;
    VkSampler sampler_ = VK_NULL_HANDLE;
    VkSemaphore acquireSemaphore_ = VK_NULL_HANDLE;
    VkFormat format_ = VK_FORMAT_UNDEFINED;
    VulkanNormalizedSourceRect sourceRect_;
    bool submitted_ = false;
    std::atomic<bool> producerWaitQueued_ { false };
};

} // namespace

class OHCodecVulkanInterop::Impl final {
public:
    Impl(
        BorrowedVulkanDevice device,
        OHCodecVulkanInteropConfig config)
    {
        if (!device.isValid()) {
            error_ = "OHCodec Vulkan interop requires a valid borrowed Vulkan device";
            return;
        }
        if (!config.ohosExternalMemoryEnabled
            || !config.foreignQueueFamilyEnabled
            || !config.syncFdSemaphoreEnabled) {
            error_ = "OHCodec Vulkan interop requires explicitly enabled VK_OHOS_external_memory, VK_EXT_queue_family_foreign, and VK_KHR_external_semaphore_fd";
            return;
        }
        auto getProperties = reinterpret_cast<
            PFN_vkGetNativeBufferPropertiesOHOS>(
                vkGetDeviceProcAddr(
                    device.device,
                    "vkGetNativeBufferPropertiesOHOS"));
        if (!getProperties) {
            error_ = "vkGetNativeBufferPropertiesOHOS is unavailable on the borrowed device";
            return;
        }
        auto importSemaphoreFd = reinterpret_cast<
            PFN_vkImportSemaphoreFdKHR>(
                vkGetDeviceProcAddr(
                    device.device,
                    "vkImportSemaphoreFdKHR"));
        if (!importSemaphoreFd) {
            error_ = "vkImportSemaphoreFdKHR is unavailable on the borrowed device";
            return;
        }
        state_ = std::make_shared<SharedState>();
        state_->device = device;
        state_->getNativeBufferProperties = getProperties;
        state_->importSemaphoreFd = importSemaphoreFd;
        state_->samplerYcbcrConversionEnabled =
            config.samplerYcbcrConversionEnabled;
        state_->externalFormatProbeMode =
            config.externalFormatProbeMode;
        state_->consumerSurface = OH_ConsumerSurface_Create();
        if (!state_->consumerSurface) {
            error_ = "OH_ConsumerSurface_Create failed for OHCodec Vulkan interop";
            state_.reset();
            return;
        }
        if (OH_ConsumerSurface_SetDefaultUsage(
                state_->consumerSurface,
                NATIVEBUFFER_USAGE_HW_TEXTURE)
            != 0) {
            error_ = "OH_ConsumerSurface_SetDefaultUsage(HW_TEXTURE) failed";
            state_.reset();
            return;
        }
        if (config.width > 0 && config.height > 0
            && OH_ConsumerSurface_SetDefaultSize(
                   state_->consumerSurface,
                   config.width,
                   config.height)
                != 0) {
            error_ = "OH_ConsumerSurface_SetDefaultSize failed";
            state_.reset();
            return;
        }
        OH_OnFrameAvailableListener listener {};
        listener.context = state_.get();
        listener.onFrameAvailable = &onFrameAvailable;
        if (OH_NativeImage_SetOnFrameAvailableListener(
                state_->consumerSurface,
                listener)
            != 0) {
            error_ = "OH_NativeImage_SetOnFrameAvailableListener failed";
            state_.reset();
            return;
        }
        OHNativeWindow* window = OH_NativeImage_AcquireNativeWindow(
            state_->consumerSurface);
        if (!window) {
            error_ = "Could not acquire the OHCodec Vulkan producer window";
            state_.reset();
            return;
        }
        if (OH_NativeWindow_NativeWindowHandleOpt(
                   window,
                   SET_FORMAT,
                   NATIVEBUFFER_PIXEL_FMT_YCBCR_420_SP)
                != 0) {
            error_ = "Could not request explicit YCbCr 4:2:0 output from the OHCodec Vulkan producer window";
            state_.reset();
            return;
        }
        state_->surface = OHCodecSurface(window);
        if (!state_->surface) {
            error_ = "Could not acquire or retain the OHCodec Vulkan producer window";
            state_.reset();
            return;
        }
    }

    bool supports(const HardwareFrame& frame) const noexcept
    {
        if (!state_ || !frame
            || frame.deviceType() != HardwareDeviceType::OHCodec) {
            return false;
        }
        const NativeHandle output =
            frame.nativeHandle(HardwareHandleType::Frame);
        const NativeHandle surface =
            frame.nativeHandle(HardwareHandleType::Surface);
        return output && surface
            && output.subresource == state_->surface.generation()
            && surface.subresource == state_->surface.generation()
            && surface.value == reinterpret_cast<std::uintptr_t>(
                state_->surface.nativeWindow());
    }

    VulkanHardwareImportStatus prepareFrame(
        const VideoFrame& frame,
        std::string& detail)
    {
        if (!frame || !frame.hasHardwareFrame()) {
            detail = "The frame has no OHCodec hardware output";
            return VulkanHardwareImportStatus::Unsupported;
        }
        if (!supports(frame.hardwareFrame())) {
            detail = "The OHCodec frame belongs to a stale, foreign, or detached decoder surface";
            return VulkanHardwareImportStatus::Stale;
        }
        const FrameKey key = frameKey(frame);
        {
            std::lock_guard<std::mutex> lock(state_->mutex);
            if (state_->queuedFrame) {
                if (!(state_->queuedFrame == key)) {
                    detail = "Another OHCodec output is awaiting its exact consumer buffer";
                    return VulkanHardwareImportStatus::Pending;
                }
                detail.clear();
                return state_->frameAvailable
                    ? VulkanHardwareImportStatus::Ready
                    : VulkanHardwareImportStatus::Pending;
            }
            state_->queuedFrame = key;
            state_->frameAvailable = false;
        }

        OHCodecFrame output = ohCodecFrame(frame, state_->surface);
        if (!output || !output.isPending() || !output.present()) {
            std::lock_guard<std::mutex> lock(state_->mutex);
            state_->queuedFrame = {};
            state_->frameAvailable = false;
            detail = "Could not queue the OHCodec output into the private OH_ConsumerSurface";
            return VulkanHardwareImportStatus::Error;
        }
        state_->statistics.codecOutputsQueued.fetch_add(
            1,
            std::memory_order_relaxed);
        detail.clear();
        return VulkanHardwareImportStatus::Pending;
    }

    VulkanHardwareImportResult importFrame(const VideoFrame& frame)
    {
        std::string detail;
        const VulkanHardwareImportStatus prepared =
            prepareFrame(frame, detail);
        if (prepared != VulkanHardwareImportStatus::Ready) {
            return { prepared, {}, std::move(detail) };
        }
        OHNativeWindowBuffer* windowBuffer = nullptr;
        int acquireFence = -1;
        {
            std::lock_guard<std::mutex> lock(state_->mutex);
            if (!(state_->queuedFrame == frameKey(frame))
                || !state_->frameAvailable) {
                return {
                    VulkanHardwareImportStatus::Pending,
                    {},
                    {},
                };
            }
            const int32_t result =
                OH_NativeImage_AcquireNativeWindowBuffer(
                    state_->consumerSurface,
                    &windowBuffer,
                    &acquireFence);
            if (result != 0 || !windowBuffer) {
                return {
                    VulkanHardwareImportStatus::Pending,
                    {},
                    {},
                };
            }
            state_->queuedFrame = {};
            state_->frameAvailable = false;
        }
        if (OH_NativeWindow_NativeObjectReference(windowBuffer) != 0) {
            const int32_t releaseResult =
                OH_NativeImage_ReleaseNativeWindowBuffer(
                    state_->consumerSurface,
                    windowBuffer,
                    acquireFence);
            if (releaseResult != 0) {
                closeDescriptor(acquireFence);
            }
            return {
                VulkanHardwareImportStatus::Error,
                {},
                "Could not retain the acquired OHNativeWindowBuffer",
            };
        }
        OH_NativeBuffer* nativeBuffer = nullptr;
        if (OH_NativeBuffer_FromNativeWindowBuffer(
                windowBuffer,
                &nativeBuffer)
                != 0
            || !nativeBuffer
            || OH_NativeBuffer_Reference(nativeBuffer) != 0) {
            const int32_t releaseResult =
                OH_NativeImage_ReleaseNativeWindowBuffer(
                    state_->consumerSurface,
                    windowBuffer,
                    acquireFence);
            if (releaseResult != 0) {
                closeDescriptor(acquireFence);
            }
            OH_NativeWindow_NativeObjectUnreference(windowBuffer);
            return {
                VulkanHardwareImportStatus::Error,
                {},
                "Could not expose or retain the acquired OH_NativeBuffer",
            };
        }
        state_->statistics.nativeBuffersAcquired.fetch_add(
            1,
            std::memory_order_relaxed);
        VulkanHardwareImportStatus status =
            VulkanHardwareImportStatus::Error;
        auto texture = OHOSNativeBufferTexture::create(
            state_,
            windowBuffer,
            nativeBuffer,
            acquireFence,
            frame.width(),
            frame.height(),
            status,
            detail);
        if (!texture) {
            setLastError(detail);
            return { status, {}, std::move(detail) };
        }
        return {
            VulkanHardwareImportStatus::Ready,
            std::move(texture),
            {},
        };
    }

    void setLastError(const std::string& error)
    {
        std::lock_guard<std::mutex> lock(errorMutex_);
        error_ = error;
    }

    void setFrameAvailableCallback(
        VulkanHardwareFrameInterop::FrameAvailableCallback callback)
    {
        std::lock_guard<std::mutex> lock(state_->mutex);
        state_->callback = std::move(callback);
    }

    std::string lastError() const
    {
        std::lock_guard<std::mutex> lock(errorMutex_);
        return error_;
    }

    std::shared_ptr<SharedState> state_;
    mutable std::mutex errorMutex_;
    std::string error_;
};

OHCodecVulkanInterop::OHCodecVulkanInterop(
    BorrowedVulkanDevice device,
    OHCodecVulkanInteropConfig config)
    : impl_(std::make_unique<Impl>(
          device,
          config))
{
}

OHCodecVulkanInterop::~OHCodecVulkanInterop() = default;
OHCodecVulkanInterop::OHCodecVulkanInterop(
    OHCodecVulkanInterop&&) noexcept = default;
OHCodecVulkanInterop& OHCodecVulkanInterop::operator=(
    OHCodecVulkanInterop&&) noexcept = default;

OHCodecVulkanInterop::operator bool() const noexcept
{
    return isValid();
}

bool OHCodecVulkanInterop::isValid() const noexcept
{
    return impl_ && static_cast<bool>(impl_->state_);
}

std::string OHCodecVulkanInterop::lastError() const
{
    return impl_ ? impl_->lastError()
                 : "The OHCodec Vulkan interop object is empty";
}

OHCodecSurface OHCodecVulkanInterop::surface() const noexcept
{
    return isValid() ? impl_->state_->surface : OHCodecSurface {};
}

BorrowedVulkanDevice OHCodecVulkanInterop::device() const noexcept
{
    return isValid() ? impl_->state_->device : BorrowedVulkanDevice {};
}

HardwareInteropCapabilities OHCodecVulkanInterop::capabilities() const
{
    HardwareInteropCapabilities result;
    result.sourceDevices = { HardwareDeviceType::OHCodec };
    result.targetDevice = HardwareDeviceType::Vulkan;
    result.zeroCopy = true;
    result.cpuFallback = false;
    return result;
}

bool OHCodecVulkanInterop::supports(
    const HardwareFrame& frame) const noexcept
{
    return impl_ && impl_->supports(frame);
}

VulkanHardwareImportStatus OHCodecVulkanInterop::prepareFrame(
    const VideoFrame& frame,
    std::string& detail)
{
    if (!impl_) {
        detail = "The OHCodec Vulkan interop object is empty";
        return VulkanHardwareImportStatus::Error;
    }
    return impl_->prepareFrame(frame, detail);
}

VulkanHardwareImportResult OHCodecVulkanInterop::importFrame(
    const VideoFrame& frame)
{
    if (!impl_) {
        return {
            VulkanHardwareImportStatus::Error,
            {},
            "The OHCodec Vulkan interop object is empty",
        };
    }
    return impl_->importFrame(frame);
}

void OHCodecVulkanInterop::setFrameAvailableCallback(
    FrameAvailableCallback callback)
{
    if (isValid()) {
        impl_->setFrameAvailableCallback(std::move(callback));
    }
}

OHCodecVulkanInteropStatistics
OHCodecVulkanInterop::statistics() const noexcept
{
    OHCodecVulkanInteropStatistics result;
    if (!isValid()) {
        return result;
    }
    const AtomicStatistics& source = impl_->state_->statistics;
    result.nativeBuffersAcquired = source.nativeBuffersAcquired.load(
        std::memory_order_relaxed);
    result.nativeBuffersImported = source.nativeBuffersImported.load(
        std::memory_order_relaxed);
    result.directPlaneImports = source.directPlaneImports.load(
        std::memory_order_relaxed);
    result.opaqueExternalImports = source.opaqueExternalImports.load(
        std::memory_order_relaxed);
    result.forcedVkFormatImports = source.forcedVkFormatImports.load(
        std::memory_order_relaxed);
    result.forcedVkFormatNativeSamples =
        source.forcedVkFormatNativeSamples.load(
            std::memory_order_relaxed);
    result.forcedVkFormatLibplaceboImports =
        source.forcedVkFormatLibplaceboImports.load(
            std::memory_order_relaxed);
    result.codecOutputsQueued = source.codecOutputsQueued.load(
        std::memory_order_relaxed);
    result.frameAvailableCallbacks =
        source.frameAvailableCallbacks.load(
            std::memory_order_relaxed);
    result.acquireFencesImported = source.acquireFencesImported.load(
        std::memory_order_relaxed);
    result.opaqueExternalObjectProbes =
        source.opaqueExternalObjectProbes.load(
            std::memory_order_relaxed);
    result.opaqueExternalObjectProbeSuccesses =
        source.opaqueExternalObjectProbeSuccesses.load(
            std::memory_order_relaxed);
    result.opaqueFormatsRejected = source.opaqueFormatsRejected.load(
        std::memory_order_relaxed);
    result.unsupportedFormatsRejected =
        source.unsupportedFormatsRejected.load(
            std::memory_order_relaxed);
    result.outputsReleasedAfterGpu =
        source.outputsReleasedAfterGpu.load(
            std::memory_order_relaxed);
    result.normalizationPasses = source.normalizationPasses.load(
        std::memory_order_relaxed);
    result.lastNativeFormat = source.lastNativeFormat.load(
        std::memory_order_relaxed);
    result.lastVulkanFormat = static_cast<VkFormat>(
        source.lastVulkanFormat.load(std::memory_order_relaxed));
    result.lastForcedVulkanFormat = static_cast<VkFormat>(
        source.lastForcedVulkanFormat.load(
            std::memory_order_relaxed));
    result.lastExternalFormat = source.lastExternalFormat.load(
        std::memory_order_relaxed);
    return result;
}

} // namespace qtav
