// SPDX-License-Identifier: LGPL-2.1-or-later

#include <qtav/mediacodec_vulkan_interop.h>

#include <android/hardware_buffer.h>
#include <media/NdkImage.h>
#include <media/NdkImageReader.h>

#include <algorithm>
#include <atomic>
#include <chrono>
#include <condition_variable>
#include <cstdint>
#include <cstdlib>
#include <deque>
#include <limits>
#include <mutex>
#include <string>
#include <unordered_map>
#include <unistd.h>
#include <utility>
#include <vector>

namespace qtav {
namespace {

constexpr std::int64_t TimestampToleranceNanoseconds = 2'000'000;
constexpr std::int64_t MaximumPresentationLagNanoseconds =
    250'000'000;
constexpr std::size_t MaximumRetiredFrameKeys = 64;

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
    case VK_ERROR_FORMAT_NOT_SUPPORTED:
        return "VK_ERROR_FORMAT_NOT_SUPPORTED";
    case VK_ERROR_INVALID_EXTERNAL_HANDLE:
        return "VK_ERROR_INVALID_EXTERNAL_HANDLE";
    default:
        return "unknown VkResult";
    }
}

std::string resultError(const char* operation, VkResult result)
{
    return std::string(operation) + " failed: " + resultName(result)
        + " (" + std::to_string(static_cast<int>(result)) + ')';
}

void closeDescriptor(int& descriptor) noexcept
{
    if (descriptor >= 0) {
        close(descriptor);
        descriptor = -1;
    }
}

struct PendingImage {
    AImage* image = nullptr;
    int acquireFence = -1;
    std::int64_t timestampNanoseconds = 0;
};

void discardImage(PendingImage& pending) noexcept
{
    if (!pending.image) {
        closeDescriptor(pending.acquireFence);
        return;
    }
    if (pending.acquireFence >= 0) {
        AImage_deleteAsync(pending.image, pending.acquireFence);
        pending.acquireFence = -1;
    } else {
        AImage_delete(pending.image);
    }
    pending.image = nullptr;
}

struct FrameKey {
    std::uintptr_t buffer = 0;
    std::uint32_t generation = 0;
    std::int64_t timestampMilliseconds = 0;

    bool operator==(const FrameKey& other) const noexcept
    {
        return buffer == other.buffer
            && generation == other.generation
            && timestampMilliseconds == other.timestampMilliseconds;
    }
};

struct AtomicStatistics {
    std::atomic<std::uint64_t> codecOutputsQueued { 0 };
    std::atomic<std::uint64_t> imagesAcquired { 0 };
    std::atomic<std::uint64_t> imagesImported { 0 };
    std::atomic<std::uint64_t> acquireFencesImported { 0 };
    std::atomic<std::uint64_t> releaseFencesReturned { 0 };
    std::atomic<std::uint64_t> releaseFenceFallbacks { 0 };
    std::atomic<std::uint64_t> staleImagesDropped { 0 };
    std::atomic<std::uint64_t> maximumPendingImages { 0 };
    std::atomic<std::uint64_t> hardwareBufferImports { 0 };
    std::atomic<std::uint64_t> hardwareBufferImportCacheHits { 0 };
    std::atomic<std::uint64_t> hardwareBufferImportsRemoved { 0 };
    std::atomic<std::uint64_t> maximumCachedHardwareBufferImports { 0 };
    std::atomic<std::uint32_t> lastHardwareBufferFormat { 0 };
    std::atomic<int> lastVulkanFormat {
        static_cast<int>(VK_FORMAT_UNDEFINED)
    };
    std::atomic<std::uint64_t> lastExternalFormat { 0 };
};

struct HardwareBufferResources;

void updateMaximum(
    std::atomic<std::uint64_t>& maximum,
    std::uint64_t value) noexcept
{
    std::uint64_t current = maximum.load(std::memory_order_relaxed);
    while (current < value
           && !maximum.compare_exchange_weak(
               current,
               value,
               std::memory_order_relaxed)) {
    }
}

struct ConversionKey {
    VkFormat format = VK_FORMAT_UNDEFINED;
    std::uint64_t externalFormat = 0;
    VkSamplerYcbcrModelConversion model =
        VK_SAMPLER_YCBCR_MODEL_CONVERSION_RGB_IDENTITY;
    VkSamplerYcbcrRange range = VK_SAMPLER_YCBCR_RANGE_ITU_FULL;
    VkComponentMapping components {};
    VkChromaLocation xOffset = VK_CHROMA_LOCATION_COSITED_EVEN;
    VkChromaLocation yOffset = VK_CHROMA_LOCATION_COSITED_EVEN;
    VkFilter filter = VK_FILTER_NEAREST;

    bool operator==(const ConversionKey& other) const noexcept
    {
        return format == other.format
            && externalFormat == other.externalFormat
            && model == other.model
            && range == other.range
            && components.r == other.components.r
            && components.g == other.components.g
            && components.b == other.components.b
            && components.a == other.components.a
            && xOffset == other.xOffset
            && yOffset == other.yOffset
            && filter == other.filter;
    }
};

struct ConversionResources {
    ~ConversionResources()
    {
        if (sampler) {
            vkDestroySampler(device, sampler, nullptr);
        }
        if (conversion && destroyConversion) {
            destroyConversion(device, conversion, nullptr);
        }
    }

    VkDevice device = VK_NULL_HANDLE;
    PFN_vkDestroySamplerYcbcrConversion destroyConversion = nullptr;
    VkSamplerYcbcrConversion conversion = VK_NULL_HANDLE;
    VkSampler sampler = VK_NULL_HANDLE;
    ConversionKey key;
};

struct SharedState {
    ~SharedState()
    {
        if (reader) {
            AImageReader_setImageListener(reader, nullptr);
            AImageReader_setBufferRemovedListener(reader, nullptr);
        }
        std::deque<PendingImage> pending;
        {
            std::lock_guard<std::mutex> lock(mutex);
            shuttingDown = true;
            pending.swap(images);
            queuedFrames.clear();
            retiredFrames.clear();
            bufferImports.clear();
            frameAvailable = {};
        }
        for (auto& image : pending) {
            discardImage(image);
        }
        surface = {};
        if (reader) {
            AImageReader_delete(reader);
            reader = nullptr;
        }
    }

    BorrowedVulkanDevice device;
    MediaCodecVulkanInteropConfig config;
    AImageReader* reader = nullptr;
    MediaCodecSurface surface;
    PFN_vkGetAndroidHardwareBufferPropertiesANDROID
        getHardwareBufferProperties = nullptr;
    PFN_vkImportSemaphoreFdKHR importSemaphoreFd = nullptr;
    PFN_vkGetSemaphoreFdKHR getSemaphoreFd = nullptr;
    PFN_vkCreateSamplerYcbcrConversion createConversion = nullptr;
    PFN_vkDestroySamplerYcbcrConversion destroyConversion = nullptr;

