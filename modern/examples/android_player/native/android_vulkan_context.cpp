// SPDX-License-Identifier: LGPL-2.1-or-later

#include "android_vulkan_context.h"

#include <android/log.h>

#include <algorithm>
#include <array>
#include <cstring>
#include <vector>

namespace qtav::android_player {
namespace {

constexpr const char* LogTag = "QtAVCorePlayer";
constexpr const char* ValidationLayer = "VK_LAYER_KHRONOS_validation";

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

bool hasLayer(
    const std::vector<VkLayerProperties>& layers,
    const char* name) noexcept
{
    return std::any_of(
        layers.begin(),
        layers.end(),
        [name](const VkLayerProperties& layer) {
            return std::strcmp(layer.layerName, name) == 0;
        });
}

VKAPI_ATTR VkBool32 VKAPI_CALL debugCallback(
    VkDebugUtilsMessageSeverityFlagBitsEXT severity,
    VkDebugUtilsMessageTypeFlagsEXT,
    const VkDebugUtilsMessengerCallbackDataEXT* data,
    void*) noexcept
{
    int priority = ANDROID_LOG_DEBUG;
    if ((severity & VK_DEBUG_UTILS_MESSAGE_SEVERITY_ERROR_BIT_EXT) != 0U) {
        priority = ANDROID_LOG_ERROR;
    } else if (
        (severity & VK_DEBUG_UTILS_MESSAGE_SEVERITY_WARNING_BIT_EXT) != 0U) {
        priority = ANDROID_LOG_WARN;
    } else if (
        (severity & VK_DEBUG_UTILS_MESSAGE_SEVERITY_INFO_BIT_EXT) != 0U) {
        priority = ANDROID_LOG_INFO;
    }
    __android_log_print(
        priority,
        LogTag,
        "Vulkan validation: %s",
        data && data->pMessage ? data->pMessage : "(no message)");
    return VK_FALSE;
}

VkDebugUtilsMessengerCreateInfoEXT debugMessengerInfo() noexcept
{
    VkDebugUtilsMessengerCreateInfoEXT info {
        VK_STRUCTURE_TYPE_DEBUG_UTILS_MESSENGER_CREATE_INFO_EXT,
    };
    info.messageSeverity =
        VK_DEBUG_UTILS_MESSAGE_SEVERITY_VERBOSE_BIT_EXT
        | VK_DEBUG_UTILS_MESSAGE_SEVERITY_INFO_BIT_EXT
        | VK_DEBUG_UTILS_MESSAGE_SEVERITY_WARNING_BIT_EXT
        | VK_DEBUG_UTILS_MESSAGE_SEVERITY_ERROR_BIT_EXT;
    info.messageType =
        VK_DEBUG_UTILS_MESSAGE_TYPE_GENERAL_BIT_EXT
        | VK_DEBUG_UTILS_MESSAGE_TYPE_VALIDATION_BIT_EXT
        | VK_DEBUG_UTILS_MESSAGE_TYPE_PERFORMANCE_BIT_EXT;
    info.pfnUserCallback = debugCallback;
    return info;
}

} // namespace

AndroidVulkanContext::~AndroidVulkanContext()
{
    reset();
}

void AndroidVulkanContext::reset() noexcept
{
    if (device_) {
        vkDeviceWaitIdle(device_);
        vkDestroyDevice(device_, nullptr);
        device_ = VK_NULL_HANDLE;
    }
    if (debugMessenger_ && instance_) {
        const auto destroyMessenger =
            reinterpret_cast<PFN_vkDestroyDebugUtilsMessengerEXT>(
                vkGetInstanceProcAddr(
                    instance_,
                    "vkDestroyDebugUtilsMessengerEXT"));
        if (destroyMessenger) {
            destroyMessenger(instance_, debugMessenger_, nullptr);
        }
        debugMessenger_ = VK_NULL_HANDLE;
    }
    if (instance_) {
        vkDestroyInstance(instance_, nullptr);
        instance_ = VK_NULL_HANDLE;
    }
    physicalDevice_ = VK_NULL_HANDLE;
    queue_ = VK_NULL_HANDLE;
    queueFamilyIndex_ = 0;
    hdrMetadataEnabled_ = false;
    androidHardwareBufferEnabled_ = false;
    externalSemaphoreFdEnabled_ = false;
    samplerYcbcrConversionEnabled_ = false;
    foreignQueueFamilyEnabled_ = false;
    debugLayerEnabled_ = false;
    deviceName_.clear();
}

bool AndroidVulkanContext::create(
    ANativeWindow* window,
    bool requireMediaCodecInterop,
    bool enableDebugLayer,
    std::string& error)
{
    reset();
    if (!window) {
        error = "Vulkan requires an active Android Surface";
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
            VK_KHR_ANDROID_SURFACE_EXTENSION_NAME)) {
        error = "Required Android Vulkan instance extensions are unavailable";
        return false;
    }

