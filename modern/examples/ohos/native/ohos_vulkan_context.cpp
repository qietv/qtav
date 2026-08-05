// SPDX-License-Identifier: LGPL-2.1-or-later

#include "ohos_vulkan_context.h"

#include <algorithm>
#include <cstring>
#include <vector>

namespace qtav::ohos_example {
namespace {

bool hasExtension(
    const std::vector<VkExtensionProperties>& extensions,
    const char* name) noexcept
{
    return std::any_of(
        extensions.begin(),
        extensions.end(),
        [name](const VkExtensionProperties& extension) {
            return std::strcmp(extension.extensionName, name) == 0;
        });
}

std::string resultError(const char* operation, VkResult result)
{
    return std::string(operation) + " failed ("
        + std::to_string(static_cast<int>(result)) + ')';
}

} // namespace

OHOSVulkanContext::~OHOSVulkanContext()
{
    reset();
}

void OHOSVulkanContext::reset() noexcept
{
    if (device_) {
        vkDeviceWaitIdle(device_);
        vkDestroyDevice(device_, nullptr);
        device_ = VK_NULL_HANDLE;
    }
    if (instance_) {
        vkDestroyInstance(instance_, nullptr);
        instance_ = VK_NULL_HANDLE;
    }
    physicalDevice_ = VK_NULL_HANDLE;
    queue_ = VK_NULL_HANDLE;
    queueFamilyIndex_ = 0;
    hdrMetadataEnabled_ = false;
    deviceName_.clear();
}

bool OHOSVulkanContext::create(
    OHNativeWindow* window,
    std::string& error)
{
    reset();
    if (!window) {
        error = "Vulkan requires an active XComponent OHNativeWindow";
        return false;
    }

    std::uint32_t extensionCount = 0;
    VkResult result = vkEnumerateInstanceExtensionProperties(
        nullptr,
        &extensionCount,
        nullptr);
    std::vector<VkExtensionProperties> availableExtensions(extensionCount);
    if (result == VK_SUCCESS && extensionCount > 0) {
        result = vkEnumerateInstanceExtensionProperties(
            nullptr,
            &extensionCount,
            availableExtensions.data());
    }
    if (result != VK_SUCCESS
        || !hasExtension(
            availableExtensions,
            VK_KHR_SURFACE_EXTENSION_NAME)
        || !hasExtension(
            availableExtensions,
            VK_OHOS_SURFACE_EXTENSION_NAME)) {
        error = "Required OHOS Vulkan instance extensions are unavailable";
        return false;
    }

    std::vector<const char*> enabledExtensions {
        VK_KHR_SURFACE_EXTENSION_NAME,
        VK_OHOS_SURFACE_EXTENSION_NAME,
    };
    if (hasExtension(
            availableExtensions,
            VK_EXT_SWAPCHAIN_COLOR_SPACE_EXTENSION_NAME)) {
        enabledExtensions.push_back(
            VK_EXT_SWAPCHAIN_COLOR_SPACE_EXTENSION_NAME);
    }

    VkApplicationInfo applicationInfo {
        VK_STRUCTURE_TYPE_APPLICATION_INFO,
    };
    applicationInfo.pApplicationName = "QtAVCore OHOS XComponent";
    applicationInfo.applicationVersion = VK_MAKE_VERSION(1, 0, 0);
    applicationInfo.pEngineName = "QtAVCore";
    applicationInfo.engineVersion = VK_MAKE_VERSION(2, 0, 0);
    applicationInfo.apiVersion = VK_API_VERSION_1_2;

    VkInstanceCreateInfo instanceInfo {
        VK_STRUCTURE_TYPE_INSTANCE_CREATE_INFO,
    };
    instanceInfo.pApplicationInfo = &applicationInfo;
    instanceInfo.enabledExtensionCount =
        static_cast<std::uint32_t>(enabledExtensions.size());
    instanceInfo.ppEnabledExtensionNames = enabledExtensions.data();
    result = vkCreateInstance(&instanceInfo, nullptr, &instance_);
    if (result != VK_SUCCESS) {
        error = resultError("vkCreateInstance", result);
        reset();
        return false;
    }

    VkSurfaceCreateInfoOHOS surfaceInfo {
        VK_STRUCTURE_TYPE_SURFACE_CREATE_INFO_OHOS,
    };
    surfaceInfo.window = window;
    VkSurfaceKHR probeSurface = VK_NULL_HANDLE;
    result = vkCreateSurfaceOHOS(
        instance_,
        &surfaceInfo,
        nullptr,
        &probeSurface);
    if (result != VK_SUCCESS) {
        error = resultError("vkCreateSurfaceOHOS", result);
        reset();
        return false;
    }

    std::uint32_t physicalDeviceCount = 0;
    result = vkEnumeratePhysicalDevices(
        instance_,
        &physicalDeviceCount,
        nullptr);
    std::vector<VkPhysicalDevice> physicalDevices(physicalDeviceCount);
    if (result == VK_SUCCESS && physicalDeviceCount > 0) {
        result = vkEnumeratePhysicalDevices(
            instance_,
            &physicalDeviceCount,
            physicalDevices.data());
    }
    bool foundQueue = false;
    if (result == VK_SUCCESS) {
        for (VkPhysicalDevice candidate : physicalDevices) {
            std::uint32_t familyCount = 0;
            vkGetPhysicalDeviceQueueFamilyProperties(
                candidate,
                &familyCount,
                nullptr);
            std::vector<VkQueueFamilyProperties> families(familyCount);
            vkGetPhysicalDeviceQueueFamilyProperties(
                candidate,
                &familyCount,
                families.data());
            for (std::uint32_t family = 0;
                 family < familyCount;
                 ++family) {
                VkBool32 present = VK_FALSE;
                const VkResult presentResult =
                    vkGetPhysicalDeviceSurfaceSupportKHR(
                        candidate,
                        family,
                        probeSurface,
                        &present);
                if (presentResult == VK_SUCCESS
                    && present == VK_TRUE
                    && (families[family].queueFlags
                        & VK_QUEUE_GRAPHICS_BIT)
                        != 0U) {
                    physicalDevice_ = candidate;
                    queueFamilyIndex_ = family;
                    foundQueue = true;
                    break;
                }
            }
            if (foundQueue) {
                break;
            }
        }
    }
    vkDestroySurfaceKHR(instance_, probeSurface, nullptr);
    if (!foundQueue) {
        error =
            "No Vulkan graphics queue can present to the XComponent surface";
        reset();
        return false;
    }

    std::uint32_t deviceExtensionCount = 0;
    result = vkEnumerateDeviceExtensionProperties(
        physicalDevice_,
        nullptr,
        &deviceExtensionCount,
        nullptr);
    std::vector<VkExtensionProperties> deviceExtensions(
        deviceExtensionCount);
    if (result == VK_SUCCESS && deviceExtensionCount > 0) {
        result = vkEnumerateDeviceExtensionProperties(
            physicalDevice_,
            nullptr,
            &deviceExtensionCount,
            deviceExtensions.data());
    }
    if (result != VK_SUCCESS
        || !hasExtension(
            deviceExtensions,
            VK_KHR_SWAPCHAIN_EXTENSION_NAME)) {
        error = "VK_KHR_swapchain is unavailable";
        reset();
        return false;
    }

    VkPhysicalDeviceVulkan12Features supportedVulkan12Features {
        VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_VULKAN_1_2_FEATURES,
    };
    VkPhysicalDeviceFeatures2 supportedFeatures {
        VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_FEATURES_2,
    };
    supportedFeatures.pNext = &supportedVulkan12Features;
    vkGetPhysicalDeviceFeatures2(physicalDevice_, &supportedFeatures);
    if (supportedVulkan12Features.timelineSemaphore != VK_TRUE
        || supportedVulkan12Features.hostQueryReset != VK_TRUE) {
        error =
            "libplacebo requires Vulkan timelineSemaphore and hostQueryReset";
        reset();
        return false;
    }

    std::vector<const char*> enabledDeviceExtensions {
        VK_KHR_SWAPCHAIN_EXTENSION_NAME,
    };
    if (hasExtension(
            deviceExtensions,
            VK_EXT_HDR_METADATA_EXTENSION_NAME)) {
        enabledDeviceExtensions.push_back(
            VK_EXT_HDR_METADATA_EXTENSION_NAME);
        hdrMetadataEnabled_ = true;
    }

    constexpr float QueuePriority = 1.0F;
    VkDeviceQueueCreateInfo queueInfo {
        VK_STRUCTURE_TYPE_DEVICE_QUEUE_CREATE_INFO,
    };
    queueInfo.queueFamilyIndex = queueFamilyIndex_;
    queueInfo.queueCount = 1;
    queueInfo.pQueuePriorities = &QueuePriority;

    VkPhysicalDeviceVulkan12Features enabledVulkan12Features {
        VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_VULKAN_1_2_FEATURES,
    };
    enabledVulkan12Features.timelineSemaphore = VK_TRUE;
    enabledVulkan12Features.hostQueryReset = VK_TRUE;
    VkDeviceCreateInfo deviceInfo {
        VK_STRUCTURE_TYPE_DEVICE_CREATE_INFO,
    };
    deviceInfo.queueCreateInfoCount = 1;
    deviceInfo.pQueueCreateInfos = &queueInfo;
    deviceInfo.enabledExtensionCount =
        static_cast<std::uint32_t>(enabledDeviceExtensions.size());
    deviceInfo.ppEnabledExtensionNames = enabledDeviceExtensions.data();
    deviceInfo.pNext = &enabledVulkan12Features;
    result = vkCreateDevice(
        physicalDevice_,
        &deviceInfo,
        nullptr,
        &device_);
    if (result != VK_SUCCESS) {
        error = resultError("vkCreateDevice", result);
        reset();
        return false;
    }
    vkGetDeviceQueue(device_, queueFamilyIndex_, 0, &queue_);

    VkPhysicalDeviceProperties properties {};
    vkGetPhysicalDeviceProperties(physicalDevice_, &properties);
    deviceName_ = properties.deviceName;
    if (!queue_) {
        error = "vkGetDeviceQueue returned a null graphics queue";
        reset();
        return false;
    }
    return true;
}

BorrowedOHOSVulkanContext OHOSVulkanContext::borrowed() const noexcept
{
    return {
        instance_,
        {
            instance_,
            physicalDevice_,
            device_,
            queue_,
            queueFamilyIndex_,
            true,
            true,
        },
        hdrMetadataEnabled_,
    };
}

std::string OHOSVulkanContext::description() const
{
    return deviceName_.empty() ? "Vulkan" : "Vulkan / " + deviceName_;
}

} // namespace qtav::ohos_example