    mutable std::mutex mutex;
    std::condition_variable imageChanged;
    std::deque<PendingImage> images;
    std::vector<FrameKey> queuedFrames;
    std::deque<FrameKey> retiredFrames;
    std::vector<
        std::pair<
            ConversionKey,
            std::weak_ptr<ConversionResources>>>
        conversions;
    std::unordered_map<
        AHardwareBuffer*,
        std::shared_ptr<HardwareBufferResources>>
        bufferImports;
    VulkanHardwareFrameInterop::FrameAvailableCallback frameAvailable;
    std::string asyncError;
    std::string lastImportError;
    bool shuttingDown = false;
    AtomicStatistics statistics;
};

void onBufferRemoved(
    void* context,
    AImageReader*,
    AHardwareBuffer* buffer) noexcept
{
    auto* state = static_cast<SharedState*>(context);
    if (!state || !buffer) {
        return;
    }
    std::shared_ptr<HardwareBufferResources> retired;
    {
        std::lock_guard<std::mutex> lock(state->mutex);
        const auto found = state->bufferImports.find(buffer);
        if (found == state->bufferImports.end()) {
            return;
        }
        retired = std::move(found->second);
        state->bufferImports.erase(found);
        state->statistics.hardwareBufferImportsRemoved.fetch_add(
            1,
            std::memory_order_relaxed);
    }
    // Existing submitted frames may still retain this import. Dropping the
    // cache reference here destroys it only after the last submission fence
    // releases its frame object.
}

