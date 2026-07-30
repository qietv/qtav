// SPDX-License-Identifier: LGPL-2.1-or-later

#include <qtav/android_vulkan_video_renderer.h>
#include <qtav/player.h>

#include <android/asset_manager.h>
#include <android/log.h>
#include <android/native_activity.h>
#include <android/native_window.h>

#include <algorithm>
#include <atomic>
#include <array>
#include <cerrno>
#include <cstddef>
#include <cstdint>
#include <fcntl.h>
#include <limits>
#include <memory>
#include <string>
#include <sys/stat.h>
#include <unistd.h>
#include <utility>
#include <vector>

namespace {

constexpr const char* LogTag = "QtAVCoreTest";
constexpr const char* AssetName = "qtav-test.avi";

void logInfo(const std::string& message)
{
    __android_log_print(ANDROID_LOG_INFO, LogTag, "%s", message.c_str());
}

void logError(const std::string& message)
{
    __android_log_print(ANDROID_LOG_ERROR, LogTag, "%s", message.c_str());
}

bool writeAll(int descriptor, const void* data, std::size_t size)
{
    const auto* bytes = static_cast<const std::uint8_t*>(data);
    while (size > 0) {
        const ssize_t written = write(descriptor, bytes, size);
        if (written < 0) {
            if (errno == EINTR) {
                continue;
            }
            return false;
        }
        bytes += written;
        size -= static_cast<std::size_t>(written);
    }
    return true;
}

std::string copyTestAsset(ANativeActivity* activity)
{
    if (!activity || !activity->assetManager || !activity->internalDataPath) {
        return {};
    }

    AAsset* asset = AAssetManager_open(
        activity->assetManager,
        AssetName,
        AASSET_MODE_STREAMING);
    if (!asset) {
        return {};
    }

    const std::string destination =
        std::string(activity->internalDataPath) + '/' + AssetName;
    const int descriptor = open(
        destination.c_str(),
        O_CREAT | O_TRUNC | O_WRONLY | O_CLOEXEC,
        S_IRUSR | S_IWUSR);
    if (descriptor < 0) {
        AAsset_close(asset);
        return {};
    }

    bool succeeded = true;
    std::uint8_t buffer[16 * 1024];
    for (;;) {
        const int count = AAsset_read(asset, buffer, sizeof(buffer));
        if (count == 0) {
            break;
        }
        if (count < 0
            || !writeAll(
                descriptor,
                buffer,
                static_cast<std::size_t>(count))) {
            succeeded = false;
            break;
        }
    }

    if (close(descriptor) != 0) {
        succeeded = false;
    }
    AAsset_close(asset);
    if (!succeeded) {
        unlink(destination.c_str());
        return {};
    }
    return destination;
}

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

    VulkanContext(const VulkanContext&) = delete;
    VulkanContext& operator=(const VulkanContext&) = delete;
    VulkanContext() = default;

