// SPDX-License-Identifier: LGPL-2.1-or-later

#include <qtav/aaudio_audio_sink.h>
#include <qtav/android_opengl_video_renderer.h>
#include <qtav/android_vulkan_video_renderer.h>
#include <qtav/mediacodec_hardware_decoder.h>
#include <qtav/mobile_video_renderer.h>
#include <qtav/player.h>
#include <qtav/swresample_audio_converter.h>

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
#include <time.h>
#include <unistd.h>
#include <utility>
#include <vector>

namespace {

constexpr const char* LogTag = "QtAVCoreTest";
constexpr const char* SoftwareAssetName = "qtav-test.avi";
constexpr const char* MediaCodecH264AssetName =
    "qtav-mediacodec-h264.mp4";
constexpr const char* MediaCodecHevcAssetName =
    "qtav-mediacodec-hevc.mp4";

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

std::string copyTestAsset(
    ANativeActivity* activity,
    const char* assetName)
{
    if (!activity || !activity->assetManager || !activity->internalDataPath
        || !assetName || !assetName[0]) {
        return {};
    }

    AAsset* asset = AAssetManager_open(
        activity->assetManager,
        assetName,
        AASSET_MODE_STREAMING);
    if (!asset) {
        return {};
    }

    const std::string destination =
        std::string(activity->internalDataPath) + '/' + assetName;
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

enum class TestPhase {
    Software,
    WaitingForMediaCodecSurface,
    MediaCodecH264,
    MediaCodecHevc,
};

struct TestState {
    explicit TestState(ANativeActivity* nativeActivity)
        : activity(nativeActivity)
    {
    }

    ~TestState()
    {
        player.setState(qtav::State::Stopped);
        player.setHardwareDecodeConfig({});
        player.setVideoRenderAPI({});
        if (rendererSelector) {
            rendererSelector->close();
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
            + " aaudio="
            + (aaudioClockObserved.load() ? "pass" : "fail")
            + " surface_recreations="
            + std::to_string(surfaceRecreations.load())
            + " gles_fallback="
            + (openGlFallbackPassed.load() ? "pass" : "fail")
            + " gles_offscreen="
            + (openGlOffscreenPassed.load() ? "pass" : "fail")
            + " gles_hdr="
            + (openGlHdrOutputPassed.load() ? "pass" : "fail")
            + " offscreen=" + (offscreenPassed.load() ? "pass" : "fail")
            + " hdr=pq,hlg native_hdr="
            + (nativeHdrOutputPassed.load() ? "pass" : "fail")
            + " hdr_source="
            + (nativeHdrFramePresented.load() ? "pass" : "fail")
            + " mediacodec=h264,hevc"
            + " mediacodec_frames="
            + std::to_string(
                mediaCodecH264Frames.load()
                + mediaCodecHevcFrames.load())
            + " mediacodec_presented="
            + std::to_string(mediaCodecPresented.load())
            + " mediacodec_dropped="
            + std::to_string(mediaCodecDropped.load())
            + " mediacodec_surface_recreations="
            + std::to_string(
                mediaCodecSurfaceRecreations.load())
            + " hdr_metadata="
            + (vulkan && vulkan->hdrMetadataEnabled
                    ? "enabled"
                    : "unavailable"));
        ANativeActivity_finish(activity);
    }

    void start()
    {
        mediaPath = copyTestAsset(activity, SoftwareAssetName);
        mediaCodecH264Path =
            copyTestAsset(activity, MediaCodecH264AssetName);
        mediaCodecHevcPath =
            copyTestAsset(activity, MediaCodecHevcAssetName);
        if (mediaPath.empty() || mediaCodecH264Path.empty()
            || mediaCodecHevcPath.empty()) {
            fail("could not copy packaged media assets");
            return;
        }

        aaudioSink =
            std::make_shared<qtav::AAudioAudioSink>();
        audioConverter =
            std::make_shared<qtav::SwresampleAudioConverter>();
        player
            .setAudioFrameConverter(audioConverter)
            .setAudioSink(aaudioSink)
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
                        handleEndOfMedia();
                    }
                    return false;
                })
            .onStateChanged([this](qtav::State state) {
                if (state == qtav::State::Stopped
                    && phase.load() == TestPhase::MediaCodecHevc
                    && mediaCodecStopRequested.load()) {
                    finishMediaCodecValidation();
                }
            })
            .onVideoFrame([this](const qtav::VideoFrame& frame, int) {
                handleVideoFrame(frame);
            })
            .onAudioFrame([this](const qtav::AudioFrame& frame, int) {
                if (phase.load() != TestPhase::Software) {
                    return;
                }
                if (!frame || frame.sampleRate() != 48'000) {
                    fail("unexpected audio frame");
                    return;
                }
                ++audioFrames;
                observeAAudio();
            });