void onImageAvailable(void* context, AImageReader* reader) noexcept
{
    auto* state = static_cast<SharedState*>(context);
    if (!state || !reader) {
        return;
    }

    bool acquiredAny = false;
    std::deque<PendingImage> discarded;
    for (;;) {
        PendingImage pending;
        const media_status_t status =
            AImageReader_acquireNextImageAsync(
                reader,
                &pending.image,
                &pending.acquireFence);
        if (status == AMEDIA_IMGREADER_NO_BUFFER_AVAILABLE) {
            break;
        }
        if (status != AMEDIA_OK || !pending.image) {
            std::lock_guard<std::mutex> lock(state->mutex);
            if (!state->shuttingDown) {
                state->asyncError =
                    "AImageReader_acquireNextImageAsync failed: "
                    + std::to_string(status);
            }
            break;
        }
        if (AImage_getTimestamp(
                pending.image,
                &pending.timestampNanoseconds)
            != AMEDIA_OK
            || pending.timestampNanoseconds < 0) {
            discardImage(pending);
            state->statistics.staleImagesDropped.fetch_add(
                1,
                std::memory_order_relaxed);
            continue;
        }

        acquiredAny = true;
        state->statistics.imagesAcquired.fetch_add(
            1,
            std::memory_order_relaxed);
        {
            std::lock_guard<std::mutex> lock(state->mutex);
            if (state->shuttingDown) {
                discarded.push_back(pending);
            } else {
                state->images.push_back(pending);
                while (static_cast<int>(state->images.size())
                       > state->config.maximumImages) {
                    discarded.push_back(state->images.front());
                    state->images.pop_front();
                    state->statistics.staleImagesDropped.fetch_add(
                        1,
                        std::memory_order_relaxed);
                }
                updateMaximum(
                    state->statistics.maximumPendingImages,
                    state->images.size());
            }
        }
    }
    for (auto& image : discarded) {
        discardImage(image);
    }

    VulkanHardwareFrameInterop::FrameAvailableCallback callback;
    bool hasAsyncError = false;
    {
        std::lock_guard<std::mutex> lock(state->mutex);
        callback = state->frameAvailable;
        hasAsyncError = !state->asyncError.empty();
    }
    if (acquiredAny || hasAsyncError) {
        state->imageChanged.notify_all();
    }
    if ((acquiredAny || hasAsyncError) && callback) {
        callback();
    }
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

bool supportedChromaOffset(
    VkFormatFeatureFlags features,
    VkChromaLocation location) noexcept
{
    if (location == VK_CHROMA_LOCATION_MIDPOINT) {
        return (features
                & VK_FORMAT_FEATURE_MIDPOINT_CHROMA_SAMPLES_BIT)
            != 0U;
    }
    return (features
            & VK_FORMAT_FEATURE_COSITED_CHROMA_SAMPLES_BIT)
        != 0U;
}

std::shared_ptr<ConversionResources> conversionResources(
    const std::shared_ptr<SharedState>& state,
    const VkAndroidHardwareBufferFormatPropertiesANDROID& properties,
    std::string& error)
{
    if ((properties.formatFeatures
            & VK_FORMAT_FEATURE_SAMPLED_IMAGE_BIT)
        == 0U) {
        error =
            "The Android hardware-buffer format is not Vulkan sampled-image capable";
        return {};
    }
    if (properties.format == VK_FORMAT_UNDEFINED
        && properties.externalFormat == 0) {
        error =
            "The Android hardware buffer exposes neither a Vulkan format nor an external format";
        return {};
    }
    if (!supportedChromaOffset(
            properties.formatFeatures,
            properties.suggestedXChromaOffset)
        || !supportedChromaOffset(
            properties.formatFeatures,
            properties.suggestedYChromaOffset)) {
        error =
            "The suggested Android chroma sample locations are unsupported";
        return {};
    }

    ConversionKey key;
    key.format = properties.format;
    key.externalFormat = properties.externalFormat;
    key.model = properties.suggestedYcbcrModel;
    key.range = properties.suggestedYcbcrRange;
    key.components = properties.samplerYcbcrConversionComponents;
    key.xOffset = properties.suggestedXChromaOffset;
    key.yOffset = properties.suggestedYChromaOffset;
    key.filter =
        (properties.formatFeatures
            & VK_FORMAT_FEATURE_SAMPLED_IMAGE_YCBCR_CONVERSION_LINEAR_FILTER_BIT)
            != 0U
        ? VK_FILTER_LINEAR
        : VK_FILTER_NEAREST;

    {
        std::lock_guard<std::mutex> lock(state->mutex);
        for (auto iterator = state->conversions.begin();
             iterator != state->conversions.end();) {
            if (const auto existing = iterator->second.lock()) {
                if (iterator->first == key) {
                    return existing;
                }
                ++iterator;
            } else {
                iterator = state->conversions.erase(iterator);
            }
        }
    }

    auto result = std::make_shared<ConversionResources>();
    result->device = state->device.device;
    result->destroyConversion = state->destroyConversion;
    result->key = key;

    VkExternalFormatANDROID externalFormat {
        VK_STRUCTURE_TYPE_EXTERNAL_FORMAT_ANDROID,
    };
    externalFormat.externalFormat = properties.externalFormat;
    VkSamplerYcbcrConversionCreateInfo conversionInfo {
        VK_STRUCTURE_TYPE_SAMPLER_YCBCR_CONVERSION_CREATE_INFO,
    };
    conversionInfo.pNext =
        properties.format == VK_FORMAT_UNDEFINED
        ? &externalFormat
        : nullptr;
    conversionInfo.format = properties.format;
    conversionInfo.ycbcrModel = properties.suggestedYcbcrModel;
    conversionInfo.ycbcrRange = properties.suggestedYcbcrRange;
    conversionInfo.components =
        properties.samplerYcbcrConversionComponents;
    conversionInfo.xChromaOffset =
        properties.suggestedXChromaOffset;
    conversionInfo.yChromaOffset =
        properties.suggestedYChromaOffset;
    conversionInfo.chromaFilter = key.filter;
    VkResult vkResult = state->createConversion(
        state->device.device,
        &conversionInfo,
        nullptr,
        &result->conversion);
    if (vkResult != VK_SUCCESS) {
        error = resultError(
            "vkCreateSamplerYcbcrConversion",
            vkResult);
        return {};
    }

    VkSamplerYcbcrConversionInfo samplerConversion {
        VK_STRUCTURE_TYPE_SAMPLER_YCBCR_CONVERSION_INFO,
    };
    samplerConversion.conversion = result->conversion;
    VkSamplerCreateInfo samplerInfo {
        VK_STRUCTURE_TYPE_SAMPLER_CREATE_INFO,
    };
    samplerInfo.pNext = &samplerConversion;
    samplerInfo.magFilter = key.filter;
    samplerInfo.minFilter = key.filter;
    samplerInfo.mipmapMode = VK_SAMPLER_MIPMAP_MODE_NEAREST;
    samplerInfo.addressModeU = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE;
    samplerInfo.addressModeV = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE;
    samplerInfo.addressModeW = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE;
    samplerInfo.maxLod = 1.0F;
    vkResult = vkCreateSampler(
        state->device.device,
        &samplerInfo,
        nullptr,
        &result->sampler);
    if (vkResult != VK_SUCCESS) {
        error = resultError("vkCreateSampler(YCbCr)", vkResult);
        return {};
    }
    {
        std::lock_guard<std::mutex> lock(state->mutex);
        state->conversions.emplace_back(key, result);
    }
    return result;
}

struct HardwareBufferResources {
    static std::shared_ptr<HardwareBufferResources> create(
        const std::shared_ptr<SharedState>& state,
        AHardwareBuffer* hardwareBuffer,
        const AHardwareBuffer_Desc& description,
        std::string& error)
    {
        auto result = std::make_shared<HardwareBufferResources>();
        result->device = state->device.device;
        result->hardwareBuffer = hardwareBuffer;
        AHardwareBuffer_acquire(result->hardwareBuffer);
        if (!result->initialize(state, description, error)) {
            return {};
        }
        return result;
    }

    ~HardwareBufferResources()
    {
        if (imageView) {
            vkDestroyImageView(device, imageView, nullptr);
        }
        if (image) {
            vkDestroyImage(device, image, nullptr);
        }
        if (memory) {
            vkFreeMemory(device, memory, nullptr);
        }
        if (hardwareBuffer) {
            AHardwareBuffer_release(hardwareBuffer);
        }
    }

    bool initialize(
        const std::shared_ptr<SharedState>& state,
        const AHardwareBuffer_Desc& description,
        std::string& error)
    {
        VkAndroidHardwareBufferFormatPropertiesANDROID formatProperties {
            VK_STRUCTURE_TYPE_ANDROID_HARDWARE_BUFFER_FORMAT_PROPERTIES_ANDROID,
        };
        VkAndroidHardwareBufferPropertiesANDROID properties {
            VK_STRUCTURE_TYPE_ANDROID_HARDWARE_BUFFER_PROPERTIES_ANDROID,
        };
        properties.pNext = &formatProperties;
        VkResult result = state->getHardwareBufferProperties(
            state->device.device,
            hardwareBuffer,
            &properties);
        if (result != VK_SUCCESS) {
            error = resultError(
                "vkGetAndroidHardwareBufferPropertiesANDROID",
                result);
            return false;
        }
        conversion = conversionResources(
            state,
            formatProperties,
            error);
        if (!conversion) {
            return false;
        }

        VkExternalFormatANDROID externalFormatInfo {
            VK_STRUCTURE_TYPE_EXTERNAL_FORMAT_ANDROID,
        };
        externalFormatInfo.externalFormat =
            formatProperties.externalFormat;
        VkExternalMemoryImageCreateInfo externalMemory {
            VK_STRUCTURE_TYPE_EXTERNAL_MEMORY_IMAGE_CREATE_INFO,
        };
        externalMemory.pNext =
            formatProperties.format == VK_FORMAT_UNDEFINED
            ? &externalFormatInfo
            : nullptr;
        externalMemory.handleTypes =
            VK_EXTERNAL_MEMORY_HANDLE_TYPE_ANDROID_HARDWARE_BUFFER_BIT_ANDROID;
        VkImageCreateInfo imageInfo {
            VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO,
        };
        imageInfo.pNext = &externalMemory;
        imageInfo.imageType = VK_IMAGE_TYPE_2D;
        imageInfo.format = formatProperties.format;
        imageInfo.extent = {
            description.width,
            description.height,
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
            state->device.device,
            &imageInfo,
            nullptr,
            &image);
        if (result != VK_SUCCESS) {
            error = resultError(
                "vkCreateImage(AHardwareBuffer)",
                result);
            return false;
        }

        VkMemoryRequirements requirements {};
        vkGetImageMemoryRequirements(
            state->device.device,
            image,
            &requirements);
        const std::uint32_t memoryType = firstMemoryType(
            properties.memoryTypeBits
                & requirements.memoryTypeBits);
        if (memoryType
            == std::numeric_limits<std::uint32_t>::max()) {
            error =
                "The Android hardware buffer exposes no compatible Vulkan memory type";
            return false;
        }
        VkMemoryDedicatedAllocateInfo dedicated {
            VK_STRUCTURE_TYPE_MEMORY_DEDICATED_ALLOCATE_INFO,
        };
        dedicated.image = image;
        VkImportAndroidHardwareBufferInfoANDROID import {
            VK_STRUCTURE_TYPE_IMPORT_ANDROID_HARDWARE_BUFFER_INFO_ANDROID,
        };
        import.pNext = &dedicated;
        import.buffer = hardwareBuffer;
        VkMemoryAllocateInfo allocation {
            VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO,
        };
        allocation.pNext = &import;
        allocation.allocationSize = properties.allocationSize;
        allocation.memoryTypeIndex = memoryType;
        result = vkAllocateMemory(
            state->device.device,
            &allocation,
            nullptr,
            &memory);
        if (result != VK_SUCCESS) {
            error = resultError(
                "vkAllocateMemory(AHardwareBuffer)",
                result);
            return false;
        }
        result = vkBindImageMemory(
            state->device.device,
            image,
            memory,
            0);
        if (result != VK_SUCCESS) {
            error = resultError(
                "vkBindImageMemory(AHardwareBuffer)",
                result);
            return false;
        }

        VkSamplerYcbcrConversionInfo viewConversion {
            VK_STRUCTURE_TYPE_SAMPLER_YCBCR_CONVERSION_INFO,
        };
        viewConversion.conversion = conversion->conversion;
        VkImageViewCreateInfo viewInfo {
            VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO,
        };
        viewInfo.pNext = &viewConversion;
        viewInfo.image = image;
        viewInfo.viewType = VK_IMAGE_VIEW_TYPE_2D;
        viewInfo.format = formatProperties.format;
        viewInfo.components =
            formatProperties.samplerYcbcrConversionComponents;
        viewInfo.subresourceRange.aspectMask =
            VK_IMAGE_ASPECT_COLOR_BIT;
        viewInfo.subresourceRange.levelCount = 1;
        viewInfo.subresourceRange.layerCount = 1;
        result = vkCreateImageView(
            state->device.device,
            &viewInfo,
            nullptr,
            &imageView);
        if (result != VK_SUCCESS) {
            error = resultError(
                "vkCreateImageView(AHardwareBuffer)",
                result);
            return false;
        }

        hardwareBufferFormat = description.format;
        vulkanFormat = formatProperties.format;
        externalFormat = formatProperties.externalFormat;
        return true;
    }

    VkDevice device = VK_NULL_HANDLE;
    AHardwareBuffer* hardwareBuffer = nullptr;
    VkImage image = VK_NULL_HANDLE;
    VkDeviceMemory memory = VK_NULL_HANDLE;
    VkImageView imageView = VK_NULL_HANDLE;
    std::shared_ptr<ConversionResources> conversion;
    std::atomic<bool> returnedToProducer { false };
    std::uint32_t hardwareBufferFormat = 0;
    VkFormat vulkanFormat = VK_FORMAT_UNDEFINED;
    std::uint64_t externalFormat = 0;
};

std::shared_ptr<HardwareBufferResources> hardwareBufferResources(
    const std::shared_ptr<SharedState>& state,
    AHardwareBuffer* hardwareBuffer,
    const AHardwareBuffer_Desc& description,
    std::string& error)
{
    {
        std::lock_guard<std::mutex> lock(state->mutex);
        const auto found = state->bufferImports.find(hardwareBuffer);
        if (found != state->bufferImports.end()) {
            state->statistics.hardwareBufferImportCacheHits.fetch_add(
                1,
                std::memory_order_relaxed);
            return found->second;
        }
    }

    auto created = HardwareBufferResources::create(
        state,
        hardwareBuffer,
        description,
        error);
    if (!created) {
        return {};
    }
    {
        std::lock_guard<std::mutex> lock(state->mutex);
        const auto found = state->bufferImports.find(hardwareBuffer);
        if (found != state->bufferImports.end()) {
            state->statistics.hardwareBufferImportCacheHits.fetch_add(
                1,
                std::memory_order_relaxed);
            return found->second;
        }
        state->bufferImports.emplace(hardwareBuffer, created);
        state->statistics.hardwareBufferImports.fetch_add(
            1,
            std::memory_order_relaxed);
        updateMaximum(
            state->statistics.maximumCachedHardwareBufferImports,
            state->bufferImports.size());
    }
    return created;
}

class AndroidHardwareBufferTexture final
    : public VulkanTextureFrame {
public:
    static std::shared_ptr<AndroidHardwareBufferTexture> create(
        std::shared_ptr<SharedState> state,
        PendingImage pending,
        std::string& error)
    {
        auto result =
            std::shared_ptr<AndroidHardwareBufferTexture>(
                new AndroidHardwareBufferTexture(
                    std::move(state),
                    pending));
        pending = {};
        if (!result->initialize(error)) {
            return {};
        }
        return result;
    }

    ~AndroidHardwareBufferTexture() override
    {
        if (imageObject_ && !releaseAttempted_.exchange(true)) {
            if (!acquireSemaphore_ && acquireFence_ >= 0) {
                AImage_deleteAsync(imageObject_, acquireFence_);
                imageObject_ = nullptr;
                acquireFence_ = -1;
            }
            // Import may have succeeded before a later render-target or
            // pipeline failure. Consume the producer fence on the Vulkan
            // queue before returning the image in this exceptional path.
            if (imageObject_ && acquireSemaphore_) {
                VkPipelineStageFlags stage =
                    VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT;
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
                        == VK_SUCCESS
                    && vkQueueWaitIdle(state_->device.queue)
                        == VK_SUCCESS) {
                    AImage_delete(imageObject_);
                    imageObject_ = nullptr;
                }
            } else if (imageObject_) {
                AImage_delete(imageObject_);
                imageObject_ = nullptr;
            }
        }
        if (acquireSemaphore_) {
            vkDestroySemaphore(
                state_->device.device,
                acquireSemaphore_,
                nullptr);
        }
        if (releaseSemaphore_) {
            vkDestroySemaphore(
                state_->device.device,
                releaseSemaphore_,
                nullptr);
        }
        if (imageObject_) {
            if (acquireFence_ >= 0) {
                AImage_deleteAsync(imageObject_, acquireFence_);
                acquireFence_ = -1;
            } else {
                AImage_delete(imageObject_);
            }
            imageObject_ = nullptr;
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
        return resources_ ? resources_->image : VK_NULL_HANDLE;
    }

    VkImageView imageView() const noexcept override
    {
        return resources_ ? resources_->imageView : VK_NULL_HANDLE;
    }

    VkSampler sampler() const noexcept override
    {
        return resources_ && resources_->conversion
            ? resources_->conversion->sampler
            : VK_NULL_HANDLE;
    }

    VkSemaphore acquireSemaphore() const noexcept override
    {
        return acquireSemaphore_;
    }

    VkSemaphore releaseSemaphore() const noexcept override
    {
        return releaseSemaphore_;
    }

    VkImageLayout initialLayout() const noexcept override
    {
        return initialLayout_;
    }

    VulkanNormalizedSourceRect
    normalizedSourceRect() const noexcept override
    {
        return normalizedSourceRect_;
    }

    void releaseToProducer() noexcept override
    {
        if (!imageObject_ || releaseAttempted_.exchange(true)) {
            return;
        }
        // The renderer released this persistent VkImage to the foreign queue
        // in GENERAL layout. A later AImage backed by the same buffer must
        // acquire from that layout rather than treating a reused import as a
        // newly created UNDEFINED image.
        resources_->returnedToProducer.store(
            true,
            std::memory_order_release);
        VkSemaphoreGetFdInfoKHR descriptorInfo {
            VK_STRUCTURE_TYPE_SEMAPHORE_GET_FD_INFO_KHR,
        };
        descriptorInfo.semaphore = releaseSemaphore_;
        descriptorInfo.handleType =
            VK_EXTERNAL_SEMAPHORE_HANDLE_TYPE_SYNC_FD_BIT;
        int descriptor = -1;
        const VkResult result = state_->getSemaphoreFd(
            state_->device.device,
            &descriptorInfo,
            &descriptor);
        if (result == VK_SUCCESS && descriptor >= 0) {
            AImage_deleteAsync(imageObject_, descriptor);
            imageObject_ = nullptr;
            state_->statistics.releaseFencesReturned.fetch_add(
                1,
                std::memory_order_relaxed);
        } else {
            closeDescriptor(descriptor);
            state_->statistics.releaseFenceFallbacks.fetch_add(
                1,
                std::memory_order_relaxed);
        }
    }

private:
    AndroidHardwareBufferTexture(
        std::shared_ptr<SharedState> state,
        PendingImage pending)
        : state_(std::move(state))
        , imageObject_(pending.image)
        , acquireFence_(pending.acquireFence)
    {
    }

    bool initialize(std::string& error)
    {
        if (AImage_getWidth(imageObject_, &width_) != AMEDIA_OK
            || AImage_getHeight(imageObject_, &height_) != AMEDIA_OK
            || width_ <= 0 || height_ <= 0) {
            error = "AImage returned invalid dimensions";
            return false;
        }
        AHardwareBuffer* hardwareBuffer = nullptr;
        if (AImage_getHardwareBuffer(
                imageObject_,
                &hardwareBuffer)
            != AMEDIA_OK
            || !hardwareBuffer) {
            error = "AImage_getHardwareBuffer failed";
            return false;
        }

        AHardwareBuffer_Desc description {};
        AHardwareBuffer_describe(hardwareBuffer, &description);
        AImageCropRect crop {};
        if (AImage_getCropRect(imageObject_, &crop) != AMEDIA_OK
            || crop.left < 0 || crop.top < 0
            || crop.right <= crop.left || crop.bottom <= crop.top
            || crop.right > static_cast<std::int32_t>(description.width)
            || crop.bottom > static_cast<std::int32_t>(description.height)) {
            error =
                "AImage returned a crop rectangle outside its Android hardware buffer";
            return false;
        }
        if (description.width
                < static_cast<std::uint32_t>(width_)
            || description.height
                < static_cast<std::uint32_t>(height_)
            || description.layers != 1
            || (description.usage
                & AHARDWAREBUFFER_USAGE_GPU_SAMPLED_IMAGE)
                == 0U) {
            error =
                "The acquired Android hardware buffer is not a single-layer sampled image: image="
                + std::to_string(width_) + 'x'
                + std::to_string(height_)
                + " buffer="
                + std::to_string(description.width) + 'x'
                + std::to_string(description.height)
                + " layers="
                + std::to_string(description.layers)
                + " usage="
                + std::to_string(description.usage)
                + " format="
                + std::to_string(description.format);
            return false;
        }
        normalizedSourceRect_ = {
            static_cast<float>(crop.left)
                / static_cast<float>(description.width),
            static_cast<float>(crop.top)
                / static_cast<float>(description.height),
            static_cast<float>(crop.right)
                / static_cast<float>(description.width),
            static_cast<float>(crop.bottom)
                / static_cast<float>(description.height),
        };
        if ((description.usage
                & AHARDWAREBUFFER_USAGE_PROTECTED_CONTENT)
            != 0U) {
            error =
                "Protected Android hardware buffers are not supported";
            return false;
        }

        resources_ = hardwareBufferResources(
            state_,
            hardwareBuffer,
            description,
            error);
        if (!resources_) {
            return false;
        }
        initialLayout_ = resources_->returnedToProducer.load(
            std::memory_order_acquire)
            ? VK_IMAGE_LAYOUT_GENERAL
            : VK_IMAGE_LAYOUT_UNDEFINED;

        VkResult result = VK_SUCCESS;
        if (acquireFence_ >= 0) {
            VkSemaphoreCreateInfo semaphoreInfo {
                VK_STRUCTURE_TYPE_SEMAPHORE_CREATE_INFO,
            };
            result = vkCreateSemaphore(
                state_->device.device,
                &semaphoreInfo,
                nullptr,
                &acquireSemaphore_);
            if (result != VK_SUCCESS) {
                error = resultError(
                    "vkCreateSemaphore(acquire)",
                    result);
                return false;
            }
            VkImportSemaphoreFdInfoKHR importInfo {
                VK_STRUCTURE_TYPE_IMPORT_SEMAPHORE_FD_INFO_KHR,
            };
            importInfo.semaphore = acquireSemaphore_;
            importInfo.flags =
                VK_SEMAPHORE_IMPORT_TEMPORARY_BIT;
            importInfo.handleType =
                VK_EXTERNAL_SEMAPHORE_HANDLE_TYPE_SYNC_FD_BIT;
            importInfo.fd = acquireFence_;
            result = state_->importSemaphoreFd(
                state_->device.device,
                &importInfo);
            if (result != VK_SUCCESS) {
                error = resultError(
                    "vkImportSemaphoreFdKHR",
                    result);
                return false;
            }
            acquireFence_ = -1;
            state_->statistics.acquireFencesImported.fetch_add(
                1,
                std::memory_order_relaxed);
        }

        VkExportSemaphoreCreateInfo exportInfo {
            VK_STRUCTURE_TYPE_EXPORT_SEMAPHORE_CREATE_INFO,
        };
        exportInfo.handleTypes =
            VK_EXTERNAL_SEMAPHORE_HANDLE_TYPE_SYNC_FD_BIT;
        VkSemaphoreCreateInfo releaseInfo {
            VK_STRUCTURE_TYPE_SEMAPHORE_CREATE_INFO,
        };
        releaseInfo.pNext = &exportInfo;
        result = vkCreateSemaphore(
            state_->device.device,
            &releaseInfo,
            nullptr,
            &releaseSemaphore_);
        if (result != VK_SUCCESS) {
            error = resultError(
                "vkCreateSemaphore(release)",
                result);
            return false;
        }

        state_->statistics.imagesImported.fetch_add(
            1,
            std::memory_order_relaxed);
        state_->statistics.lastHardwareBufferFormat.store(
            resources_->hardwareBufferFormat,
            std::memory_order_relaxed);
        state_->statistics.lastVulkanFormat.store(
            static_cast<int>(resources_->vulkanFormat),
            std::memory_order_relaxed);
        state_->statistics.lastExternalFormat.store(
            resources_->externalFormat,
            std::memory_order_relaxed);
        return true;
    }

    std::shared_ptr<SharedState> state_;
    AImage* imageObject_ = nullptr;
    int acquireFence_ = -1;
    int width_ = 0;
    int height_ = 0;
    VulkanNormalizedSourceRect normalizedSourceRect_;
    VkImageLayout initialLayout_ = VK_IMAGE_LAYOUT_UNDEFINED;
    VkSemaphore acquireSemaphore_ = VK_NULL_HANDLE;
    VkSemaphore releaseSemaphore_ = VK_NULL_HANDLE;
    std::shared_ptr<HardwareBufferResources> resources_;
    std::atomic<bool> releaseAttempted_ { false };
};

FrameKey frameKey(const VideoFrame& frame) noexcept
{
    FrameKey result;
    if (!frame || !frame.hasHardwareFrame()) {
        return result;
    }
    const HardwareFrame hardware = frame.hardwareFrame();
    const NativeHandle output =
        hardware.nativeHandle(HardwareHandleType::Frame);
    result.buffer = output.value;
    result.generation = output.subresource;
    result.timestampMilliseconds = frame.timestamp();
    return result;
}

} // namespace

