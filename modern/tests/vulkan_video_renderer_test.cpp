// SPDX-License-Identifier: LGPL-2.1-or-later

#include "vulkan_video_renderer_test_support.h"

#include <qtav/player.h>

#include <chrono>
#include <condition_variable>
#include <cstdint>
#include <iostream>
#include <limits>
#include <mutex>
#include <string>
#include <vector>

namespace {

struct VulkanContext {
    ~VulkanContext()
    {
        if (device) {
            vkDeviceWaitIdle(device);
            vkDestroyDevice(device, nullptr);
        }
        if (instance) {
            vkDestroyInstance(instance, nullptr);
        }
    }

    bool create(std::string& error)
    {
        VkApplicationInfo application {
            VK_STRUCTURE_TYPE_APPLICATION_INFO,
        };
        application.pApplicationName = "QtAVCore Vulkan renderer test";
        application.apiVersion = VK_API_VERSION_1_0;
        VkInstanceCreateInfo instanceInfo {
            VK_STRUCTURE_TYPE_INSTANCE_CREATE_INFO,
        };
        instanceInfo.pApplicationInfo = &application;
        VkResult result =
            vkCreateInstance(&instanceInfo, nullptr, &instance);
        if (result != VK_SUCCESS) {
            error = "vkCreateInstance failed ("
                + std::to_string(static_cast<int>(result)) + ')';
            return false;
        }

        std::uint32_t physicalCount = 0;
        result = vkEnumeratePhysicalDevices(
            instance,
            &physicalCount,
            nullptr);
        std::vector<VkPhysicalDevice> physicalDevices(physicalCount);
        if (result == VK_SUCCESS && physicalCount > 0) {
            result = vkEnumeratePhysicalDevices(
                instance,
                &physicalCount,
                physicalDevices.data());
        }
        if (result != VK_SUCCESS || physicalDevices.empty()) {
            error = "No Vulkan physical device is available";
            return false;
        }

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
                if ((families[family].queueFlags & VK_QUEUE_GRAPHICS_BIT)
                    != 0U) {
                    physicalDevice = candidate;
                    queueFamilyIndex = family;
                    break;
                }
            }
            if (physicalDevice) {
                break;
            }
        }
        if (!physicalDevice) {
            error = "No Vulkan graphics queue is available";
            return false;
        }

        constexpr float priority = 1.0F;
        VkDeviceQueueCreateInfo queueInfo {
            VK_STRUCTURE_TYPE_DEVICE_QUEUE_CREATE_INFO,
        };
        queueInfo.queueFamilyIndex = queueFamilyIndex;
        queueInfo.queueCount = 1;
        queueInfo.pQueuePriorities = &priority;
        VkDeviceCreateInfo deviceInfo {
            VK_STRUCTURE_TYPE_DEVICE_CREATE_INFO,
        };
        deviceInfo.queueCreateInfoCount = 1;
        deviceInfo.pQueueCreateInfos = &queueInfo;
        result = vkCreateDevice(
            physicalDevice,
            &deviceInfo,
            nullptr,
            &device);
        if (result != VK_SUCCESS) {
            error = "vkCreateDevice failed ("
                + std::to_string(static_cast<int>(result)) + ')';
            return false;
        }
        vkGetDeviceQueue(device, queueFamilyIndex, 0, &queue);
        return queue != VK_NULL_HANDLE;
    }

    qtav::BorrowedVulkanDevice borrowed() const noexcept
    {
        return { physicalDevice, device, queue, queueFamilyIndex };
    }

    VkInstance instance = VK_NULL_HANDLE;
    VkPhysicalDevice physicalDevice = VK_NULL_HANDLE;
    VkDevice device = VK_NULL_HANDLE;
    VkQueue queue = VK_NULL_HANDLE;
    std::uint32_t queueFamilyIndex = 0;
};

} // namespace

int main(int argc, char** argv)
{
    if (argc != 2) {
        std::cerr << "expected one deterministic media path\n";
        return 1;
    }

    VulkanContext context;
    std::string error;
    if (!context.create(error)) {
        std::cerr << error << '\n';
        return 77;
    }

    std::mutex mutex;
    std::condition_variable condition;
    bool finished = false;
    bool passed = false;
    qtav::Player player;
    player.onVideoFrame([&](const qtav::VideoFrame& frame, int) {
        passed = qtav::test::runVulkanOffscreenRendererChecks(
            context.borrowed(),
            frame,
            error);
        player.setState(qtav::State::Stopped);
        {
            std::lock_guard<std::mutex> lock(mutex);
            finished = true;
        }
        condition.notify_one();
    });
    player.setMedia(argv[1]);
    player.setState(qtav::State::Playing);

    {
        std::unique_lock<std::mutex> lock(mutex);
        if (!condition.wait_for(
                lock,
                std::chrono::seconds(15),
                [&] { return finished; })) {
            std::cerr << "timed out waiting for a decoded Vulkan test frame\n";
            return 1;
        }
    }
    if (!passed) {
        std::cerr << error << '\n';
        return 1;
    }
    return 0;
}
