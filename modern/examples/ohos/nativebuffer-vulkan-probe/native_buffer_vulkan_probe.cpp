// SPDX-License-Identifier: CC0-1.0

#include "native_buffer_vulkan_probe.h"

#include <limits>

namespace ohos_native_buffer_probe {
namespace {

std::uint32_t firstMemoryType(std::uint32_t bits) noexcept
{
    for (std::uint32_t index = 0; index < 32; ++index) {
        if ((bits & (1U << index)) != 0U) {
            return index;
        }
    }
    return std::numeric_limits<std::uint32_t>::max();
}

} // namespace

VkFormat forcedFormat(std::uint64_t externalFormat) noexcept
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

bool probeNativeBuffer(
    VkDevice device,
    OH_NativeBuffer* nativeBuffer,
    const Options& options,
    Log log,
    Consume consume,
    void* user,
    Report* report)
{
    if (!device || !nativeBuffer || !log) {
        return false;
    }

    const auto getProperties = reinterpret_cast<
        PFN_vkGetNativeBufferPropertiesOHOS>(
            vkGetDeviceProcAddr(
                device,
                "vkGetNativeBufferPropertiesOHOS"));
    if (!getProperties) {
        log("vkGetDeviceProcAddr(vkGetNativeBufferPropertiesOHOS)",
            VK_ERROR_EXTENSION_NOT_PRESENT);
        return false;
    }

    OH_NativeBuffer_Config nativeConfig {};
    OH_NativeBuffer_GetConfig(nativeBuffer, &nativeConfig);
    if (nativeConfig.width <= 0 || nativeConfig.height <= 0) {
        log("OH_NativeBuffer_GetConfig", VK_ERROR_INITIALIZATION_FAILED);
        return false;
    }

    VkNativeBufferFormatPropertiesOHOS formatProperties {
        VK_STRUCTURE_TYPE_NATIVE_BUFFER_FORMAT_PROPERTIES_OHOS,
    };
    VkNativeBufferPropertiesOHOS properties {
        VK_STRUCTURE_TYPE_NATIVE_BUFFER_PROPERTIES_OHOS,
    };
    properties.pNext = &formatProperties;
    VkResult result = getProperties(device, nativeBuffer, &properties);
    log("vkGetNativeBufferPropertiesOHOS", result);
    if (result != VK_SUCCESS) {
        return false;
    }

    Report observation;
    observation.nativeFormat = nativeConfig.format;
    observation.nativeStride = nativeConfig.stride;
    observation.nativeUsage = static_cast<std::uint32_t>(
        nativeConfig.usage);
    OH_NativeBuffer_ColorSpace nativeColorSpace = OH_COLORSPACE_NONE;
    observation.nativeColorSpaceResult = OH_NativeBuffer_GetColorSpace(
        nativeBuffer,
        &nativeColorSpace);
    if (observation.nativeColorSpaceResult == 0) {
        observation.nativeColorSpace = static_cast<std::int32_t>(
            nativeColorSpace);
    }
    observation.width = static_cast<std::uint32_t>(nativeConfig.width);
    observation.height = static_cast<std::uint32_t>(nativeConfig.height);
    observation.queriedFormat = formatProperties.format;
    observation.externalFormat = formatProperties.externalFormat;
    observation.formatFeatures = formatProperties.formatFeatures;

    const bool forced =
        options.formatMode == FormatMode::ForcedExplicitFormat;
    if (forced) {
        if (formatProperties.format != VK_FORMAT_UNDEFINED) {
            log("forced mode requires queried VK_FORMAT_UNDEFINED",
                VK_ERROR_FORMAT_NOT_SUPPORTED);
            return false;
        }
        observation.imageFormat = forcedFormat(
            formatProperties.externalFormat);
        if (observation.imageFormat == VK_FORMAT_UNDEFINED) {
            log("map externalFormat to explicit VkFormat",
                VK_ERROR_FORMAT_NOT_SUPPORTED);
            return false;
        }
    } else {
        if (formatProperties.format != VK_FORMAT_UNDEFINED
            || formatProperties.externalFormat == 0) {
            log("opaque mode requires externalFormat",
                VK_ERROR_FORMAT_NOT_SUPPORTED);
            return false;
        }
        observation.imageFormat = VK_FORMAT_UNDEFINED;
    }
    if (report) {
        *report = observation;
    }

    VkSamplerYcbcrConversion conversion = VK_NULL_HANDLE;
    VkSampler sampler = VK_NULL_HANDLE;
    VkImage image = VK_NULL_HANDLE;
    VkDeviceMemory memory = VK_NULL_HANDLE;
    VkImageView view = VK_NULL_HANDLE;
    const auto cleanup = [&] {
        if (view) vkDestroyImageView(device, view, nullptr);
        if (sampler) vkDestroySampler(device, sampler, nullptr);
        if (conversion) {
            vkDestroySamplerYcbcrConversion(device, conversion, nullptr);
        }
        if (image) vkDestroyImage(device, image, nullptr);
        if (memory) vkFreeMemory(device, memory, nullptr);
    };
    const auto checked = [&](const char* operation, VkResult value) {
        log(operation, value);
        if (value != VK_SUCCESS) {
            cleanup();
            return false;
        }
        return true;
    };

    VkExternalFormatOHOS externalFormat {
        VK_STRUCTURE_TYPE_EXTERNAL_FORMAT_OHOS,
    };
    externalFormat.externalFormat = formatProperties.externalFormat;

    if (options.createSamplerObjects) {
        VkSamplerYcbcrConversionCreateInfo conversionInfo {
            VK_STRUCTURE_TYPE_SAMPLER_YCBCR_CONVERSION_CREATE_INFO,
        };
        conversionInfo.pNext = forced ? nullptr : &externalFormat;
        conversionInfo.format = observation.imageFormat;
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
        result = vkCreateSamplerYcbcrConversion(
            device,
            &conversionInfo,
            nullptr,
            &conversion);
        if (!checked("vkCreateSamplerYcbcrConversion", result)) {
            return false;
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
        result = vkCreateSampler(device, &samplerInfo, nullptr, &sampler);
        if (!checked("vkCreateSampler(YCbCr)", result)) {
            return false;
        }
    }

    VkExternalMemoryImageCreateInfo externalMemory {
        VK_STRUCTURE_TYPE_EXTERNAL_MEMORY_IMAGE_CREATE_INFO,
    };
    // This is the exact difference under consultation: forced mode omits
    // VkExternalFormatOHOS while retaining NativeBuffer external memory.
    externalMemory.pNext = forced ? nullptr : &externalFormat;
    externalMemory.handleTypes =
        VK_EXTERNAL_MEMORY_HANDLE_TYPE_OH_NATIVE_BUFFER_BIT_OHOS;
    VkImageCreateInfo imageInfo {
        VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO,
    };
    imageInfo.pNext = &externalMemory;
    imageInfo.imageType = VK_IMAGE_TYPE_2D;
    imageInfo.format = observation.imageFormat;
    imageInfo.extent = {
        observation.width,
        observation.height,
        1,
    };
    imageInfo.mipLevels = 1;
    imageInfo.arrayLayers = 1;
    imageInfo.samples = VK_SAMPLE_COUNT_1_BIT;
    imageInfo.tiling = VK_IMAGE_TILING_OPTIMAL;
    imageInfo.usage = VK_IMAGE_USAGE_SAMPLED_BIT;
    imageInfo.sharingMode = VK_SHARING_MODE_EXCLUSIVE;
    imageInfo.initialLayout = VK_IMAGE_LAYOUT_UNDEFINED;
    result = vkCreateImage(device, &imageInfo, nullptr, &image);
    if (!checked("vkCreateImage(import NativeBuffer)", result)) {
        return false;
    }

    VkMemoryRequirements requirements {};
    vkGetImageMemoryRequirements(device, image, &requirements);
    const std::uint32_t memoryType = firstMemoryType(
        properties.memoryTypeBits & requirements.memoryTypeBits);
    if (memoryType == std::numeric_limits<std::uint32_t>::max()) {
        log("select compatible memory type", VK_ERROR_FORMAT_NOT_SUPPORTED);
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
    result = vkAllocateMemory(device, &allocation, nullptr, &memory);
    if (!checked("vkAllocateMemory(import NativeBuffer)", result)) {
        return false;
    }
    result = vkBindImageMemory(device, image, memory, 0);
    if (!checked("vkBindImageMemory", result)) {
        return false;
    }

    if (options.createSamplerObjects) {
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
        viewInfo.format = observation.imageFormat;
        viewInfo.components = {
            VK_COMPONENT_SWIZZLE_IDENTITY,
            VK_COMPONENT_SWIZZLE_IDENTITY,
            VK_COMPONENT_SWIZZLE_IDENTITY,
            VK_COMPONENT_SWIZZLE_IDENTITY,
        };
        viewInfo.subresourceRange.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
        viewInfo.subresourceRange.levelCount = 1;
        viewInfo.subresourceRange.layerCount = 1;
        result = vkCreateImageView(device, &viewInfo, nullptr, &view);
        if (!checked("vkCreateImageView(YCbCr)", result)) {
            return false;
        }
    }

    const ImportedImage imported {
        image,
        view,
        sampler,
        conversion,
        observation.imageFormat,
        imageInfo.usage,
        observation.width,
        observation.height,
    };
    const bool consumed = !consume || consume(user, imported);
    if (!consumed) {
        log("consume imported image", VK_ERROR_FORMAT_NOT_SUPPORTED);
    }
    cleanup();
    return consumed;
}

} // namespace ohos_native_buffer_probe