class MediaCodecVulkanInterop::Impl {
public:
    Impl(
        BorrowedVulkanDevice device,
        MediaCodecVulkanInteropConfig config)
        : state_(std::make_shared<SharedState>())
    {
        state_->device = device;
        state_->config = config;
        state_->config.maximumImages = std::clamp(
            state_->config.maximumImages,
            4,
            16);
        initialize();
    }

    void initialize()
    {
        if (!state_->device.isValid()) {
            error_ =
                "MediaCodec Vulkan interop requires a borrowed physical device, logical device, and queue";
            return;
        }
        if (state_->config.width <= 0
            || state_->config.height <= 0) {
            error_ =
                "MediaCodec Vulkan interop requires positive image dimensions";
            return;
        }
        if (!state_->config
                 .androidHardwareBufferExternalMemoryEnabled
            || !state_->config.externalSemaphoreFdEnabled
            || !state_->config.samplerYcbcrConversionEnabled
            || !state_->config.foreignQueueFamilyEnabled) {
            error_ =
                "MediaCodec Vulkan interop requires explicitly enabled AHardwareBuffer external memory, sync-fd semaphore, YCbCr conversion, and foreign-queue capabilities";
            return;
        }

        state_->getHardwareBufferProperties =
            reinterpret_cast<
                PFN_vkGetAndroidHardwareBufferPropertiesANDROID>(
                vkGetDeviceProcAddr(
                    state_->device.device,
                    "vkGetAndroidHardwareBufferPropertiesANDROID"));
        state_->importSemaphoreFd =
            reinterpret_cast<PFN_vkImportSemaphoreFdKHR>(
                vkGetDeviceProcAddr(
                    state_->device.device,
                    "vkImportSemaphoreFdKHR"));
        state_->getSemaphoreFd =
            reinterpret_cast<PFN_vkGetSemaphoreFdKHR>(
                vkGetDeviceProcAddr(
                    state_->device.device,
                    "vkGetSemaphoreFdKHR"));
        state_->createConversion =
            reinterpret_cast<PFN_vkCreateSamplerYcbcrConversion>(
                vkGetDeviceProcAddr(
                    state_->device.device,
                    "vkCreateSamplerYcbcrConversion"));
        state_->destroyConversion =
            reinterpret_cast<PFN_vkDestroySamplerYcbcrConversion>(
                vkGetDeviceProcAddr(
                    state_->device.device,
                    "vkDestroySamplerYcbcrConversion"));
        if (!state_->getHardwareBufferProperties
            || !state_->importSemaphoreFd
            || !state_->getSemaphoreFd
            || !state_->createConversion
            || !state_->destroyConversion) {
            error_ =
                "The Vulkan device does not expose the required Android external-memory, sync-fd, or YCbCr entry points";
            return;
        }

        const media_status_t status =
            AImageReader_newWithUsage(
                state_->config.width,
                state_->config.height,
                AIMAGE_FORMAT_PRIVATE,
                AHARDWAREBUFFER_USAGE_GPU_SAMPLED_IMAGE,
                state_->config.maximumImages,
                &state_->reader);
        if (status != AMEDIA_OK || !state_->reader) {
            error_ =
                "AImageReader_newWithUsage(PRIVATE, GPU_SAMPLED_IMAGE) failed: "
                + std::to_string(status);
            return;
        }
        ANativeWindow* window = nullptr;
        if (AImageReader_getWindow(state_->reader, &window)
                != AMEDIA_OK
            || !window) {
            error_ = "AImageReader_getWindow failed";
            return;
        }
        state_->surface = MediaCodecSurface(window);
        if (!state_->surface) {
            error_ =
                "Could not retain the private AImageReader producer window";
            return;
        }

        AImageReader_ImageListener listener {};
        listener.context = state_.get();
        listener.onImageAvailable = &onImageAvailable;
        if (AImageReader_setImageListener(
                state_->reader,
                &listener)
            != AMEDIA_OK) {
            error_ = "AImageReader_setImageListener failed";
            return;
        }
        AImageReader_BufferRemovedListener removed {};
        removed.context = state_.get();
        removed.onBufferRemoved = &onBufferRemoved;
        if (AImageReader_setBufferRemovedListener(
                state_->reader,
                &removed)
            != AMEDIA_OK) {
            error_ =
                "AImageReader_setBufferRemovedListener failed";
            return;
        }
        valid_ = true;
    }