        logInfo("QTAV_ANDROID_TEST: READY media=" + mediaPath);
    }

    void handleEndOfMedia()
    {
        const TestPhase currentPhase = phase.load();
        if (currentPhase == TestPhase::Software) {
            if (videoFrames.load() < 10) {
                fail("too few decoded video frames");
            } else if (renderedFrames.load() < 10) {
                fail("too few renderer-presented video frames");
            } else if (audioFrames.load() == 0) {
                fail("no decoded audio frames");
            } else if (!aaudioClockObserved.load()) {
                fail("no valid AAudio device clock was observed");
            } else if (!aaudioLatencyObserved.load()) {
                fail("AAudio latency was not reported");
            } else if (surfaceRecreations.load() == 0) {
                fail("the Android surface was not recreated");
            } else if (!offscreenPassed.load()) {
                fail("the Vulkan offscreen checks did not pass");
            } else if (!openGlOffscreenPassed.load()) {
                fail("the OpenGL ES offscreen checks did not pass");
            } else if (!nativeHdrFramePresented.load()) {
                fail("the native HDR source frame was not presented");
            } else if (!runOpenGlFallbackCheck()) {
                return;
            } else {
                phase = TestPhase::WaitingForMediaCodecSurface;
                logInfo(
                    "QTAV_ANDROID_TEST: "
                    "MEDIACODEC_WAITING_FOR_SURFACE_RESET");
            }
            return;
        }

        if (currentPhase == TestPhase::MediaCodecH264) {
            if (mediaCodecH264Frames.load() < 60) {
                fail("too few MediaCodec H.264 outputs");
                return;
            }
            if (!mediaCodecSeekRequested.load()) {
                fail("MediaCodec H.264 seek was not exercised");
                return;
            }
            if (mediaCodecSurfaceRecreations.load() == 0) {
                fail("MediaCodec surface recreation was not exercised");
                return;
            }
            logInfo(
                "QTAV_ANDROID_TEST: MEDIACODEC_H264_PASS frames="
                + std::to_string(mediaCodecH264Frames.load())
                + " surface_recreations="
                + std::to_string(
                    mediaCodecSurfaceRecreations.load()));
            startMediaCodecPhase(TestPhase::MediaCodecHevc);
            return;
        }

        if (currentPhase == TestPhase::MediaCodecHevc
            && !mediaCodecStopRequested.load()) {
            fail("MediaCodec HEVC reached end before explicit stop");
        }
    }

    void handleVideoFrame(const qtav::VideoFrame& frame)
    {
        if (!frame || frame.width() != 160 || frame.height() != 90) {
            fail("unexpected video frame");
            return;
        }

        const TestPhase currentPhase = phase.load();
        if (currentPhase == TestPhase::Software) {
            observeAAudio();
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
            return;
        }

        if (currentPhase != TestPhase::MediaCodecH264
            && currentPhase != TestPhase::MediaCodecHevc) {
            return;
        }

        qtav::MediaCodecSurface surface;
        {
            std::lock_guard<std::mutex> lock(mediaCodecMutex);
            surface = mediaCodecSurface;
        }
        qtav::MediaCodecFrame output =
            qtav::mediaCodecFrame(frame, surface);
        if (!output || !output.isPending()
            || frame.format() != qtav::PixelFormat::Native) {
            fail(
                "MediaCodec returned an invalid or stale direct-surface "
                "output");
            return;
        }

        std::atomic<int>& phaseFrames =
            currentPhase == TestPhase::MediaCodecH264
            ? mediaCodecH264Frames
            : mediaCodecHevcFrames;
        const int frameNumber =
            phaseFrames.fetch_add(1, std::memory_order_acq_rel) + 1;
        if (frameNumber == 1) {
            logInfo(
                std::string(
                    "QTAV_ANDROID_TEST: "
                    "MEDIACODEC_FIRST_OUTPUT codec=")
                + (currentPhase == TestPhase::MediaCodecH264
                        ? "h264"
                        : "hevc")
                + " generation="
                + std::to_string(surface.generation()));
        }
        if (mediaCodecSurfaceRecreations.load() > 0
            && !mediaCodecStaleSurfaceValidated.exchange(true)) {
            qtav::MediaCodecSurface staleSurface;
            {
                std::lock_guard<std::mutex> lock(mediaCodecMutex);
                staleSurface = staleMediaCodecSurface;
            }
            if (!staleSurface
                || qtav::mediaCodecFrame(frame, staleSurface)) {
                fail(
                    "MediaCodec accepted an output for a stale surface "
                    "generation");
                return;
            }
            logInfo(
                "QTAV_ANDROID_TEST: "
                "MEDIACODEC_STALE_SURFACE_REJECTED old_generation="
                + std::to_string(staleSurface.generation())
                + " active_generation="
                + std::to_string(surface.generation()));
        }
        const bool shouldDrop = frameNumber % 17 == 0;
        if (shouldDrop) {
            if (!output.drop()) {
                fail("MediaCodec could not drop an output buffer");
                return;
            }
            ++mediaCodecDropped;
        } else {
            timespec now {};
            if (clock_gettime(CLOCK_MONOTONIC, &now) != 0) {
                fail("could not query CLOCK_MONOTONIC");
                return;
            }
            const std::int64_t presentationTime =
                static_cast<std::int64_t>(now.tv_sec)
                    * 1'000'000'000LL
                + now.tv_nsec + 1'000'000LL;
            if (!output.presentAt(presentationTime)) {
                fail(
                    "MediaCodec could not schedule an output buffer");
                return;
            }
            ++mediaCodecPresented;
        }

        if (currentPhase == TestPhase::MediaCodecH264
            && frameNumber >= 45
            && !mediaCodecSeekRequested.exchange(true)) {
            logInfo(
                "QTAV_ANDROID_TEST: MEDIACODEC_SEEK codec=h264 "
                "target_ms=2500");
            player.seek(2'500);
        }
        if (currentPhase == TestPhase::MediaCodecHevc
            && frameNumber >= 90
            && !mediaCodecStopRequested.exchange(true)) {
            logInfo(
                "QTAV_ANDROID_TEST: MEDIACODEC_STOP codec=hevc "
                "frames=" + std::to_string(frameNumber));
            player.setState(qtav::State::Stopped);
        }
    }

    void startMediaCodecPhase(TestPhase targetPhase)
    {
        qtav::MediaCodecSurface surface;
        if (targetPhase == TestPhase::MediaCodecH264) {
            ANativeWindow* window = retainedActiveWindow();
            if (window) {
                surface = qtav::MediaCodecSurface(window);
                ANativeWindow_release(window);
            }
        } else {
            std::lock_guard<std::mutex> lock(mediaCodecMutex);
            surface = mediaCodecSurface;
        }
        if (!surface) {
            fail("no active surface for MediaCodec direct output");
            return;
        }

        qtav::MediaCodecHardwareDecodeOptions options;
        options.allowSoftwareFallback = false;
        options.extraHardwareFrames = 4;
        const qtav::HardwareDecodeConfig decodeConfig =
            qtav::mediaCodecHardwareDecodeConfig(surface, options);
        if (!decodeConfig.device
            || decodeConfig.decoderWrapper != "mediacodec"
            || decodeConfig.surfaceGeneration != surface.generation()) {
            fail("could not create the MediaCodec decode configuration");
            return;
        }

        {
            std::lock_guard<std::mutex> lock(mediaCodecMutex);
            mediaCodecSurface = surface;
        }
        phase = targetPhase;
        const bool h264 = targetPhase == TestPhase::MediaCodecH264;
        const std::string& path =
            h264 ? mediaCodecH264Path : mediaCodecHevcPath;
        player
            .setVideoRenderAPI({})
            .setRenderCallback({})
            .setAudioSink({})
            .setAudioFrameConverter({});
        player.setMedia(path);
        player.setHardwareDecodeConfig(decodeConfig);
        logInfo(
            std::string(
                "QTAV_ANDROID_TEST: MEDIACODEC_PHASE_READY codec=")
            + (h264 ? "h264" : "hevc")
            + " generation="
            + std::to_string(surface.generation())
            + " media=" + path);
        player.setState(qtav::State::Playing);
    }

    void recreateMediaCodecSurface(ANativeWindow* window)
    {
        qtav::MediaCodecSurface surface(window);
        qtav::MediaCodecHardwareDecodeOptions options;
        options.allowSoftwareFallback = false;
        options.extraHardwareFrames = 4;
        const qtav::HardwareDecodeConfig decodeConfig =
            qtav::mediaCodecHardwareDecodeConfig(surface, options);
        if (!surface || !decodeConfig.device) {
            fail("could not recreate the MediaCodec surface token");
            return;
        }
        {
            std::lock_guard<std::mutex> lock(mediaCodecMutex);
            mediaCodecSurface = surface;
        }
        player.setHardwareDecodeConfig(decodeConfig);
        player.setState(qtav::State::Playing);
        ++mediaCodecSurfaceRecreations;
        logInfo(
            "QTAV_ANDROID_TEST: MEDIACODEC_SURFACE_RECREATED generation="
            + std::to_string(surface.generation())
            + " count="
            + std::to_string(mediaCodecSurfaceRecreations.load()));
    }

    void finishMediaCodecValidation()
    {
        if (mediaCodecHevcFrames.load() < 60
            || mediaCodecPresented.load() == 0
            || mediaCodecDropped.load() == 0) {
            fail("MediaCodec HEVC present/drop validation was incomplete");
            return;
        }
        logInfo(
            "QTAV_ANDROID_TEST: MEDIACODEC_HEVC_PASS frames="
            + std::to_string(mediaCodecHevcFrames.load())
            + " presented="
            + std::to_string(mediaCodecPresented.load())
            + " dropped="
            + std::to_string(mediaCodecDropped.load()));
        pass();
    }

    void observeAAudio()
    {
        if (!aaudioSink) {
            return;
        }
        const qtav::AudioSinkClock current =
            aaudioSink->clock();
        if (!current.valid) {
            return;
        }
        if (current.latencyMilliseconds < 0) {
            fail("AAudio reported negative latency");
            return;
        }
        std::int64_t previous =
            lastAAudioPosition.load(std::memory_order_acquire);
        if (current.positionMilliseconds < previous) {
            fail("AAudio device clock moved backwards");
            return;
        }
        while (current.positionMilliseconds > previous
               && !lastAAudioPosition.compare_exchange_weak(
                   previous,
                   current.positionMilliseconds,
                   std::memory_order_acq_rel,
                   std::memory_order_acquire)) {
        }
        aaudioClockObserved = true;
        aaudioLatencyObserved = true;
        ++aaudioClockSamples;
        if (!aaudioClockLogged.exchange(true)) {
            const qtav::AudioFormat format =
                aaudioSink->deviceFormat();
            const qtav::AAudioStreamInfo info =
                aaudioSink->streamInfo();
            if (!format.isValid()
                || format.sampleFormat
                    != qtav::SampleFormat::Float
                || !info.device
                || info.bufferCapacityInFrames <= 0
                || info.framesPerBurst <= 0) {
                fail("AAudio stream diagnostics are invalid");
                return;
            }
            logInfo(
                "QTAV_ANDROID_TEST: AAUDIO_PASS device="
                + std::to_string(info.device.value())
                + " rate=" + std::to_string(format.sampleRate)
                + " channels=" + std::to_string(format.channels)
                + " format=float"
                + " buffer_frames="
                + std::to_string(info.bufferSizeInFrames)
                + " capacity_frames="
                + std::to_string(info.bufferCapacityInFrames)
                + " burst_frames="
                + std::to_string(info.framesPerBurst)
                + " latency_ms="
                + std::to_string(current.latencyMilliseconds)
                + " clock_ms="
                + std::to_string(current.positionMilliseconds));
        }
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
            const TestPhase currentPhase = phase.load();
            if (currentPhase
                == TestPhase::WaitingForMediaCodecSurface) {
                startMediaCodecPhase(TestPhase::MediaCodecH264);
                return;
            }
            if (currentPhase == TestPhase::MediaCodecH264
                || currentPhase == TestPhase::MediaCodecHevc) {
                recreateMediaCodecSurface(window);
                return;
            }
            if (!rendererSelector
                || !rendererSelector->recreateSurface()) {
                fail(
                    "could not recreate the selected Android renderer");
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
            vulkanInitializationError = error;
            logError(
                "QTAV_ANDROID_TEST: VULKAN_UNAVAILABLE detail="
                + error);
        }
        qtav::MobileRendererSelectorConfig selectorConfig;
        selectorConfig.maximumRecoveryAttempts = 2;
        selectorConfig.vulkan =
            [this] { return createVulkanCandidate(); };
        selectorConfig.openGLES =
            [this] { return createOpenGLCandidate(); };
        rendererSelector =
            std::make_shared<qtav::MobileVideoRendererSelector>(
                std::move(selectorConfig));
        rendererSelector->setSelectionCallback(
            [](const qtav::MobileRendererSelectionEvent& event) {
                logInfo(
                    "QTAV_ANDROID_TEST: RENDERER_SELECTION previous="
                    + std::string(
                        qtav::mobileRenderAPIName(event.previousAPI))
                    + " selected="
                    + qtav::mobileRenderAPIName(event.selectedAPI)
                    + " generation="
                    + std::to_string(event.sessionGeneration)
                    + " detail=" + event.detail);
            });
        rendererSelector->setEventCallback(
            [this](const qtav::VideoRenderEvent& event) {
                if (event.type == qtav::VideoRenderEventType::Error) {
                    fail("mobile renderer: " + event.detail);
                }
            });
        qtav::VideoRenderConfig config;
        config.surfaceSize = {
            ANativeWindow_getWidth(window),
            ANativeWindow_getHeight(window),
        };
        config.aspectRatio = qtav::VideoAspectRatioMode::Fit;
        if (!rendererSelector->open(config)) {
            fail(
                "could not open an Android mobile renderer: "
                + rendererSelector->lastError());
            return;
        }
        if (rendererSelector->selectedAPI()
            != qtav::MobileRenderAPI::Vulkan) {
            fail(
                "the strict HDR device path did not select Vulkan");
            return;
        }
        std::shared_ptr<qtav::AndroidVulkanVideoRenderer>
            activeVulkanRenderer;
        {
            std::lock_guard<std::mutex> lock(rendererMutex);
            activeVulkanRenderer = latestVulkanRenderer;
        }
        if (!activeVulkanRenderer
            || !activeVulkanRenderer->hdrOutputActive()) {
            fail("the Android Vulkan adapter did not activate native HDR");
            return;
        }
        nativeHdrOutputPassed = true;
        const VkSurfaceFormatKHR surfaceFormat =
            activeVulkanRenderer->surfaceFormat();
        logInfo(
            "QTAV_ANDROID_TEST: HDR_SWAPCHAIN format="
            + std::to_string(static_cast<int>(surfaceFormat.format))
            + " color_space="
            + std::to_string(static_cast<int>(surfaceFormat.colorSpace))
            + " metadata="
            + (vulkan->hdrMetadataEnabled ? "enabled" : "unavailable"));
        const qtav::VideoFrame hdrFrame =
            qtav::test::makeVulkanHdrTestFrame();
        if (!hdrFrame || !rendererSelector->render(hdrFrame)) {
            fail("could not present the native HDR test frame");
            return;
        }
        nativeHdrFramePresented = true;
        logInfo(
            "QTAV_ANDROID_TEST: NATIVE_HDR_FRAME transfer=pq "
            "primaries=bt2020 mastering=present maxcll=4000");
        player
            .setVideoRenderAPI(rendererSelector)
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
        const TestPhase currentPhase = phase.load();
        player.setState(qtav::State::Paused);
        if (currentPhase == TestPhase::Software) {
            if (rendererSelector) {
                rendererSelector->suspendSurface();
            }
        } else if (currentPhase
                   == TestPhase::WaitingForMediaCodecSurface) {
            if (openGlHdrSelector) {
                openGlHdrSelector->close();
            }
            if (rendererSelector) {
                rendererSelector->close();
            }
        } else {
            player.setHardwareDecodeConfig({});
            {
                std::lock_guard<std::mutex> lock(mediaCodecMutex);
                staleMediaCodecSurface = mediaCodecSurface;
                mediaCodecSurface = {};
            }
        }
        {
            std::lock_guard<std::mutex> lock(windowMutex);
            if (activeWindow) {
                ANativeWindow_release(activeWindow);
                activeWindow = nullptr;
            }
        }
        if (currentPhase == TestPhase::Software) {
            logInfo("QTAV_ANDROID_TEST: SURFACE_REMOVED");
        } else if (currentPhase
                   == TestPhase::WaitingForMediaCodecSurface) {
            logInfo(
                "QTAV_ANDROID_TEST: "
                "MEDIACODEC_PREPARE_SURFACE_REMOVED");
        } else {
            logInfo(
                "QTAV_ANDROID_TEST: MEDIACODEC_SURFACE_REMOVED");
        }
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
        if (rendererSelector) {
            rendererSelector->suspendSurface();
        }

        const qtav::VideoFrame openGlFrame =
            qtav::test::makeVulkanHdrTestFrame();
        if (!openGlFrame) {
            ANativeWindow_release(window);
            fail("could not create the OpenGL ES HDR source frame");
            return false;
        }

        std::string openGlError;
        std::shared_ptr<qtav::AndroidOpenGLVideoRenderer>
            sdrRenderer;
        qtav::MobileRendererSelectorConfig sdrSelectorConfig;
        sdrSelectorConfig.vulkan = [] {
            return qtav::MobileRendererCandidate {
                {},
                "forced Vulkan-unavailable startup case",
            };
        };
        sdrSelectorConfig.openGLES =
            [window, &openGlError, &sdrRenderer] {
                auto candidate = std::make_shared<
                    qtav::AndroidOpenGLVideoRenderer>(
                        qtav::OpenGLOutputPreference::SdrOnly);
                candidate->setEventCallback(
                    [&openGlError](
                        const qtav::VideoRenderEvent& event) {
                        if (event.type !=
                            qtav::VideoRenderEventType::RedrawRequested) {
                            openGlError = event.detail;
                        }
                    });
                if (!candidate->setWindow(window)) {
                    return qtav::MobileRendererCandidate {
                        {},
                        openGlError.empty()
                            ? "could not create the Android EGL surface"
                            : openGlError,
                    };
                }
                candidate->setEventCallback({});
                sdrRenderer = candidate;
                return qtav::MobileRendererCandidate {
                    std::move(candidate),
                    {},
                };
            };
        qtav::MobileVideoRendererSelector sdrSelector(
            std::move(sdrSelectorConfig));
        qtav::VideoRenderConfig openGlConfig;
        openGlConfig.surfaceSize = {
            ANativeWindow_getWidth(window),
            ANativeWindow_getHeight(window),
        };
        openGlConfig.aspectRatio =
            qtav::VideoAspectRatioMode::Fit;
        bool succeeded = sdrSelector.open(openGlConfig)
            && sdrSelector.selectedAPI()
                == qtav::MobileRenderAPI::OpenGLES
            && sdrSelector.usingFallback()
            && sdrRenderer
            && !sdrRenderer->hdrOutputActive()
            && sdrRenderer->colorComponentBits() == 8
            && sdrRenderer->outputColorSpace()
                == qtav::OpenGLOutputColorSpace::SdrSrgb;
        if (succeeded) {
            succeeded = sdrSelector.render(openGlFrame);
        }
        const std::uint64_t sdrGeneration = sdrRenderer
            ? sdrRenderer->surfaceGeneration()
            : 0;
        sdrSelector.close();
        if (!succeeded) {
            ANativeWindow_release(window);
            fail(
                "could not render the explicit OpenGL ES SDR fallback: "
                + openGlError);
            return false;
        }
        openGlFallbackPassed = true;
        logInfo(
            "QTAV_ANDROID_TEST: GLES_SDR_FALLBACK_PASS "
            "version=3 surface=rgba8 color_space=srgb "
            "sdr_tonemap=p010,pq generation="
            + std::to_string(sdrGeneration));

        qtav::MobileRendererSelectorConfig hdrSelectorConfig;
        hdrSelectorConfig.vulkan = [] {
            return qtav::MobileRendererCandidate {
                {},
                "forced Vulkan-unavailable native-HDR startup case",
            };
        };
        hdrSelectorConfig.openGLES =
            [this] { return createRequiredHdrOpenGLCandidate(); };
        openGlHdrSelector =
            std::make_unique<qtav::MobileVideoRendererSelector>(
                std::move(hdrSelectorConfig));
        if (!openGlHdrSelector->open(openGlConfig)
            || openGlHdrSelector->selectedAPI()
                != qtav::MobileRenderAPI::OpenGLES
            || !openGlHdrSelector->usingFallback()) {
            const std::string detail =
                openGlHdrSelector->lastError();
            ANativeWindow_release(window);
            fail(
                "could not open the required OpenGL ES HDR fallback: "
                + detail);
            return false;
        }
        std::shared_ptr<qtav::AndroidOpenGLVideoRenderer> hdrRenderer;
        {
            std::lock_guard<std::mutex> lock(rendererMutex);
            hdrRenderer = latestOpenGLHdrRenderer;
        }
        if (!hdrRenderer || !hdrRenderer->hdrOutputActive()
            || hdrRenderer->colorComponentBits() != 10
            || !openGlHdrSelector->render(openGlFrame)) {
            ANativeWindow_release(window);
            fail(
                "the Android EGL adapter did not present through a "
                "10-bit native HDR surface");
            return false;
        }
        const qtav::OpenGLOutputColorSpace colorSpace =
            hdrRenderer->outputColorSpace();
        const std::uint64_t hdrGeneration =
            hdrRenderer->surfaceGeneration();
        ANativeWindow_release(window);
        openGlHdrOutputPassed = true;
        logInfo(
            "QTAV_ANDROID_TEST: GLES_HDR_SURFACE_PASS "
            "version=3 surface=rgb10_a2 component_bits=10 color_space="
            + std::string(
                colorSpace == qtav::OpenGLOutputColorSpace::HDR10PQ
                    ? "bt2020_pq"
                    : "bt2020_hlg")
            + " source=p010,pq generation="
            + std::to_string(hdrGeneration));
        return true;
    }

    qtav::MobileRendererCandidate
    createRequiredHdrOpenGLCandidate()
    {
        ANativeWindow* window = retainedActiveWindow();
        if (!window) {
            return { {}, "No active Android native window" };
        }
        std::string error;
        auto candidate =
            std::make_shared<qtav::AndroidOpenGLVideoRenderer>(
                qtav::OpenGLOutputPreference::RequireHdr);
        candidate->setEventCallback(
            [&error](const qtav::VideoRenderEvent& event) {
                if (event.type !=
                    qtav::VideoRenderEventType::RedrawRequested) {
                    error = event.detail;
                }
            });
        const bool succeeded = candidate->setWindow(window);
        ANativeWindow_release(window);
        if (!succeeded) {
            return {
                {},
                error.empty()
                    ? "Could not create the required Android EGL HDR surface"
                    : std::move(error),
            };
        }
        candidate->setEventCallback({});
        {
            std::lock_guard<std::mutex> lock(rendererMutex);
            latestOpenGLHdrRenderer = candidate;
        }
        return { std::move(candidate), {} };
    }

    qtav::MobileRendererCandidate createVulkanCandidate()
    {
        if (!vulkan || !vulkan->borrowed().isValid()) {
            return {
                {},
                vulkanInitializationError.empty()
                    ? "The application did not provide a valid Vulkan context"
                    : vulkanInitializationError,
            };
        }
        ANativeWindow* window = retainedActiveWindow();
        if (!window) {
            return { {}, "No active Android native window" };
        }
        std::string error;
        auto candidate =
            std::make_shared<qtav::AndroidVulkanVideoRenderer>(
                vulkan->borrowed(),
                qtav::VulkanOutputPreference::RequireHdr);
        candidate->setEventCallback(
            [&error](const qtav::VideoRenderEvent& event) {
                if (event.type !=
                    qtav::VideoRenderEventType::RedrawRequested) {
                    error = event.detail;
                }
            });
        const bool succeeded = candidate->setWindow(window);
        ANativeWindow_release(window);
        if (!succeeded) {
            return {
                {},
                error.empty()
                    ? "Could not create the Android Vulkan surface"
                    : std::move(error),
            };
        }
        candidate->setEventCallback({});
        {
            std::lock_guard<std::mutex> lock(rendererMutex);
            latestVulkanRenderer = candidate;
        }
        return { std::move(candidate), {} };
    }

    qtav::MobileRendererCandidate createOpenGLCandidate()
    {
        ANativeWindow* window = retainedActiveWindow();
        if (!window) {
            return { {}, "No active Android native window" };
        }
        std::string error;
        auto candidate =
            std::make_shared<qtav::AndroidOpenGLVideoRenderer>();
        candidate->setEventCallback(
            [&error](const qtav::VideoRenderEvent& event) {
                if (event.type !=
                    qtav::VideoRenderEventType::RedrawRequested) {
                    error = event.detail;
                }
            });
        const bool succeeded = candidate->setWindow(window);
        ANativeWindow_release(window);
        if (!succeeded) {
            return {
                {},
                error.empty()
                    ? "Could not create the Android EGL surface"
                    : std::move(error),
            };
        }
        candidate->setEventCallback({});
        return { std::move(candidate), {} };
    }

    ANativeWindow* retainedActiveWindow()
    {
        std::lock_guard<std::mutex> lock(windowMutex);
        if (activeWindow) {
            ANativeWindow_acquire(activeWindow);
        }
        return activeWindow;
    }

    ANativeActivity* activity = nullptr;
    std::unique_ptr<VulkanContext> vulkan;
    std::shared_ptr<qtav::MobileVideoRendererSelector>
        rendererSelector;
    std::shared_ptr<qtav::AndroidVulkanVideoRenderer>
        latestVulkanRenderer;
    std::unique_ptr<qtav::MobileVideoRendererSelector>
        openGlHdrSelector;
    std::shared_ptr<qtav::AndroidOpenGLVideoRenderer>
        latestOpenGLHdrRenderer;
    std::string vulkanInitializationError;
    std::string mediaPath;
    std::string mediaCodecH264Path;
    std::string mediaCodecHevcPath;
    std::shared_ptr<qtav::AAudioAudioSink> aaudioSink;
    std::shared_ptr<qtav::SwresampleAudioConverter>
        audioConverter;
    std::atomic<TestPhase> phase { TestPhase::Software };
    qtav::MediaCodecSurface mediaCodecSurface;
    qtav::MediaCodecSurface staleMediaCodecSurface;
    std::atomic<int> mediaCodecH264Frames { 0 };
    std::atomic<int> mediaCodecHevcFrames { 0 };
    std::atomic<int> mediaCodecPresented { 0 };
    std::atomic<int> mediaCodecDropped { 0 };
    std::atomic<int> mediaCodecSurfaceRecreations { 0 };
    std::atomic<bool> mediaCodecSeekRequested { false };
    std::atomic<bool> mediaCodecStopRequested { false };
    std::atomic<bool> mediaCodecStaleSurfaceValidated { false };
    std::atomic<int> videoFrames { 0 };
    std::atomic<int> renderedFrames { 0 };
    std::atomic<int> audioFrames { 0 };
    std::atomic<int> surfaceRecreations { 0 };
    std::atomic<int> aaudioClockSamples { 0 };
    std::atomic<std::int64_t> lastAAudioPosition { 0 };
    std::atomic<bool> offscreenChecked { false };
    std::atomic<bool> offscreenPassed { false };
    std::atomic<bool> openGlOffscreenPassed { false };
    std::atomic<bool> openGlFallbackPassed { false };
    std::atomic<bool> openGlHdrOutputPassed { false };
    std::atomic<bool> nativeHdrFramePresented { false };
    std::atomic<bool> nativeHdrOutputPassed { false };
    std::atomic<bool> aaudioClockObserved { false };
    std::atomic<bool> aaudioLatencyObserved { false };
    std::atomic<bool> aaudioClockLogged { false };
    std::atomic<bool> finished { false };
    std::atomic<bool> started { false };
    std::mutex mediaCodecMutex;
    std::mutex rendererMutex;
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
    logInfo("QTAV_ANDROID_TEST: SHUTDOWN_PASS");
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