    bool create(ANativeWindow* window, std::string& error)
    {
        const std::array<const char*, 2> instanceExtensions {
            VK_KHR_SURFACE_EXTENSION_NAME,
            VK_KHR_ANDROID_SURFACE_EXTENSION_NAME,
        };
        VkApplicationInfo application {
            VK_STRUCTURE_TYPE_APPLICATION_INFO,
        };
        application.pApplicationName = "QtAVCore Android test";
        application.applicationVersion = VK_MAKE_VERSION(1, 0, 0);
        application.pEngineName = "QtAVCore";
        application.engineVersion = VK_MAKE_VERSION(2, 0, 0);
        application.apiVersion = VK_API_VERSION_1_0;
        VkInstanceCreateInfo instanceInfo {
            VK_STRUCTURE_TYPE_INSTANCE_CREATE_INFO,
        };
        instanceInfo.pApplicationInfo = &application;
        instanceInfo.enabledExtensionCount =
            static_cast<std::uint32_t>(instanceExtensions.size());
        instanceInfo.ppEnabledExtensionNames = instanceExtensions.data();
        VkResult result =
            vkCreateInstance(&instanceInfo, nullptr, &instance);
        if (result != VK_SUCCESS) {
            error = "vkCreateInstance failed: "
                + std::to_string(static_cast<int>(result));
            return false;
        }

        VkAndroidSurfaceCreateInfoKHR surfaceInfo {
            VK_STRUCTURE_TYPE_ANDROID_SURFACE_CREATE_INFO_KHR,
        };
        surfaceInfo.window = window;
        VkSurfaceKHR probeSurface = VK_NULL_HANDLE;
        result = vkCreateAndroidSurfaceKHR(
            instance,
            &surfaceInfo,
            nullptr,
            &probeSurface);
        if (result != VK_SUCCESS) {
            error = "vkCreateAndroidSurfaceKHR probe failed: "
                + std::to_string(static_cast<int>(result));
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
            vkDestroySurfaceKHR(instance, probeSurface, nullptr);
            error = "No Android Vulkan physical device is available";
            return false;
        }

        bool found = false;
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
                const VkResult support =
                    vkGetPhysicalDeviceSurfaceSupportKHR(
                        candidate,
                        family,
                        probeSurface,
                        &present);
                if (support == VK_SUCCESS
                    && present == VK_TRUE
                    && (families[family].queueFlags
                        & VK_QUEUE_GRAPHICS_BIT)
                        != 0U) {
                    physicalDevice = candidate;
                    queueFamilyIndex = family;
                    found = true;
                    break;
                }
            }
            if (found) {
                break;
            }
        }
        vkDestroySurfaceKHR(instance, probeSurface, nullptr);
        if (!found) {
            error =
                "No Android Vulkan graphics queue can present to this window";
            return false;
        }

        constexpr float priority = 1.0F;
        VkDeviceQueueCreateInfo queueInfo {
            VK_STRUCTURE_TYPE_DEVICE_QUEUE_CREATE_INFO,
        };
        queueInfo.queueFamilyIndex = queueFamilyIndex;
        queueInfo.queueCount = 1;
        queueInfo.pQueuePriorities = &priority;
        const std::array<const char*, 1> deviceExtensions {
            VK_KHR_SWAPCHAIN_EXTENSION_NAME,
        };
        VkDeviceCreateInfo deviceInfo {
            VK_STRUCTURE_TYPE_DEVICE_CREATE_INFO,
        };
        deviceInfo.queueCreateInfoCount = 1;
        deviceInfo.pQueueCreateInfos = &queueInfo;
        deviceInfo.enabledExtensionCount =
            static_cast<std::uint32_t>(deviceExtensions.size());
        deviceInfo.ppEnabledExtensionNames = deviceExtensions.data();
        result = vkCreateDevice(
            physicalDevice,
            &deviceInfo,
            nullptr,
            &device);
        if (result != VK_SUCCESS) {
            error = "vkCreateDevice failed: "
                + std::to_string(static_cast<int>(result));
            return false;
        }
        vkGetDeviceQueue(device, queueFamilyIndex, 0, &queue);

        VkPhysicalDeviceProperties properties {};
        vkGetPhysicalDeviceProperties(physicalDevice, &properties);
        logInfo(
            "QTAV_ANDROID_TEST: VULKAN device="
            + std::string(properties.deviceName)
            + " api="
            + std::to_string(VK_VERSION_MAJOR(properties.apiVersion))
            + '.'
            + std::to_string(VK_VERSION_MINOR(properties.apiVersion))
            + '.'
            + std::to_string(VK_VERSION_PATCH(properties.apiVersion)));
        return queue != VK_NULL_HANDLE;
    }

    qtav::BorrowedAndroidVulkanContext borrowed() const noexcept
    {
        return {
            instance,
            {
                physicalDevice,
                device,
                queue,
                queueFamilyIndex,
            },
        };
    }

    VkInstance instance = VK_NULL_HANDLE;
    VkPhysicalDevice physicalDevice = VK_NULL_HANDLE;
    VkDevice device = VK_NULL_HANDLE;
    VkQueue queue = VK_NULL_HANDLE;
    std::uint32_t queueFamilyIndex = 0;
};

struct TestState {
    explicit TestState(ANativeActivity* nativeActivity)
        : activity(nativeActivity)
    {
    }

    ~TestState()
    {
        player.setState(qtav::State::Stopped);
        player.setVideoRenderAPI({});
        if (renderer) {
            renderer->close();
        }
    }

    void fail(const std::string& detail)
    {
        bool expected = false;
        if (finished.compare_exchange_strong(expected, true)) {
            logError("QTAV_ANDROID_TEST: FAIL " + detail);
        }
    }

    void pass()
    {
        bool expected = false;
        if (!finished.compare_exchange_strong(expected, true)) {
            return;
        }
        logInfo(
            "QTAV_ANDROID_TEST: PASS video_frames="
            + std::to_string(videoFrames.load())
            + " rendered_frames=" + std::to_string(renderedFrames.load())
            + " audio_frames=" + std::to_string(audioFrames.load()));
    }