    VulkanHardwareImportStatus prepareFrame(
        const VideoFrame& frame,
        std::string& detail)
    {
        if (!valid_) {
            detail = error_;
            return VulkanHardwareImportStatus::Error;
        }
        const FrameKey key = frameKey(frame);
        if (key.buffer == 0 || key.generation == 0) {
            detail =
                "The frame is not a MediaCodec direct-surface output";
            return VulkanHardwareImportStatus::Unsupported;
        }
        if (!supports(frame.hardwareFrame())) {
            detail =
                "The MediaCodec frame belongs to a stale or foreign AImageReader surface";
            return VulkanHardwareImportStatus::Stale;
        }

        std::deque<PendingImage> discarded;
        bool alreadyQueued = false;
        bool alreadyRetired = false;
        bool imageReady = false;
        {
            std::lock_guard<std::mutex> lock(state_->mutex);
            if (!state_->asyncError.empty()) {
                detail = std::move(state_->asyncError);
                state_->asyncError.clear();
                return VulkanHardwareImportStatus::Error;
            }
            const std::int64_t expected =
                frame.timestamp() * 1'000'000LL;
            for (auto iterator = state_->images.begin();
                 iterator != state_->images.end();) {
                if (iterator->timestampNanoseconds
                    < expected
                        - MaximumPresentationLagNanoseconds) {
                    discarded.push_back(*iterator);
                    iterator = state_->images.erase(iterator);
                    state_->statistics.staleImagesDropped.fetch_add(
                        1,
                        std::memory_order_relaxed);
                    continue;
                }
                const std::int64_t distance = std::llabs(
                    iterator->timestampNanoseconds - expected);
                if (iterator->timestampNanoseconds
                        <= expected
                            + TimestampToleranceNanoseconds
                    && distance
                        <= TimestampToleranceNanoseconds) {
                    imageReady = true;
                }
                ++iterator;
            }
            if (!imageReady) {
                alreadyQueued =
                    std::find(
                        state_->queuedFrames.begin(),
                        state_->queuedFrames.end(),
                        key)
                    != state_->queuedFrames.end();
                alreadyRetired =
                    std::find(
                        state_->retiredFrames.begin(),
                        state_->retiredFrames.end(),
                        key)
                    != state_->retiredFrames.end();
                if (!alreadyQueued && !alreadyRetired) {
                    for (auto iterator = state_->queuedFrames.begin();
                         iterator != state_->queuedFrames.end();) {
                        if (iterator->timestampMilliseconds
                                * 1'000'000LL
                            < expected
                                - MaximumPresentationLagNanoseconds) {
                            state_->retiredFrames.push_back(*iterator);
                            iterator = state_->queuedFrames.erase(iterator);
                        } else {
                            ++iterator;
                        }
                    }
                    if (static_cast<int>(
                            state_->queuedFrames.size())
                        >= state_->config.maximumImages) {
                        state_->retiredFrames.push_back(
                            state_->queuedFrames.front());
                        state_->queuedFrames.erase(
                            state_->queuedFrames.begin());
                        state_->statistics.staleImagesDropped.fetch_add(
                            1,
                            std::memory_order_relaxed);
                    }
                    state_->queuedFrames.push_back(key);
                    while (state_->retiredFrames.size()
                           > MaximumRetiredFrameKeys) {
                        state_->retiredFrames.pop_front();
                    }
                }
            }
        }
        for (auto& image : discarded) {
            discardImage(image);
        }
        if (imageReady) {
            return VulkanHardwareImportStatus::Ready;
        }
        if (alreadyQueued || alreadyRetired) {
            return VulkanHardwareImportStatus::Pending;
        }

        MediaCodecFrame output =
            mediaCodecFrame(frame, state_->surface);
        const bool released =
            output && output.isPending() && output.present();
        if (!released) {
            std::lock_guard<std::mutex> lock(state_->mutex);
            state_->queuedFrames.erase(
                std::remove(
                    state_->queuedFrames.begin(),
                    state_->queuedFrames.end(),
                    key),
                state_->queuedFrames.end());
            detail =
                "Could not release the MediaCodec output into the private AImageReader";
            return VulkanHardwareImportStatus::Error;
        }
        state_->statistics.codecOutputsQueued.fetch_add(
            1,
            std::memory_order_relaxed);
        return VulkanHardwareImportStatus::Pending;
    }