    std::vector<const char*> enabledExtensions {
        VK_KHR_SURFACE_EXTENSION_NAME,
        VK_KHR_ANDROID_SURFACE_EXTENSION_NAME,
    };
    if (hasExtension(
            availableExtensions,
            VK_EXT_SWAPCHAIN_COLOR_SPACE_EXTENSION_NAME)) {
        enabledExtensions.push_back(
            VK_EXT_SWAPCHAIN_COLOR_SPACE_EXTENSION_NAME);
    }

    std::vector<const char*> enabledLayers;
    VkDebugUtilsMessengerCreateInfoEXT debugInfo {};
    if (enableDebugLayer) {
        std::uint32_t layerCount = 0;
        result = vkEnumerateInstanceLayerProperties(&layerCount, nullptr);
        std::vector<VkLayerProperties> layers(layerCount);
        if (result == VK_SUCCESS && layerCount > 0) {
            result = vkEnumerateInstanceLayerProperties(
                &layerCount,
                layers.data());
        }
        if (result != VK_SUCCESS || !hasLayer(layers, ValidationLayer)) {
            error =
                "Vulkan debug layer requested, but "
                "VK_LAYER_KHRONOS_validation is not available on this device";
            return false;
        }
        if (!hasExtension(
                availableExtensions,
                VK_EXT_DEBUG_UTILS_EXTENSION_NAME)) {
            error =
                "Vulkan debug layer requested, but VK_EXT_debug_utils "
                "is unavailable";
            return false;
        }
        enabledLayers.push_back(ValidationLayer);
        enabledExtensions.push_back(VK_EXT_DEBUG_UTILS_EXTENSION_NAME);
        debugInfo = debugMessengerInfo();
    }

    VkApplicationInfo applicationInfo {
        VK_STRUCTURE_TYPE_APPLICATION_INFO,
    };
    applicationInfo.pApplicationName = "QtAVCore Android Player";
    applicationInfo.applicationVersion = VK_MAKE_VERSION(1, 0, 0);
    applicationInfo.pEngineName = "QtAVCore";
    applicationInfo.engineVersion = VK_MAKE_VERSION(2, 0, 0);
    applicationInfo.apiVersion = VK_API_VERSION_1_1;

    VkInstanceCreateInfo instanceInfo {
        VK_STRUCTURE_TYPE_INSTANCE_CREATE_INFO,
    };
    instanceInfo.pApplicationInfo = &applicationInfo;
    instanceInfo.enabledExtensionCount =
        static_cast<std::uint32_t>(enabledExtensions.size());
    instanceInfo.ppEnabledExtensionNames = enabledExtensions.data();
    instanceInfo.enabledLayerCount =
        static_cast<std::uint32_t>(enabledLayers.size());
    instanceInfo.ppEnabledLayerNames = enabledLayers.data();
    instanceInfo.pNext = enableDebugLayer ? &debugInfo : nullptr;
    result = vkCreateInstance(&instanceInfo, nullptr, &instance_);
    if (result != VK_SUCCESS) {
        error = "vkCreateInstance failed: "
            + std::to_string(static_cast<int>(result));
        reset();
        return false;
    }

    if (enableDebugLayer) {
        const auto createMessenger =
            reinterpret_cast<PFN_vkCreateDebugUtilsMessengerEXT>(
                vkGetInstanceProcAddr(
                    instance_,
                    "vkCreateDebugUtilsMessengerEXT"));
        if (!createMessenger
            || createMessenger(
                   instance_,
                   &debugInfo,
                   nullptr,
                   &debugMessenger_)
                != VK_SUCCESS) {
            error = "Could not create the Vulkan validation messenger";
            reset();
            return false;
        }
        debugLayerEnabled_ = true;
    }

