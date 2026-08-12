// SPDX-License-Identifier: CC0-1.0
#pragma once

#include <native_buffer/native_buffer.h>
#include <vulkan/vulkan.h>
#include <vulkan/vulkan_ohos.h>

#include <cstdint>

namespace ohos_native_buffer_probe {

enum class FormatMode {
    // Vulkan's opaque external-format path:
    //   VkImageCreateInfo::format = VK_FORMAT_UNDEFINED
    //   VkExternalFormatOHOS is present in the pNext chain.
    OpaqueExternalFormat,

    // Huawei's proposed diagnostic path:
    //   externalFormat 1000156003 -> explicit NV12 VkFormat
    //   externalFormat 1000156013 -> explicit P010 VkFormat
    //   VkExternalFormatOHOS is omitted from the pNext chain.
    ForcedExplicitFormat,
};

struct Options {
    FormatMode formatMode = FormatMode::OpaqueExternalFormat;

    // true: create VkSamplerYcbcrConversion, VkSampler, and VkImageView so an
    // application shader can sample the image.
    // false: import only the VkImage, for direct pl_vulkan_wrap().
    bool createSamplerObjects = true;

    // Opaque external formats default to RGB_IDENTITY while preserving the
    // implementation-defined component mapping returned by the driver. This
    // exposes the encoded samples without guessing another component swizzle.
    // Forced-explicit diagnostic mode ignores this option.
    bool preserveRawYcbcr = true;
};

struct Report {
    std::int32_t nativeFormat = 0;
    std::int32_t nativeStride = 0;
    std::uint64_t nativeUsage = 0;
    std::int32_t nativeColorSpace = 0;
    std::int32_t nativeColorSpaceResult = 0;
    std::uint32_t width = 0;
    std::uint32_t height = 0;
    VkFormat queriedFormat = VK_FORMAT_UNDEFINED;
    std::uint64_t externalFormat = 0;
    VkFormat imageFormat = VK_FORMAT_UNDEFINED;
    VkFormatFeatureFlags formatFeatures = 0;
};

struct ImportedImage {
    VkImage image = VK_NULL_HANDLE;
    VkImageView view = VK_NULL_HANDLE;
    VkSampler sampler = VK_NULL_HANDLE;
    VkSamplerYcbcrConversion conversion = VK_NULL_HANDLE;
    VkFormat format = VK_FORMAT_UNDEFINED;
    VkImageUsageFlags usage = VK_IMAGE_USAGE_SAMPLED_BIT;
    std::uint32_t width = 0;
    std::uint32_t height = 0;
};

using Log = void (*)(const char* operation, VkResult result);
using Consume = bool (*)(void* user, const ImportedImage& image);

// Returns the diagnostic explicit-format mapping tested on the Mate 60 Pro.
// VK_FORMAT_UNDEFINED means that the external ID is not recognized.
VkFormat forcedFormat(std::uint64_t externalFormat) noexcept;

// Imports one retained OHCodec/ConsumerSurface OH_NativeBuffer. The caller
// owns producer/acquire synchronization and must keep nativeBuffer retained
// until this function returns. Imported Vulkan objects are valid only inside
// consume(); they are destroyed before this function returns.
bool probeNativeBuffer(
    VkDevice device,
    OH_NativeBuffer* nativeBuffer,
    const Options& options,
    Log log,
    Consume consume = nullptr,
    void* user = nullptr,
    Report* report = nullptr);

} // namespace ohos_native_buffer_probe