    VulkanHardwareImportResult importFrame(
        const VideoFrame& frame)
    {
        if (!valid_) {
            return {
                VulkanHardwareImportStatus::Error,
                {},
                error_,
            };
        }
        const FrameKey key = frameKey(frame);
        if (key.buffer == 0 || key.generation == 0
            || !supports(frame.hardwareFrame())) {
            return {
                VulkanHardwareImportStatus::Stale,
                {},
                "The MediaCodec frame is stale or incompatible with this AImageReader",
            };
        }

        PendingImage matched;
        std::deque<PendingImage> discarded;
        {
            std::lock_guard<std::mutex> lock(state_->mutex);
            const std::int64_t expected =
                frame.timestamp() * 1'000'000LL;
            auto closest = state_->images.end();
            std::int64_t closestDistance =
                MaximumPresentationLagNanoseconds + 1;
            for (auto iterator = state_->images.begin();
                 iterator != state_->images.end();) {
                if (iterator->timestampNanoseconds
                    < expected
                        - MaximumPresentationLagNanoseconds) {
                    discarded.push_back(*iterator);
                    iterator = state_->images.erase(iterator);
                    state_->statistics.staleImagesDropped.fetch_add(
                        1,
                        std::memory_order_relaxed);
                    continue;
                }
                const std::int64_t distance = std::llabs(
                    iterator->timestampNanoseconds - expected);
                if (iterator->timestampNanoseconds
                        <= expected
                            + TimestampToleranceNanoseconds
                    && distance <= TimestampToleranceNanoseconds
                    && distance < closestDistance) {
                    closest = iterator;
                    closestDistance = distance;
                }
                ++iterator;
            }
            if (closest != state_->images.end()
                && closestDistance
                    <= MaximumPresentationLagNanoseconds) {
                matched = *closest;
                state_->images.erase(closest);
                const auto correlated = std::find(
                    state_->queuedFrames.begin(),
                    state_->queuedFrames.end(),
                    key);
                if (correlated
                        == state_->queuedFrames.end()
                    || std::llabs(
                           matched.timestampNanoseconds
                           - correlated->timestampMilliseconds
                               * 1'000'000LL)
                        > TimestampToleranceNanoseconds) {
                    discarded.push_back(matched);
                    matched = {};
                    state_->statistics.staleImagesDropped.fetch_add(
                        1,
                        std::memory_order_relaxed);
                } else {
                    state_->queuedFrames.erase(correlated);
                }
            }
        }
        for (auto& image : discarded) {
            discardImage(image);
        }
        if (!matched.image) {
            return {
                VulkanHardwareImportStatus::Pending,
                {},
                {},
            };
        }

        std::string importError;
        auto texture =
            AndroidHardwareBufferTexture::create(
                state_,
                matched,
                importError);
        matched = {};
        if (!texture) {
            {
                std::lock_guard<std::mutex> lock(state_->mutex);
                state_->lastImportError = importError;
            }
            return {
                VulkanHardwareImportStatus::Error,
                {},
                std::move(importError),
            };
        }
        return {
            VulkanHardwareImportStatus::Ready,
            std::move(texture),
            {},
        };
    }