    VkAndroidSurfaceCreateInfoKHR surfaceInfo {
        VK_STRUCTURE_TYPE_ANDROID_SURFACE_CREATE_INFO_KHR,
    };
    surfaceInfo.window = window;
    VkSurfaceKHR probeSurface = VK_NULL_HANDLE;
    result = vkCreateAndroidSurfaceKHR(
        instance_,
        &surfaceInfo,
        nullptr,
        &probeSurface);
    if (result != VK_SUCCESS) {
        error = "vkCreateAndroidSurfaceKHR failed: "
            + std::to_string(static_cast<int>(result));
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
            for (std::uint32_t family = 0; family < familyCount; ++family) {
                VkBool32 present = VK_FALSE;
                const VkResult presentResult =
                    vkGetPhysicalDeviceSurfaceSupportKHR(
                        candidate,
                        family,
                        probeSurface,
                        &present);
                if (presentResult == VK_SUCCESS
                    && present == VK_TRUE
                    && (families[family].queueFlags & VK_QUEUE_GRAPHICS_BIT)
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
            "No Vulkan graphics queue can present to the Android Surface";
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

    std::vector<const char*> enabledDeviceExtensions {
        VK_KHR_SWAPCHAIN_EXTENSION_NAME,
    };
    VkPhysicalDeviceSamplerYcbcrConversionFeatures ycbcrFeatures {
        VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_SAMPLER_YCBCR_CONVERSION_FEATURES,
    };
    if (requireMediaCodecInterop) {
        constexpr std::array<const char*, 3> requiredInteropExtensions {
            VK_ANDROID_EXTERNAL_MEMORY_ANDROID_HARDWARE_BUFFER_EXTENSION_NAME,
            VK_KHR_EXTERNAL_SEMAPHORE_FD_EXTENSION_NAME,
            VK_EXT_QUEUE_FAMILY_FOREIGN_EXTENSION_NAME,
        };
        for (const char* extension : requiredInteropExtensions) {
            if (!hasExtension(deviceExtensions, extension)) {
                error = "MediaCodec Vulkan ZeroCopy requires " +
                    std::string(extension);
                reset();
                return false;
            }
            enabledDeviceExtensions.push_back(extension);
        }

        VkPhysicalDeviceFeatures2 features {
            VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_FEATURES_2,
        };
        features.pNext = &ycbcrFeatures;
        vkGetPhysicalDeviceFeatures2(physicalDevice_, &features);
        if (ycbcrFeatures.samplerYcbcrConversion != VK_TRUE) {
            error =
                "MediaCodec Vulkan ZeroCopy requires sampler YCbCr conversion";
            reset();
            return false;
        }
        androidHardwareBufferEnabled_ = true;
        externalSemaphoreFdEnabled_ = true;
        samplerYcbcrConversionEnabled_ = true;
        foreignQueueFamilyEnabled_ = true;
    }
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

    VkDeviceCreateInfo deviceInfo {
        VK_STRUCTURE_TYPE_DEVICE_CREATE_INFO,
    };
    deviceInfo.queueCreateInfoCount = 1;
    deviceInfo.pQueueCreateInfos = &queueInfo;
    deviceInfo.enabledExtensionCount =
        static_cast<std::uint32_t>(enabledDeviceExtensions.size());
    deviceInfo.ppEnabledExtensionNames = enabledDeviceExtensions.data();
    deviceInfo.pNext = requireMediaCodecInterop ? &ycbcrFeatures : nullptr;
    result = vkCreateDevice(
        physicalDevice_,
        &deviceInfo,
        nullptr,
        &device_);
    if (result != VK_SUCCESS) {
        error = "vkCreateDevice failed: "
            + std::to_string(static_cast<int>(result));
        reset();
        return false;
    }
    vkGetDeviceQueue(device_, queueFamilyIndex_, 0, &queue_);

    VkPhysicalDeviceProperties properties {};
    vkGetPhysicalDeviceProperties(physicalDevice_, &properties);
    deviceName_ = properties.deviceName;
    return queue_ != VK_NULL_HANDLE;
}

BorrowedAndroidVulkanContext
AndroidVulkanContext::borrowed() const noexcept
{
    return {
        instance_,
        {
            physicalDevice_,
            device_,
            queue_,
            queueFamilyIndex_,
        },
        hdrMetadataEnabled_,
    };
}

bool AndroidVulkanContext::mediaCodecInteropEnabled() const noexcept
{
    return androidHardwareBufferEnabled_
        && externalSemaphoreFdEnabled_
        && samplerYcbcrConversionEnabled_
        && foreignQueueFamilyEnabled_;
}

bool AndroidVulkanContext::debugLayerEnabled() const noexcept
{
    return debugLayerEnabled_;
}

std::string AndroidVulkanContext::description() const
{
    return deviceName_.empty() ? "Vulkan" : "Vulkan / " + deviceName_;
}

} // namespace qtav::android_player
