// SPDX-License-Identifier: LGPL-2.1-or-later

#include <qtav/android_opengl_video_renderer.h>
#include <qtav/android_vulkan_video_renderer.h>
#include <qtav/player.h>

#include "opengl_video_renderer_test_support.h"
#include "vulkan_video_renderer_test_support.h"

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
#include <cstring>
#include <fcntl.h>
#include <limits>
#include <memory>
#include <mutex>
#include <string>
#include <sys/stat.h>
#include <unistd.h>
#include <utility>
#include <vector>

namespace {

constexpr const char* LogTag = "QtAVCoreTest";
constexpr const char* AssetName = "qtav-test.avi";

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
        std::uint32_t instanceExtensionCount = 0;
        VkResult result = vkEnumerateInstanceExtensionProperties(
            nullptr,
            &instanceExtensionCount,
            nullptr);
        std::vector<VkExtensionProperties> availableInstanceExtensions(
            instanceExtensionCount);
        if (result == VK_SUCCESS && instanceExtensionCount > 0) {
            result = vkEnumerateInstanceExtensionProperties(
                nullptr,
                &instanceExtensionCount,
                availableInstanceExtensions.data());
        }
        if (result != VK_SUCCESS) {
            error = "Could not enumerate Vulkan instance extensions";
            return false;
        }
        std::vector<const char*> instanceExtensions {
            VK_KHR_SURFACE_EXTENSION_NAME,
            VK_KHR_ANDROID_SURFACE_EXTENSION_NAME,
        };
        if (hasExtension(
                availableInstanceExtensions,
                VK_EXT_SWAPCHAIN_COLOR_SPACE_EXTENSION_NAME)) {
            instanceExtensions.push_back(
                VK_EXT_SWAPCHAIN_COLOR_SPACE_EXTENSION_NAME);
            swapchainColorSpaceEnabled = true;
        }
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
        result = vkCreateInstance(&instanceInfo, nullptr, &instance);
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
        std::uint32_t deviceExtensionCount = 0;
        result = vkEnumerateDeviceExtensionProperties(
            physicalDevice,
            nullptr,
            &deviceExtensionCount,
            nullptr);
        std::vector<VkExtensionProperties> availableDeviceExtensions(
            deviceExtensionCount);
        if (result == VK_SUCCESS && deviceExtensionCount > 0) {
            result = vkEnumerateDeviceExtensionProperties(
                physicalDevice,
                nullptr,
                &deviceExtensionCount,
                availableDeviceExtensions.data());
        }
        if (result != VK_SUCCESS
            || !hasExtension(
                availableDeviceExtensions,
                VK_KHR_SWAPCHAIN_EXTENSION_NAME)) {
            error =
                "The Android Vulkan device does not support VK_KHR_swapchain";
            return false;
        }
        std::vector<const char*> deviceExtensions {
            VK_KHR_SWAPCHAIN_EXTENSION_NAME,
        };
        if (hasExtension(
                availableDeviceExtensions,
                VK_EXT_HDR_METADATA_EXTENSION_NAME)) {
            deviceExtensions.push_back(VK_EXT_HDR_METADATA_EXTENSION_NAME);
            hdrMetadataEnabled = true;
        }
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
            + std::to_string(VK_VERSION_PATCH(properties.apiVersion))
            + " swapchain_colorspace="
            + (swapchainColorSpaceEnabled ? "enabled" : "unavailable")
            + " hdr_metadata="
            + (hdrMetadataEnabled ? "enabled" : "unavailable"));
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
            hdrMetadataEnabled,
        };
    }

    VkInstance instance = VK_NULL_HANDLE;
    VkPhysicalDevice physicalDevice = VK_NULL_HANDLE;
    VkDevice device = VK_NULL_HANDLE;
    VkQueue queue = VK_NULL_HANDLE;
    std::uint32_t queueFamilyIndex = 0;
    bool swapchainColorSpaceEnabled = false;
    bool hdrMetadataEnabled = false;
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
        std::lock_guard<std::mutex> lock(windowMutex);
        if (activeWindow) {
            ANativeWindow_release(activeWindow);
            activeWindow = nullptr;
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
            + " audio_frames=" + std::to_string(audioFrames.load())
            + " surface_recreations="
            + std::to_string(surfaceRecreations.load())
            + " gles_fallback="
            + (openGlFallbackPassed.load() ? "pass" : "fail")
            + " gles_offscreen="
            + (openGlOffscreenPassed.load() ? "pass" : "fail")
            + " offscreen=" + (offscreenPassed.load() ? "pass" : "fail")
            + " hdr=pq,hlg native_hdr="
            + (nativeHdrOutputPassed.load() ? "pass" : "fail")
            + " hdr_source="
            + (nativeHdrFramePresented.load() ? "pass" : "fail")
            + " hdr_metadata="
            + (vulkan && vulkan->hdrMetadataEnabled
                    ? "enabled"
                    : "unavailable"));
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
                        } else if (surfaceRecreations.load() == 0) {
                            fail("the Android surface was not recreated");
                        } else if (!offscreenPassed.load()) {
                            fail("the Vulkan offscreen checks did not pass");
                        } else if (!openGlOffscreenPassed.load()) {
                            fail("the OpenGL ES offscreen checks did not pass");
                        } else if (!nativeHdrFramePresented.load()) {
                            fail("the native HDR source frame was not presented");
                        } else if (!runOpenGlFallbackCheck()) {
                            return false;
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
                if (!offscreenChecked.exchange(true)) {
                    std::string error;
                    if (!qtav::test::runVulkanOffscreenRendererChecks(
                            vulkan->borrowed().device,
                            frame,
                            error)) {
                        fail("Vulkan offscreen check: " + error);
                        return;
                    }
                    offscreenPassed = true;
                    if (!qtav::test::runOpenGLOffscreenRendererChecks(
                            frame,
                            qtav::test::makeVulkanHdrTestFrame(),
                            error)) {
                        fail("OpenGL ES offscreen check: " + error);
                        return;
                    }
                    openGlOffscreenPassed = true;
                    player.setState(qtav::State::Paused);
                    logInfo(
                        "QTAV_ANDROID_TEST: OFFSCREEN_PASS hdr=pq,hlg");
                    logInfo(
                        "QTAV_ANDROID_TEST: GLES_OFFSCREEN_PASS "
                        "formats=yuv,nv12,p010,rgb "
                        "geometry=viewport,rotation,recreation");
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
        if (finished.load()) {
            return;
        }
        {
            std::lock_guard<std::mutex> lock(windowMutex);
            if (activeWindow) {
                ANativeWindow_release(activeWindow);
            }
            activeWindow = window;
            if (activeWindow) {
                ANativeWindow_acquire(activeWindow);
            }
        }
        if (started.load()) {
            if (!renderer || !renderer->setWindow(window)) {
                fail("could not recreate the Android Vulkan surface");
                return;
            }
            player.setState(qtav::State::Playing);
            ++surfaceRecreations;
            logInfo(
                "QTAV_ANDROID_TEST: SURFACE_RECREATED count="
                + std::to_string(surfaceRecreations.load()));
            return;
        }
        started = true;
        vulkan = std::make_unique<VulkanContext>();
        std::string error;
        if (!vulkan->create(window, error)) {
            fail(error);
            return;
        }
        renderer =
            std::make_shared<qtav::AndroidVulkanVideoRenderer>(
                vulkan->borrowed(),
                qtav::VulkanOutputPreference::RequireHdr);
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
        if (!renderer->hdrOutputActive()) {
            fail("the Android Vulkan adapter did not activate native HDR");
            return;
        }
        nativeHdrOutputPassed = true;
        const VkSurfaceFormatKHR surfaceFormat = renderer->surfaceFormat();
        logInfo(
            "QTAV_ANDROID_TEST: HDR_SWAPCHAIN format="
            + std::to_string(static_cast<int>(surfaceFormat.format))
            + " color_space="
            + std::to_string(static_cast<int>(surfaceFormat.colorSpace))
            + " metadata="
            + (vulkan->hdrMetadataEnabled ? "enabled" : "unavailable"));
        qtav::VideoRenderConfig config;
        config.surfaceSize = renderer->surfaceSize();
        config.aspectRatio = qtav::VideoAspectRatioMode::Fit;
        if (!renderer->open(config)) {
            fail("could not open Android Vulkan renderer");
            return;
        }
        const qtav::VideoFrame hdrFrame =
            qtav::test::makeVulkanHdrTestFrame();
        if (!hdrFrame || !renderer->render(hdrFrame)) {
            fail("could not present the native HDR test frame");
            return;
        }
        nativeHdrFramePresented = true;
        logInfo(
            "QTAV_ANDROID_TEST: NATIVE_HDR_FRAME transfer=pq "
            "primaries=bt2020 mastering=present maxcll=4000");
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
        player.setMedia(mediaPath);
        player.setState(qtav::State::Playing);
    }

    void windowDestroyed()
    {
        if (!started.load() || finished.load()) {
            return;
        }
        player.setState(qtav::State::Paused);
        if (renderer) {
            renderer->setWindow(nullptr);
        }
        {
            std::lock_guard<std::mutex> lock(windowMutex);
            if (activeWindow) {
                ANativeWindow_release(activeWindow);
                activeWindow = nullptr;
            }
        }
        logInfo("QTAV_ANDROID_TEST: SURFACE_REMOVED");
    }

    bool runOpenGlFallbackCheck()
    {
        ANativeWindow* window = nullptr;
        {
            std::lock_guard<std::mutex> lock(windowMutex);
            window = activeWindow;
            if (window) {
                ANativeWindow_acquire(window);
            }
        }
        if (!window) {
            fail(
                "no active Android window for the OpenGL ES fallback check");
            return false;
        }

        player.setVideoRenderAPI({});
        if (renderer) {
            renderer->close();
        }

        std::string openGlError;
        auto openGlRenderer =
            std::make_shared<qtav::AndroidOpenGLVideoRenderer>();
        openGlRenderer->setEventCallback(
            [&openGlError](const qtav::VideoRenderEvent& event) {
                if (event.type !=
                    qtav::VideoRenderEventType::RedrawRequested) {
                    openGlError = event.detail;
                }
            });
        bool succeeded = openGlRenderer->setWindow(window);
        qtav::VideoRenderConfig openGlConfig;
        if (succeeded) {
            openGlConfig.surfaceSize =
                openGlRenderer->surfaceSize();
            openGlConfig.aspectRatio =
                qtav::VideoAspectRatioMode::Fit;
            succeeded = openGlRenderer->open(openGlConfig);
        }
        const qtav::VideoFrame openGlFrame =
            qtav::test::makeVulkanHdrTestFrame();
        if (succeeded) {
            succeeded = openGlFrame
                && openGlRenderer->render(openGlFrame);
        }
        const std::uint64_t generation =
            openGlRenderer->surfaceGeneration();
        openGlRenderer->close();
        ANativeWindow_release(window);
        if (!succeeded) {
            fail(
                "could not render the OpenGL ES fallback frame: "
                + openGlError);
            return false;
        }
        openGlFallbackPassed = true;
        logInfo(
            "QTAV_ANDROID_TEST: GLES_FALLBACK_PASS "
            "version=3 sdr_tonemap=p010,pq generation="
            + std::to_string(generation));
        return true;
    }

    ANativeActivity* activity = nullptr;
    std::unique_ptr<VulkanContext> vulkan;
    std::shared_ptr<qtav::AndroidVulkanVideoRenderer> renderer;
    std::string mediaPath;
    std::atomic<int> videoFrames { 0 };
    std::atomic<int> renderedFrames { 0 };
    std::atomic<int> audioFrames { 0 };
    std::atomic<int> surfaceRecreations { 0 };
    std::atomic<bool> offscreenChecked { false };
    std::atomic<bool> offscreenPassed { false };
    std::atomic<bool> openGlOffscreenPassed { false };
    std::atomic<bool> openGlFallbackPassed { false };
    std::atomic<bool> nativeHdrFramePresented { false };
    std::atomic<bool> nativeHdrOutputPassed { false };
    std::atomic<bool> finished { false };
    std::atomic<bool> started { false };
    std::mutex windowMutex;
    ANativeWindow* activeWindow = nullptr;
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