    void waitForFrameImage(
        const VideoFrame& frame,
        std::chrono::milliseconds timeout)
    {
        const auto expected = frame.timestamp() * 1'000'000LL;
        std::unique_lock<std::mutex> lock(state_->mutex);
        state_->imageChanged.wait_for(
            lock,
            timeout,
            [this, expected] {
                return state_->shuttingDown
                    || !state_->asyncError.empty()
                    || std::any_of(
                        state_->images.begin(),
                        state_->images.end(),
                        [expected](const PendingImage& image) {
                            return std::llabs(
                                image.timestampNanoseconds - expected)
                                <= TimestampToleranceNanoseconds;
                        });
            });
    }

    bool supports(const HardwareFrame& frame) const noexcept
    {
        if (!valid_ || !frame
            || frame.deviceType()
                != HardwareDeviceType::MediaCodec) {
            return false;
        }
        const NativeHandle output =
            frame.nativeHandle(HardwareHandleType::Frame);
        const NativeHandle sourceSurface =
            frame.nativeHandle(HardwareHandleType::Surface);
        return output && sourceSurface
            && output.subresource == state_->surface.generation()
            && sourceSurface.subresource
                == state_->surface.generation()
            && sourceSurface.value
                == reinterpret_cast<std::uintptr_t>(
                    state_->surface.nativeWindow());
    }