    void start()
    {
        mediaPath = copyTestAsset(activity);
        if (mediaPath.empty()) {
            fail("could not copy packaged media asset");
            return;
        }

        player
            .onEvent([](const qtav::MediaEvent& event) {
                logError(
                    "event category=" + event.category
                    + " error=" + std::to_string(event.error)
                    + " detail=" + event.detail);
                return false;
            })
            .onMediaStatus(
                [this](qtav::MediaStatus, qtav::MediaStatus status) {
                    if (status == qtav::MediaStatus::Invalid) {
                        fail("media status became invalid");
                    } else if (status == qtav::MediaStatus::EndOfMedia) {
                        if (videoFrames.load() < 10) {
                            fail("too few decoded video frames");
                        } else if (renderedFrames.load() < 10) {
                            fail("too few Vulkan-rendered video frames");
                        } else if (audioFrames.load() == 0) {
                            fail("no decoded audio frames");
                        } else {
                            pass();
                        }
                    }
                    return false;
                })
            .onVideoFrame([this](const qtav::VideoFrame& frame, int) {
                if (!frame || frame.width() != 160 || frame.height() != 90) {
                    fail("unexpected video frame");
                    return;
                }
                ++videoFrames;
            })
            .onAudioFrame([this](const qtav::AudioFrame& frame, int) {
                if (!frame || frame.sampleRate() != 48'000) {
                    fail("unexpected audio frame");
                    return;
                }
                ++audioFrames;
            });

        logInfo("QTAV_ANDROID_TEST: READY media=" + mediaPath);
    }

    void windowCreated(ANativeWindow* window)
    {
        if (finished.load() || started.exchange(true)) {
            return;
        }
        vulkan = std::make_unique<VulkanContext>();
        std::string error;
        if (!vulkan->create(window, error)) {
            fail(error);
            return;
        }
        renderer =
            std::make_shared<qtav::AndroidVulkanVideoRenderer>(
                vulkan->borrowed());
        renderer->setEventCallback(
            [this](const qtav::VideoRenderEvent& event) {
                if (event.type == qtav::VideoRenderEventType::Error) {
                    fail("Vulkan renderer: " + event.detail);
                }
            });
        if (!renderer->setWindow(window)) {
            fail("could not create Android Vulkan surface");
            return;
        }
        qtav::VideoRenderConfig config;
        config.surfaceSize = renderer->surfaceSize();
        config.aspectRatio = qtav::VideoAspectRatioMode::Fit;
        if (!renderer->open(config)) {
            fail("could not open Android Vulkan renderer");
            return;
        }
        player
            .setVideoRenderAPI(renderer)
            .setRenderCallback([this](void*) {
                if (player.renderVideo() >= 0.0) {
                    ++renderedFrames;
                }
            });
        logInfo(
            "QTAV_ANDROID_TEST: START media=" + mediaPath
            + " surface="
            + std::to_string(config.surfaceSize.width)
            + 'x' + std::to_string(config.surfaceSize.height));
        player.setPlaybackRate(4.0F);
        player.setMedia(mediaPath);
        player.setState(qtav::State::Playing);
    }

    void windowDestroyed()
    {
        player.setState(qtav::State::Stopped);
        if (renderer) {
            renderer->setWindow(nullptr);
        }
    }

    ANativeActivity* activity = nullptr;
    std::unique_ptr<VulkanContext> vulkan;
    std::shared_ptr<qtav::AndroidVulkanVideoRenderer> renderer;
    std::string mediaPath;
    std::atomic<int> videoFrames { 0 };
    std::atomic<int> renderedFrames { 0 };
    std::atomic<int> audioFrames { 0 };
    std::atomic<bool> finished { false };
    std::atomic<bool> started { false };
    // Destroyed first so its worker is joined before borrowed Vulkan objects.
    qtav::Player player;
};

void onDestroy(ANativeActivity* activity)
{
    if (!activity) {
        return;
    }
    auto* state = static_cast<TestState*>(activity->instance);
    activity->instance = nullptr;
    delete state;
}

void onNativeWindowCreated(
    ANativeActivity* activity,
    ANativeWindow* window)
{
    if (activity && activity->instance) {
        static_cast<TestState*>(activity->instance)->windowCreated(window);
    }
}

void onNativeWindowDestroyed(
    ANativeActivity* activity,
    ANativeWindow*)
{
    if (activity && activity->instance) {
        static_cast<TestState*>(activity->instance)->windowDestroyed();
    }
}

} // namespace

extern "C" __attribute__((visibility("default")))
void ANativeActivity_onCreate(
    ANativeActivity* activity,
    void*,
    std::size_t)
{
    if (!activity || !activity->callbacks) {
        return;
    }
    auto state = std::make_unique<TestState>(activity);
    activity->callbacks->onDestroy = onDestroy;
    activity->callbacks->onNativeWindowCreated =
        onNativeWindowCreated;
    activity->callbacks->onNativeWindowDestroyed =
        onNativeWindowDestroyed;
    activity->instance = state.release();
    static_cast<TestState*>(activity->instance)->start();
}