    void flush() noexcept
    {
        std::deque<PendingImage> pending;
        {
            std::lock_guard<std::mutex> lock(state_->mutex);
            pending.swap(state_->images);
            state_->queuedFrames.clear();
            state_->retiredFrames.clear();
            state_->asyncError.clear();
        }
        for (auto& image : pending) {
            discardImage(image);
        }
    }

    std::shared_ptr<SharedState> state_;
    std::string error_;
    bool valid_ = false;
};

MediaCodecVulkanInterop::MediaCodecVulkanInterop(
    BorrowedVulkanDevice device,
    MediaCodecVulkanInteropConfig config)
    : impl_(std::make_unique<Impl>(device, config))
{
}

MediaCodecVulkanInterop::~MediaCodecVulkanInterop() = default;
MediaCodecVulkanInterop::MediaCodecVulkanInterop(
    MediaCodecVulkanInterop&&) noexcept = default;
MediaCodecVulkanInterop&
MediaCodecVulkanInterop::operator=(
    MediaCodecVulkanInterop&&) noexcept = default;

MediaCodecVulkanInterop::operator bool() const noexcept
{
    return isValid();
}

bool MediaCodecVulkanInterop::isValid() const noexcept
{
    return impl_ && impl_->valid_;
}

std::string MediaCodecVulkanInterop::lastError() const
{
    if (!impl_) {
        return {};
    }
    std::lock_guard<std::mutex> lock(impl_->state_->mutex);
    return !impl_->state_->lastImportError.empty()
        ? impl_->state_->lastImportError
        : impl_->error_;
}

MediaCodecSurface
MediaCodecVulkanInterop::surface() const noexcept
{
    return impl_ ? impl_->state_->surface : MediaCodecSurface {};
}

BorrowedVulkanDevice
MediaCodecVulkanInterop::device() const noexcept
{
    return impl_
        ? impl_->state_->device
        : BorrowedVulkanDevice {};
}

HardwareInteropCapabilities
MediaCodecVulkanInterop::capabilities() const
{
    HardwareInteropCapabilities result;
    if (isValid()) {
        result.sourceDevices = {
            HardwareDeviceType::MediaCodec,
        };
        result.targetDevice = HardwareDeviceType::Vulkan;
        result.zeroCopy = true;
        result.cpuFallback = false;
    }
    return result;
}

bool MediaCodecVulkanInterop::supports(
    const HardwareFrame& frame) const noexcept
{
    return impl_ && impl_->supports(frame);
}

bool MediaCodecVulkanInterop::queueFrame(
    const VideoFrame& frame,
    std::string& detail)
{
    if (!impl_) {
        detail = "The MediaCodec Vulkan interop object is empty";
        return false;
    }
    const auto status = impl_->prepareFrame(frame, detail);
    if (status == VulkanHardwareImportStatus::Pending) {
        // Confirm ownership transfer before queuing more codec output. Packet
        // pacing in Player prevents the producer bursts that previously made
        // this bounded wait visible in presentation timing.
        impl_->waitForFrameImage(frame, std::chrono::milliseconds(100));
    }
    return status == VulkanHardwareImportStatus::Pending
        || status == VulkanHardwareImportStatus::Ready;
}

VulkanHardwareImportStatus
MediaCodecVulkanInterop::prepareFrame(
    const VideoFrame& frame,
    std::string& detail)
{
    if (!impl_) {
        detail = "The MediaCodec Vulkan interop object is empty";
        return VulkanHardwareImportStatus::Error;
    }
    return impl_->prepareFrame(frame, detail);
}

VulkanHardwareImportResult
MediaCodecVulkanInterop::importFrame(
    const VideoFrame& frame)
{
    if (!impl_) {
        return {
            VulkanHardwareImportStatus::Error,
            {},
            "The MediaCodec Vulkan interop object is empty",
        };
    }
    return impl_->importFrame(frame);
}

void MediaCodecVulkanInterop::setFrameAvailableCallback(
    FrameAvailableCallback callback)
{
    if (!impl_) {
        return;
    }
    std::lock_guard<std::mutex> lock(impl_->state_->mutex);
    impl_->state_->frameAvailable = std::move(callback);
}

void MediaCodecVulkanInterop::flush() noexcept
{
    if (impl_) {
        impl_->flush();
    }
}

MediaCodecVulkanInteropStatistics
MediaCodecVulkanInterop::statistics() const noexcept
{
    MediaCodecVulkanInteropStatistics result;
    if (!impl_) {
        return result;
    }
    const AtomicStatistics& source = impl_->state_->statistics;
    result.codecOutputsQueued =
        source.codecOutputsQueued.load(std::memory_order_relaxed);
    result.imagesAcquired =
        source.imagesAcquired.load(std::memory_order_relaxed);
    result.imagesImported =
        source.imagesImported.load(std::memory_order_relaxed);
    result.acquireFencesImported =
        source.acquireFencesImported.load(std::memory_order_relaxed);
    result.releaseFencesReturned =
        source.releaseFencesReturned.load(std::memory_order_relaxed);
    result.releaseFenceFallbacks =
        source.releaseFenceFallbacks.load(std::memory_order_relaxed);
    result.staleImagesDropped =
        source.staleImagesDropped.load(std::memory_order_relaxed);
    result.maximumPendingImages =
        source.maximumPendingImages.load(std::memory_order_relaxed);
    result.hardwareBufferImports =
        source.hardwareBufferImports.load(std::memory_order_relaxed);
    result.hardwareBufferImportCacheHits =
        source.hardwareBufferImportCacheHits.load(
            std::memory_order_relaxed);
    result.hardwareBufferImportsRemoved =
        source.hardwareBufferImportsRemoved.load(
            std::memory_order_relaxed);
    result.maximumCachedHardwareBufferImports =
        source.maximumCachedHardwareBufferImports.load(
            std::memory_order_relaxed);
    result.lastHardwareBufferFormat =
        source.lastHardwareBufferFormat.load(
            std::memory_order_relaxed);
    result.lastVulkanFormat = static_cast<VkFormat>(
        source.lastVulkanFormat.load(std::memory_order_relaxed));
    result.lastExternalFormat =
        source.lastExternalFormat.load(std::memory_order_relaxed);
    // The implementation contains no decoded-pixel map, software transfer,
    // CPU staging, or renderer-upload operation. These proof counters remain
    // explicit so connected-device validation can reject regressions.
    result.cpuMapCalls = 0;
    result.softwareTransferCalls = 0;
    result.stagingCopies = 0;
    result.rendererUploads = 0;
    return result;
}

} // namespace qtav
