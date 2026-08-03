// SPDX-License-Identifier: LGPL-2.1-or-later

#include "android_vulkan_context.h"

#include <android/log.h>
#include <android/native_window_jni.h>
#include <jni.h>

#include <qtav/aaudio_audio_sink.h>
#include <qtav/android_opengl_video_renderer.h>
#include <qtav/android_vulkan_video_renderer.h>
#include <qtav/mediacodec_hardware_decoder.h>
#include <qtav/mediacodec_opengl_interop.h>
#include <qtav/mediacodec_vulkan_interop.h>
#include <qtav/player.h>
#include <qtav/swresample_audio_converter.h>

#include <algorithm>
#include <array>
#include <atomic>
#include <chrono>
#include <cmath>
#include <condition_variable>
#include <cstddef>
#include <cstdint>
#include <cstdlib>
#include <deque>
#include <dlfcn.h>
#include <limits>
#include <memory>
#include <mutex>
#include <string>
#include <thread>
#include <unordered_map>
#include <unistd.h>
#include <utility>

namespace {

constexpr const char* LogTag = "QtAVCorePlayer";
// The Player callback is already paced against the playback clock. While an
// asynchronous AImageReader import is pending, retain a bounded
// set of exact outputs already released to the producer plus only the newest
// not-yet-released candidate. A deep second FIFO can exhaust MediaCodec output
// slots and then present obsolete frames in a burst after the path recovers.
constexpr std::size_t MaximumQueuedRenderFrames = 4;
// Keep this below the interop's 250 ms timestamp-correlation window for both
// 24 and 25 fps media, and leave AImageReader capacity for images retained by
// the Vulkan frames-in-flight ring.
constexpr std::size_t MaximumPendingRenderFrames = 4;
constexpr std::int64_t MaximumPendingRenderAgeMilliseconds = 250;
constexpr std::size_t FrameRateSampleCount = 32;
constexpr std::chrono::milliseconds PresentationFpsWindow { 1'500 };
constexpr std::chrono::milliseconds PresentationFpsStaleAfter { 750 };
JavaVM* GlobalJavaVM = nullptr;
std::string GlobalCaBundlePath;

std::string describeVideoColorSpace(qtav::VideoColorSpace color)
{
    const char* primaries = "unknown primaries";
    switch (color.primaries) {
    case qtav::ColorPrimaries::BT709:
        primaries = "BT.709";
        break;
    case qtav::ColorPrimaries::BT2020:
        primaries = "BT.2020";
        break;
    case qtav::ColorPrimaries::SMPTE431:
        primaries = "DCI-P3";
        break;
    case qtav::ColorPrimaries::SMPTE432:
        primaries = "Display-P3";
        break;
    default:
        break;
    }

    const char* transfer = "unknown transfer";
    switch (color.transfer) {
    case qtav::ColorTransfer::BT709:
    case qtav::ColorTransfer::SMPTE170M:
        transfer = "BT.709";
        break;
    case qtav::ColorTransfer::SRGB:
        transfer = "sRGB";
        break;
    case qtav::ColorTransfer::Linear:
        transfer = "linear";
        break;
    case qtav::ColorTransfer::PQ:
        transfer = "PQ";
        break;
    case qtav::ColorTransfer::HLG:
        transfer = "HLG";
        break;
    default:
        break;
    }

    const char* range = "unknown range";
    if (color.range == qtav::ColorRange::Limited) {
        range = "limited";
    } else if (color.range == qtav::ColorRange::Full) {
        range = "full";
    }
    return std::string(primaries) + "/" + transfer + "/" + range;
}

const char* describeVulkanColorSpace(VkColorSpaceKHR colorSpace) noexcept
{
    switch (colorSpace) {
    case VK_COLOR_SPACE_SRGB_NONLINEAR_KHR:
        return "sRGB";
    case VK_COLOR_SPACE_BT709_NONLINEAR_EXT:
        return "BT.709";
    case VK_COLOR_SPACE_EXTENDED_SRGB_LINEAR_EXT:
        return "extended-sRGB/linear";
    case VK_COLOR_SPACE_BT2020_LINEAR_EXT:
        return "BT.2020/linear";
    case VK_COLOR_SPACE_HDR10_ST2084_EXT:
        return "BT.2020/PQ";
    case VK_COLOR_SPACE_HDR10_HLG_EXT:
        return "BT.2020/HLG";
    default:
        return "unknown";
    }
}

const char* describeOpenGLColorSpace(
    qtav::OpenGLOutputColorSpace colorSpace) noexcept
{
    switch (colorSpace) {
    case qtav::OpenGLOutputColorSpace::SdrSrgb:
        return "sRGB";
    case qtav::OpenGLOutputColorSpace::HDR10PQ:
        return "BT.2020/PQ";
    case qtav::OpenGLOutputColorSpace::HDR10HLG:
        return "BT.2020/HLG";
    }
    return "unknown";
}

int32_t setWindowFrameRate(
    ANativeWindow* window,
    float frameRate) noexcept
{
    if (!window) {
        return -1;
    }
    using SetFrameRateWithStrategy = int32_t (*)(
        ANativeWindow*,
        float,
        int8_t,
        int8_t);
    using SetFrameRate = int32_t (*)(
        ANativeWindow*,
        float,
        int8_t);
    static const auto withStrategy =
        reinterpret_cast<SetFrameRateWithStrategy>(
            dlsym(
                RTLD_DEFAULT,
                "ANativeWindow_setFrameRateWithChangeStrategy"));
    static const auto basic = reinterpret_cast<SetFrameRate>(
        dlsym(RTLD_DEFAULT, "ANativeWindow_setFrameRate"));
    const int8_t compatibility = frameRate > 0.0F
        ? ANATIVEWINDOW_FRAME_RATE_COMPATIBILITY_FIXED_SOURCE
        : ANATIVEWINDOW_FRAME_RATE_COMPATIBILITY_DEFAULT;
    if (withStrategy) {
        constexpr int8_t ChangeFrameRateAlways = 1;
        return withStrategy(
            window,
            frameRate,
            compatibility,
            ChangeFrameRateAlways);
    }
    return basic ? basic(window, frameRate, compatibility) : -1;
}

void logMessage(int priority, const std::string& message)
{
    __android_log_print(priority, LogTag, "%s", message.c_str());
}

const char* stateName(qtav::State state) noexcept
{
    switch (state) {
    case qtav::State::Stopped:
        return "Stopped";
    case qtav::State::Playing:
        return "Playing";
    case qtav::State::Paused:
        return "Paused";
    }
    return "Unknown";
}

const char* statusName(qtav::MediaStatus status) noexcept
{
    switch (status) {
    case qtav::MediaStatus::NoMedia:
        return "No media";
    case qtav::MediaStatus::Loading:
        return "Loading";
    case qtav::MediaStatus::Loaded:
        return "Loaded";
    case qtav::MediaStatus::Buffering:
        return "Buffering";
    case qtav::MediaStatus::EndOfMedia:
        return "End of media";
    case qtav::MediaStatus::Invalid:
        return "Invalid media";
    }
    return "Unknown";
}

std::string fromJavaString(JNIEnv* environment, jstring value)
{
    if (!environment || !value) {
        return {};
    }
    const char* characters =
        environment->GetStringUTFChars(value, nullptr);
    if (!characters) {
        return {};
    }
    std::string result(characters);
    environment->ReleaseStringUTFChars(value, characters);
    return result;
}

void applyRemoteTlsOptions(qtav::Player& player)
{
    player.setProperty("avformat.tls_verify", "1");
    if (!GlobalCaBundlePath.empty()) {
        player.setProperty("avformat.ca_file", GlobalCaBundlePath);
    }
    player.setProperty(
        "avformat.user_agent",
        "QtAVCore-Android-Player/1.0");
}

struct PlayerOptions {
    bool vulkan = true;
    bool hdr = true;
    bool zeroCopy = true;
    bool hardwareDecode = true;

    bool operator==(const PlayerOptions& other) const noexcept
    {
        return vulkan == other.vulkan
            && hdr == other.hdr
            && zeroCopy == other.zeroCopy
            && hardwareDecode == other.hardwareDecode;
    }

    bool operator!=(const PlayerOptions& other) const noexcept
    {
        return !(*this == other);
    }
};

class AndroidPlayerController final {
public:
    explicit AndroidPlayerController(JavaVM* javaVM)
        : javaVM_(javaVM)
    {
        setStatus("Create a Surface and open a local or remote media file");
        renderThread_ = std::thread([this] { runRenderThread(); });
    }

    ~AndroidPlayerController()
    {
        {
            std::lock_guard<std::mutex> lock(commandMutex_);
            userWantsPlaying_.store(false);
            releasePlayerAndPipeline();
            setFrameRateWindow(nullptr);
            if (window_) {
                ANativeWindow_release(window_);
                window_ = nullptr;
            }
            closeMediaDescriptor();
        }
        stopRenderThread();
    }

    void setSurface(ANativeWindow* window, int displayRotation)
    {
        std::lock_guard<std::mutex> lock(commandMutex_);
        if (window) {
            displayRotation_ = std::clamp(displayRotation, 0, 3);
        }
        if (window == window_) {
            // surfaceCreated() and the initial surfaceChanged() normally
            // deliver the same ANativeWindow. The JNI call acquired another
            // reference. A later surfaceChanged() can still mean that the
            // buffer geometry changed in place, so refresh only the active
            // presentation target without rebuilding the decoder.
            if (window) {
                ANativeWindow_release(window);
            }
            if (vulkanRenderer_) {
                const bool refreshed =
                    vulkanRenderer_->setWindow(window_);
                qtav::VideoRenderConfig config;
                config.surfaceSize = vulkanRenderer_->surfaceSize();
                config.aspectRatio = qtav::VideoAspectRatioMode::Fit;
                config.rotation = static_cast<qtav::VideoRotation>(
                    displayRotation_);
                if (refreshed
                    && !vulkanRenderer_->configure(config)) {
                    const std::string message =
                        "Vulkan renderer could not apply the rotated Surface size";
                    if (setPipelineErrorOnce(message)) {
                        logMessage(ANDROID_LOG_ERROR, message);
                    }
                }
            } else if (openGLRenderer_) {
                openGLRenderer_->setWindow(window_);
            }
            return;
        }
        const std::int64_t resumePosition = resumePositionLocked();
        releasePlayerAndPipeline();
        if (window_) {
            ANativeWindow_release(window_);
        }
        window_ = window;
        setFrameRateWindow(window_);
        savedPosition_ = resumePosition;

        if (!window_) {
            setStatus(
                mediaPath_.empty()
                    ? "Surface released"
                    : "Surface released; playback will resume when it returns");
            return;
        }
        if (!mediaPath_.empty()) {
            rebuildPlayer(savedPosition_, userWantsPlaying_.load());
        } else {
            setStatus("Surface ready; open a local or remote media file");
        }
    }

    void openMedia(
        std::string label,
        std::string path,
        int descriptor)
    {
        std::lock_guard<std::mutex> lock(commandMutex_);
        releasePlayerAndPipeline();
        closeMediaDescriptor();

        mediaDescriptor_ = descriptor;
        mediaLabel_ = std::move(label);
        mediaPath_ = descriptor >= 0
            ? "fd:"
            : std::move(path);
        savedPosition_ = 0;
        userWantsPlaying_.store(true);
        videoSizePacked_.store(0);

        if (mediaPath_.empty()) {
            setStatus("The selected media has no readable path or descriptor");
            closeMediaDescriptor();
            return;
        }
        if (!window_) {
            setStatus("Media selected; waiting for the Android Surface");
            return;
        }
        rebuildPlayer(0, true);
    }

    void setOptions(PlayerOptions options)
    {
        std::lock_guard<std::mutex> lock(commandMutex_);
        if (options == options_) {
            return;
        }
        const bool directSurfaceActive =
            options_.hardwareDecode && !options_.zeroCopy;
        const bool directSurfaceRemainsActive =
            options.hardwareDecode && !options.zeroCopy;
        const std::int64_t resumePosition = resumePositionLocked();
        options_ = options;
        savedPosition_ = resumePosition;
        // Vulkan/OpenGL and the native HDR-output policy are application-
        // renderer preferences. Preserve them while Direct Surface is active
        // without interrupting playback. Java independently applies the HDR
        // switch as this SurfaceView layer's requested Android headroom.
        // Changing ZeroCopy or hardware decode still leaves this branch and
        // rebuilds the appropriate renderer below.
        if (directSurfaceActive && directSurfaceRemainsActive) {
            return;
        }
        if (!mediaPath_.empty() && window_) {
            rebuildPlayer(savedPosition_, userWantsPlaying_.load());
        }
    }

    void togglePlayback()
    {
        std::lock_guard<std::mutex> lock(commandMutex_);
        if (!player_) {
            if (!mediaPath_.empty() && window_) {
                userWantsPlaying_.store(true);
                rebuildPlayer(savedPosition_, true);
            }
            return;
        }
        const qtav::State state = player_->state();
        if (state == qtav::State::Playing) {
            userWantsPlaying_.store(false);
            resetPresentationContinuity();
            player_->setState(qtav::State::Paused);
        } else {
            const qtav::MediaStatus mediaStatus = player_->mediaStatus();
            if (state == qtav::State::Stopped
                || mediaStatus == qtav::MediaStatus::EndOfMedia
                || mediaStatus == qtav::MediaStatus::Invalid) {
                userWantsPlaying_.store(true);
                savedPosition_ = 0;
                rebuildPlayer(0, true);
                return;
            }
            userWantsPlaying_.store(true);
            resetPresentationContinuity();
            player_->setState(qtav::State::Playing);
        }
    }

    void stop()
    {
        std::lock_guard<std::mutex> lock(commandMutex_);
        userWantsPlaying_.store(false);
        savedPosition_ = 0;
        resetPresentationContinuity();
        clearScheduledRenderFrames();
        if (vulkanInterop_) {
            vulkanInterop_->flush();
        }
        if (openGLInterop_) {
            openGLInterop_->flush();
        }
        if (player_) {
            player_->setState(qtav::State::Stopped);
        }
        setStatus("Stopped");
    }

    void seek(std::int64_t position)
    {
        std::lock_guard<std::mutex> lock(commandMutex_);
        savedPosition_ = std::max<std::int64_t>(0, position);
        resetPresentationContinuity();
        clearScheduledRenderFrames();
        if (vulkanInterop_) {
            vulkanInterop_->flush();
        }
        if (openGLInterop_) {
            openGLInterop_->flush();
        }
        if (player_ && !player_->seek(savedPosition_)) {
            setStatus("Seek was rejected because the media is not seekable");
        }
    }

    std::int64_t position()
    {
        std::lock_guard<std::mutex> lock(commandMutex_);
        return currentPositionLocked();
    }

    std::int64_t duration()
    {
        std::lock_guard<std::mutex> lock(commandMutex_);
        if (!player_) {
            return duration_.load();
        }
        const std::int64_t currentDuration =
            player_->mediaInfo().duration;
        if (currentDuration > 0) {
            duration_.store(currentDuration);
        }
        return duration_.load();
    }

    bool isPlaying()
    {
        std::lock_guard<std::mutex> lock(commandMutex_);
        return player_ && player_->state() == qtav::State::Playing;
    }

    std::uint64_t videoSizePacked() const noexcept
    {
        return videoSizePacked_.load();
    }

    int requestedFrameRateMilliHertz() const noexcept
    {
        return requestedFrameRateMilliHertz_.load();
    }

    std::string outputColorSpace() const
    {
        std::lock_guard<std::mutex> lock(commandMutex_);
        if (directEnabled_.load()) {
            std::lock_guard<std::mutex> outputLock(outputMutex_);
            return directOutputColorSpace_.empty()
                ? "codec pending"
                : "codec " + directOutputColorSpace_;
        }
        if (vulkanRenderer_) {
            const VkSurfaceFormatKHR format =
                vulkanRenderer_->surfaceFormat();
            return format.format == VK_FORMAT_UNDEFINED
                ? "Vulkan pending"
                : std::string("Vulkan swapchain ")
                    + describeVulkanColorSpace(format.colorSpace);
        }
        if (openGLRenderer_) {
            return std::string("EGL surface ")
                + describeOpenGLColorSpace(
                    openGLRenderer_->outputColorSpace());
        }
        return "unavailable";
    }

    std::string status() const
    {
        std::string result;
        {
            std::lock_guard<std::mutex> lock(statusMutex_);
            result = status_;
        }
        const std::uint64_t decoded = decodedVideoFrames_.load();
        const std::uint64_t presented = directEnabled_.load()
            ? directPresentedFrames_.load()
            : renderedVideoFrames_.load();
        if (decoded > 0 || presented > 0) {
            result += " · callbacks/presented " + std::to_string(decoded)
                + "/" + std::to_string(presented);
        }
        const std::uint64_t dolbyVisionFrames =
            dolbyVisionFrames_.load();
        if (dolbyVisionFrames > 0) {
            result += " · Dolby Vision RPU frames "
                + std::to_string(dolbyVisionFrames);
        }
        if (presented > 0) {
            int frameRateMilliHertz = 0;
            {
                std::lock_guard<std::mutex> lock(timingMutex_);
                const auto now = std::chrono::steady_clock::now();
                if (lastPresentationTime_
                        != std::chrono::steady_clock::time_point {}
                    && now - lastPresentationTime_
                        <= PresentationFpsStaleAfter) {
                    frameRateMilliHertz = presentationFpsMilliHertz_;
                }
            }
            const int frameRateTenths =
                (frameRateMilliHertz + 50) / 100;
            result += " · present fps "
                + std::to_string(frameRateTenths / 10)
                + "." + std::to_string(frameRateTenths % 10);
        }
        const std::uint64_t queueDrops = renderQueueDrops_.load();
        if (queueDrops > 0) {
            result += " · render queue drops "
                + std::to_string(queueDrops);
        }
        const std::uint64_t timedRenders = timedRenderCount_.load();
        if (timedRenders > 0) {
            result += " · render avg/max "
                + std::to_string(
                    totalRenderMicroseconds_.load() / timedRenders)
                + "/"
                + std::to_string(maximumRenderMicroseconds_.load())
                + "us";
        }
        qtav::PlaybackStatistics coreStatistics;
        {
            std::lock_guard<std::mutex> lock(commandMutex_);
            if (player_) {
                coreStatistics = player_->playbackStatistics();
            }
        }
        if (coreStatistics.decodedVideoFrames > 0
            || coreStatistics.deliveredVideoFrames > 0) {
            result += " · core decoded/delivered "
                + std::to_string(coreStatistics.decodedVideoFrames)
                + "/"
                + std::to_string(coreStatistics.deliveredVideoFrames)
                + " drops queue/late "
                + std::to_string(
                    coreStatistics.videoQueueOverflowDrops)
                + "/"
                + std::to_string(coreStatistics.lateVideoDrops)
                + " maxQ "
                + std::to_string(
                    coreStatistics.maximumQueuedVideoFrames)
                + " starve/max "
                + std::to_string(
                    coreStatistics.videoPresentationStarvations)
                + "/"
                + std::to_string(
                    coreStatistics
                        .maximumVideoPresentationStarvationMilliseconds)
                + "ms";
        }
        const std::uint64_t attempts = renderAttempts_.load();
        if (attempts > 0 && !directEnabled_.load()) {
            result += " · render attempts " + std::to_string(attempts);
        }
        const std::uint64_t bufferImports =
            hardwareBufferImports_.load();
        const std::uint64_t cacheHits =
            hardwareBufferImportCacheHits_.load();
        if (bufferImports > 0 || cacheHits > 0) {
            result += " · AHB imports/hits "
                + std::to_string(bufferImports) + "/"
                + std::to_string(cacheHits)
                + " max "
                + std::to_string(
                    maximumCachedHardwareBufferImports_.load());
        }
        const std::uint64_t rawYcbcrImports =
            unconvertedYcbcrImports_.load();
        if (rawYcbcrImports > 0) {
            result += " · raw YCbCr imports "
                + std::to_string(rawYcbcrImports);
        }
        const std::uint64_t codecOutputs =
            interopCodecOutputsQueued_.load();
        const std::uint64_t imagesAcquired =
            interopImagesAcquired_.load();
        const std::uint64_t imagesImported =
            interopImagesImported_.load();
        if (codecOutputs > 0 || imagesAcquired > 0
            || imagesImported > 0) {
            result += " · interop queued/acquired/imported "
                + std::to_string(codecOutputs) + "/"
                + std::to_string(imagesAcquired) + "/"
                + std::to_string(imagesImported)
                + " stale "
                + std::to_string(interopStaleImagesDropped_.load())
                + " pending max "
                + std::to_string(interopMaximumPendingImages_.load());
        }
        const auto openGLHdrStatus =
            static_cast<qtav::MediaCodecOpenGLHdrSamplingStatus>(
                openGLHdrSamplingStatus_.load());
        if (openGLHdrStatus
            == qtav::MediaCodecOpenGLHdrSamplingStatus::Supported) {
            result += " · raw AHB/EGLImage YCbCr active fmt "
                + std::to_string(openGLHardwareBufferFormat_.load());
        } else if (openGLHdrStatus
                   == qtav::MediaCodecOpenGLHdrSamplingStatus::Unsupported) {
            result += " · raw AHB/EGLImage YCbCr unsupported";
        }
        const int frameRate = requestedFrameRateMilliHertz_.load();
        if (frameRate > 0) {
            result += " · rate hint "
                + std::to_string(frameRate / 1'000);
            const int fraction = frameRate % 1'000;
            if (fraction != 0) {
                result += "." + std::to_string(fraction);
            }
            result += "fps";
            if (frameRateRequestResult_.load() != 0) {
                result += " rejected";
            }
        }
        const std::uint64_t sourceSkipped = sourceFramesSkipped_.load();
        if (sourceSkipped > 0) {
            result += " · source gaps " + std::to_string(sourceSkipped);
        }
        const std::uint64_t stalls = presentationStalls_.load();
        const std::uint64_t catchups = presentationCatchups_.load();
        if (stalls > 0 || catchups > 0) {
            result += " · pacing stalls/catchups "
                + std::to_string(stalls) + "/"
                + std::to_string(catchups)
                + " max "
                + std::to_string(
                    maximumPresentationGapMilliseconds_.load())
                + "ms";
        }
        return result;
    }

private:
    static constexpr std::int64_t InvalidTimestamp =
        std::numeric_limits<std::int64_t>::min();

    void observeDolbyVisionFrame(const qtav::VideoFrame& frame)
    {
        if (!frame.hasDolbyVisionMetadata()) {
            return;
        }
        const std::uint64_t count =
            dolbyVisionFrames_.fetch_add(1) + 1;
        if (count == 1) {
            logMessage(
                ANDROID_LOG_INFO,
                directEnabled_.load()
                    ? "First Dolby Vision RPU metadata frame received; MediaCodec direct Surface bypasses libplacebo"
                    : "First Dolby Vision RPU metadata frame received; libplacebo reshaping is active");
        }
    }

    void observeVideoSize(int width, int height) noexcept
    {
        if (width <= 0 || height <= 0) {
            return;
        }
        videoSizePacked_.store(
            (static_cast<std::uint64_t>(
                 static_cast<std::uint32_t>(width))
             << 32U)
            | static_cast<std::uint32_t>(height));
    }

    void recordDirectOutputColorSpace(const qtav::VideoFrame& frame)
    {
        const qtav::VideoColorSpace color = frame.colorSpaceInfo();
        if (!color.isSpecified()) {
            return;
        }
        std::lock_guard<std::mutex> lock(outputMutex_);
        directOutputColorSpace_ = describeVideoColorSpace(color);
    }

    void setFrameRateWindow(ANativeWindow* window)
    {
        std::lock_guard<std::mutex> lock(timingMutex_);
        if (window == frameRateWindow_) {
            return;
        }
        if (frameRateWindow_) {
            setWindowFrameRate(frameRateWindow_, 0.0F);
            ANativeWindow_release(frameRateWindow_);
        }
        frameRateWindow_ = window;
        if (frameRateWindow_) {
            ANativeWindow_acquire(frameRateWindow_);
        }
        requestedFrameRateMilliHertz_.store(0);
        frameRateRequestResult_.store(0);
        frameRateDeltaCount_ = 0;
        lastCallbackTimestamp_ = InvalidTimestamp;
        lastPresentedTimestamp_ = InvalidTimestamp;
        lastPresentationTime_ = {};
        presentationTimes_.clear();
        presentationFpsMilliHertz_ = 0;
    }

    void resetPresentationTiming(std::uint64_t generation)
    {
        std::lock_guard<std::mutex> lock(timingMutex_);
        timingGeneration_ = generation;
        if (frameRateWindow_
            && requestedFrameRateMilliHertz_.load() > 0) {
            setWindowFrameRate(frameRateWindow_, 0.0F);
        }
        requestedFrameRateMilliHertz_.store(0);
        frameRateRequestResult_.store(0);
        frameRateDeltaCount_ = 0;
        lastCallbackTimestamp_ = InvalidTimestamp;
        lastPresentedTimestamp_ = InvalidTimestamp;
        lastPresentationTime_ = {};
        presentationTimes_.clear();
        presentationFpsMilliHertz_ = 0;
        sourceFramesSkipped_.store(0);
        presentationStalls_.store(0);
        presentationCatchups_.store(0);
        maximumPresentationGapMilliseconds_.store(0);
    }

    void resetPresentationContinuity()
    {
        std::lock_guard<std::mutex> lock(timingMutex_);
        lastCallbackTimestamp_ = InvalidTimestamp;
        lastPresentedTimestamp_ = InvalidTimestamp;
        lastPresentationTime_ = {};
        presentationTimes_.clear();
        presentationFpsMilliHertz_ = 0;
    }

    void observeVideoCallback(
        std::int64_t timestamp,
        std::uint64_t generation)
    {
        std::lock_guard<std::mutex> lock(timingMutex_);
        if (timingGeneration_ != generation) {
            return;
        }
        if (lastCallbackTimestamp_ != InvalidTimestamp) {
            const std::int64_t delta =
                timestamp - lastCallbackTimestamp_;
            if (delta > 0 && delta < 250
                && frameRateDeltaCount_ < frameRateDeltas_.size()) {
                frameRateDeltas_[frameRateDeltaCount_++] = delta;
            }
            const int requested =
                requestedFrameRateMilliHertz_.load();
            if (requested > 0 && delta > 0 && delta < 1'000) {
                const double expected =
                    1'000'000.0 / static_cast<double>(requested);
                if (static_cast<double>(delta) > expected * 1.5) {
                    const auto skipped = std::max<std::int64_t>(
                        1,
                        static_cast<std::int64_t>(
                            std::llround(
                                static_cast<double>(delta) / expected))
                            - 1);
                    sourceFramesSkipped_.fetch_add(
                        static_cast<std::uint64_t>(skipped));
                }
            }
        }
        lastCallbackTimestamp_ = timestamp;

        if (requestedFrameRateMilliHertz_.load() > 0
            || frameRateDeltaCount_ < FrameRateSampleCount) {
            return;
        }
        auto sorted = frameRateDeltas_;
        std::sort(
            sorted.begin(),
            sorted.begin()
                + static_cast<std::ptrdiff_t>(frameRateDeltaCount_));
        const double median = static_cast<double>(
            sorted[frameRateDeltaCount_ / 2]);
        double total = 0.0;
        std::size_t inliers = 0;
        for (std::size_t index = 0;
             index < frameRateDeltaCount_;
             ++index) {
            const double value =
                static_cast<double>(frameRateDeltas_[index]);
            if (value >= median * 0.75
                && value <= median * 1.25) {
                total += value;
                ++inliers;
            }
        }
        if (inliers < FrameRateSampleCount / 2 || total <= 0.0) {
            frameRateDeltaCount_ = 0;
            return;
        }
        double frameRate =
            1'000.0 * static_cast<double>(inliers) / total;
        constexpr std::array<double, 10> CommonFrameRates {
            23.976,
            24.0,
            25.0,
            29.97,
            30.0,
            48.0,
            50.0,
            59.94,
            60.0,
            120.0,
        };
        for (const double common : CommonFrameRates) {
            if (std::abs(frameRate - common) / common <= 0.01) {
                frameRate = common;
                break;
            }
        }
        if (frameRate < 10.0 || frameRate > 240.0) {
            frameRateDeltaCount_ = 0;
            return;
        }
        requestedFrameRateMilliHertz_.store(
            static_cast<int>(std::llround(frameRate * 1'000.0)));
        frameRateRequestResult_.store(
            setWindowFrameRate(
                frameRateWindow_,
                static_cast<float>(frameRate)));
    }

    void recordPresentation(
        const qtav::VideoFrame& frame,
        std::uint64_t generation)
    {
        const auto now = std::chrono::steady_clock::now();
        std::lock_guard<std::mutex> lock(timingMutex_);
        if (timingGeneration_ != generation) {
            return;
        }
        if (lastPresentedTimestamp_ != InvalidTimestamp
            && lastPresentationTime_
                != std::chrono::steady_clock::time_point {}) {
            const std::int64_t sourceDelta =
                frame.timestamp() - lastPresentedTimestamp_;
            const auto actualDelta =
                std::chrono::duration_cast<std::chrono::milliseconds>(
                    now - lastPresentationTime_)
                    .count();
            if (sourceDelta > 0 && sourceDelta < 500
                && actualDelta >= 0 && actualDelta < 2'000) {
                const std::int64_t tolerance = std::max<std::int64_t>(
                    10,
                    sourceDelta / 2);
                if (actualDelta > sourceDelta + tolerance) {
                    presentationStalls_.fetch_add(1);
                } else if (actualDelta + tolerance < sourceDelta) {
                    presentationCatchups_.fetch_add(1);
                }
                std::uint64_t maximum =
                    maximumPresentationGapMilliseconds_.load();
                while (maximum
                           < static_cast<std::uint64_t>(actualDelta)
                       && !maximumPresentationGapMilliseconds_
                               .compare_exchange_weak(
                                   maximum,
                                   static_cast<std::uint64_t>(actualDelta))) {
                }
            }
        }
        presentationTimes_.push_back(now);
        const auto windowStart = now - PresentationFpsWindow;
        while (presentationTimes_.size() > 1
               && presentationTimes_.front() < windowStart) {
            presentationTimes_.pop_front();
        }
        if (presentationTimes_.size() > 1) {
            const double elapsedSeconds =
                std::chrono::duration<double>(
                    presentationTimes_.back()
                    - presentationTimes_.front())
                    .count();
            if (elapsedSeconds > 0.0) {
                presentationFpsMilliHertz_ = static_cast<int>(
                    std::llround(
                        static_cast<double>(
                            presentationTimes_.size() - 1)
                        * 1'000.0 / elapsedSeconds));
            }
        } else {
            presentationFpsMilliHertz_ = 0;
        }
        lastPresentedTimestamp_ = frame.timestamp();
        lastPresentationTime_ = now;
    }

    void bindCallbacks(
        const std::shared_ptr<qtav::Player>& player,
        std::uint64_t generation)
    {
        std::weak_ptr<qtav::Player> weakPlayer = player;
        player
            ->onStateChanged([this, generation](qtav::State state) {
                if (playerGeneration_.load() != generation) {
                    return;
                }
                setPlaybackStatus(stateName(state));
            })
            .onMediaStatus(
                [this, weakPlayer, generation](
                    qtav::MediaStatus,
                    qtav::MediaStatus status) {
                    if (playerGeneration_.load() != generation) {
                        return false;
                    }
                    if (auto current = weakPlayer.lock()) {
                        const qtav::MediaInfo mediaInfo =
                            current->mediaInfo();
                        const std::int64_t mediaDuration =
                            mediaInfo.duration;
                        if (mediaDuration > 0) {
                            duration_.store(mediaDuration);
                        }
                        const auto activeTrack = std::find_if(
                            mediaInfo.tracks.begin(),
                            mediaInfo.tracks.end(),
                            [&mediaInfo](const qtav::TrackInfo& track) {
                                return track.index
                                    == mediaInfo.activeVideoTrack;
                            });
                        if (activeTrack != mediaInfo.tracks.end()) {
                            observeVideoSize(
                                activeTrack->width,
                                activeTrack->height);
                        }
                    }
                    if (status == qtav::MediaStatus::EndOfMedia) {
                        userWantsPlaying_.store(false);
                    }
                    setPlaybackStatus(statusName(status));
                    return false;
                })
            .onEvent([this, generation](const qtav::MediaEvent& event) {
                if (playerGeneration_.load() != generation) {
                    return false;
                }
                const std::string message =
                    event.category + ": " + event.detail;
                logMessage(
                    event.error == 0
                        ? ANDROID_LOG_WARN
                        : ANDROID_LOG_ERROR,
                    message);
                setStatus(message);
                return false;
            })
            .onVideoFrame(
                [this, weakPlayer, generation](
                    const qtav::VideoFrame& frame,
                    int) {
                    if (playerGeneration_.load() != generation) {
                        return;
                    }
                    decodedVideoFrames_.fetch_add(1);
                    observeDolbyVisionFrame(frame);
                    observeVideoSize(frame.width(), frame.height());
                    latestVideoTimestamp_.store(frame.timestamp());
                    observeVideoCallback(frame.timestamp(), generation);
                    if (!frame.hasHardwareFrame()
                        && frame.format() == qtav::PixelFormat::Unknown) {
                        unsupportedSoftwareFrame_.store(true);
                        const std::string message =
                            "Software video format '" + frame.formatName()
                            + "' is not supported by the mobile renderer; "
                              "enable Hardware decode for this media";
                        if (setPipelineErrorOnce(message)) {
                            logMessage(ANDROID_LOG_ERROR, message);
                        }
                        userWantsPlaying_.store(false);
                        if (auto current = weakPlayer.lock()) {
                            current->setState(qtav::State::Paused);
                        }
                        return;
                    }
                    if (directEnabled_.load()) {
                        recordDirectOutputColorSpace(frame);
                        presentDirectSurfaceFrame(frame, generation);
                    } else {
                        enqueueRenderFrame(frame, generation);
                    }
                });
    }

    bool createVulkanRenderer(
        bool useHardwareZeroCopy,
        qtav::HardwareDecodeConfig& hardwareConfig,
        std::string& error)
    {
        vulkanContext_ =
            std::make_unique<qtav::android_player::AndroidVulkanContext>();
        if (!vulkanContext_->create(
                window_,
                useHardwareZeroCopy,
                false,
                error)) {
            return false;
        }

        const qtav::VulkanOutputPreference outputPreference =
            options_.hdr
            ? qtav::VulkanOutputPreference::PreferHdr
            : qtav::VulkanOutputPreference::SdrOnly;
        vulkanRenderer_ =
            std::make_shared<qtav::AndroidVulkanVideoRenderer>(
                vulkanContext_->borrowed(),
                outputPreference);
        const std::uint64_t generation = playerGeneration_.load();
        vulkanRenderer_->setEventCallback(
            [this, generation](const qtav::VideoRenderEvent& event) {
                if (playerGeneration_.load() != generation) {
                    return;
                }
                if (event.type
                    == qtav::VideoRenderEventType::RedrawRequested) {
                    requestRender(generation);
                    return;
                }
                const std::string message =
                    "Vulkan renderer: " + event.detail;
                if (setPipelineErrorOnce(message)) {
                    logMessage(ANDROID_LOG_ERROR, message);
                }
            });

        if (useHardwareZeroCopy) {
            qtav::MediaCodecVulkanInteropConfig interopConfig;
            interopConfig.maximumImages = 8;
            interopConfig.androidHardwareBufferExternalMemoryEnabled = true;
            interopConfig.externalSemaphoreFdEnabled = true;
            interopConfig.samplerYcbcrConversionEnabled = true;
            interopConfig.foreignQueueFamilyEnabled = true;
            vulkanInterop_ =
                std::make_shared<qtav::MediaCodecVulkanInterop>(
                    vulkanContext_->borrowed().device,
                    interopConfig);
            if (!*vulkanInterop_) {
                error = "Could not create MediaCodec Vulkan ZeroCopy: "
                    + vulkanInterop_->lastError();
                return false;
            }
            vulkanRenderer_->setHardwareFrameInterop(vulkanInterop_);
            qtav::MediaCodecHardwareDecodeOptions decodeOptions;
            decodeOptions.allowSoftwareFallback = false;
            decodeOptions.extraHardwareFrames = 6;
            hardwareConfig = qtav::mediaCodecHardwareDecodeConfig(
                vulkanInterop_->surface(),
                decodeOptions);
        }

        if (!vulkanRenderer_->setWindow(window_)) {
            error = "Could not bind Vulkan to the Android Surface";
            return false;
        }
        qtav::VideoRenderConfig renderConfig;
        renderConfig.surfaceSize = vulkanRenderer_->surfaceSize();
        renderConfig.aspectRatio = qtav::VideoAspectRatioMode::Fit;
        renderConfig.rotation = static_cast<qtav::VideoRotation>(
            displayRotation_);
        if (renderConfig.surfaceSize.width <= 0
            || renderConfig.surfaceSize.height <= 0
            || !vulkanRenderer_->open(renderConfig)) {
            error = "Could not open the Android Vulkan renderer";
            return false;
        }
        renderer_ = vulkanRenderer_;
        std::string mode = options_.hardwareDecode
            ? "MediaCodec → AImageReader → Vulkan ZeroCopy"
            : "software decode → Vulkan";
        mode += " · libplacebo";
        mode += vulkanRenderer_->hdrOutputActive()
            ? " · HDR output"
            : " · SDR output";
        setActiveMode(std::move(mode));
        return true;
    }

    bool createOpenGLRenderer(
        bool useHardwareZeroCopy,
        qtav::HardwareDecodeConfig& hardwareConfig,
        std::string& error)
    {
        const qtav::OpenGLOutputPreference outputPreference =
            options_.hdr
            ? qtav::OpenGLOutputPreference::PreferHdr
            : qtav::OpenGLOutputPreference::SdrOnly;
        openGLRenderer_ =
            std::make_shared<qtav::AndroidOpenGLVideoRenderer>(
                outputPreference);
        const std::uint64_t generation = playerGeneration_.load();
        openGLRenderer_->setEventCallback(
            [this, generation](const qtav::VideoRenderEvent& event) {
                if (playerGeneration_.load() != generation) {
                    return;
                }
                if (event.type
                    == qtav::VideoRenderEventType::RedrawRequested) {
                    requestRender(generation);
                    return;
                }
                const std::string message =
                    "OpenGL ES renderer: " + event.detail;
                if (setPipelineErrorOnce(message)) {
                    logMessage(ANDROID_LOG_ERROR, message);
                }
            });

        if (useHardwareZeroCopy) {
            qtav::MediaCodecOpenGLInteropConfig interopConfig;
            interopConfig.javaVM = javaVM_;
            interopConfig.maximumPendingFrames = 4;
            openGLInterop_ =
                std::make_shared<qtav::MediaCodecOpenGLInterop>(
                    interopConfig);
            if (!*openGLInterop_) {
                error = "Could not create MediaCodec OpenGL ZeroCopy: "
                    + openGLInterop_->lastError();
                return false;
            }
            openGLRenderer_->setHardwareFrameInterop(openGLInterop_);
            qtav::MediaCodecHardwareDecodeOptions decodeOptions;
            decodeOptions.allowSoftwareFallback = false;
            decodeOptions.extraHardwareFrames = 6;
            hardwareConfig = qtav::mediaCodecHardwareDecodeConfig(
                openGLInterop_->surface(),
                decodeOptions);
        }

        if (!openGLRenderer_->setWindow(window_)) {
            error = "Could not bind OpenGL ES to the Android Surface";
            return false;
        }
        qtav::VideoRenderConfig renderConfig;
        renderConfig.surfaceSize = openGLRenderer_->surfaceSize();
        renderConfig.aspectRatio = qtav::VideoAspectRatioMode::Fit;
        if (renderConfig.surfaceSize.width <= 0
            || renderConfig.surfaceSize.height <= 0
            || !openGLRenderer_->open(renderConfig)) {
            error = "Could not open the Android OpenGL ES renderer";
            return false;
        }
        renderer_ = openGLRenderer_;
        std::string mode = options_.hardwareDecode
            ? "MediaCodec → AImageReader/AHardwareBuffer → EGLImage raw YCbCr → OpenGL ES ZeroCopy"
            : "software decode → OpenGL ES";
        mode += " · libplacebo";
        mode += openGLRenderer_->hdrOutputActive()
            ? " · HDR output"
            : " · SDR output";
        setActiveMode(std::move(mode));
        return true;
    }

    bool configurePipeline(
        const std::shared_ptr<qtav::Player>& player,
        std::string& error)
    {
        audioSink_ = std::make_shared<qtav::AAudioAudioSink>();
        audioConverter_ =
            std::make_shared<qtav::SwresampleAudioConverter>();
        player
            ->setAudioFrameConverter(audioConverter_)
            .setAudioSink(audioSink_);

        qtav::HardwareDecodeConfig hardwareConfig;
        if (options_.hardwareDecode && !options_.zeroCopy) {
            directSurface_ = qtav::MediaCodecSurface(window_);
            if (!directSurface_) {
                error = "Could not create the MediaCodec direct Surface";
                return false;
            }
            qtav::MediaCodecHardwareDecodeOptions decodeOptions;
            decodeOptions.allowSoftwareFallback = false;
            decodeOptions.extraHardwareFrames = 6;
            hardwareConfig = qtav::mediaCodecHardwareDecodeConfig(
                directSurface_,
                decodeOptions);
            directEnabled_.store(true);
            setActiveMode(
                "MediaCodec direct Surface · codec dataspace passthrough");
        } else {
            const bool hardwareZeroCopy =
                options_.hardwareDecode && options_.zeroCopy;
            const bool rendererCreated = options_.vulkan
                ? createVulkanRenderer(
                    hardwareZeroCopy,
                    hardwareConfig,
                    error)
                : createOpenGLRenderer(
                    hardwareZeroCopy,
                    hardwareConfig,
                    error);
            if (!rendererCreated) {
                return false;
            }
            player->setVideoRenderAPI(renderer_);
            renderEnabled_.store(true);
            if (hardwareZeroCopy
                && (vulkanInterop_ || openGLInterop_)) {
                const auto scheduledVulkanInterop = vulkanInterop_;
                const auto scheduledOpenGLInterop = openGLInterop_;
                player->setVideoFrameScheduler(
                    [this,
                     generation = playerGeneration_.load(),
                     scheduledVulkanInterop,
                     scheduledOpenGLInterop](
                        const qtav::VideoFrame& frame,
                        int,
                        std::int64_t monotonicNanoseconds) {
                        if (playerGeneration_.load() != generation
                            || !renderEnabled_.load()
                            || (!scheduledVulkanInterop
                                && !scheduledOpenGLInterop)) {
                            return false;
                        }
                        if (!reserveScheduledRenderFrame(generation)) {
                            return false;
                        }
                        std::string detail;
                        const bool queued = scheduledVulkanInterop
                            ? scheduledVulkanInterop->queueFrame(
                                  frame,
                                  detail)
                            : scheduledOpenGLInterop->queueFrame(
                                  frame,
                                  detail);
                        if (!queued) {
                            cancelScheduledRenderFrame();
                            return false;
                        }
                        decodedVideoFrames_.fetch_add(1);
                        observeDolbyVisionFrame(frame);
                        observeVideoSize(frame.width(), frame.height());
                        latestVideoTimestamp_.store(frame.timestamp());
                        observeVideoCallback(
                            frame.timestamp(),
                            generation);
                        commitScheduledRenderFrame(
                            frame,
                            generation,
                            monotonicNanoseconds);
                        return true;
                    });
            }
            // Scheduled hardware frames already retain the exact frame in the
            // bounded render queue. AImageReader completion wakes the render
            // thread through the interop callback; no core redraw callback is
            // needed for the same frame. Software/fallback frames continue to
            // enter through onVideoFrame().
            player->setRenderCallback({});
        }
        player->setHardwareDecodeConfig(std::move(hardwareConfig));
        return true;
    }

    void rebuildPlayer(std::int64_t startPosition, bool startPlaying)
    {
        releasePlayerAndPipeline();
        if (!window_ || mediaPath_.empty()) {
            return;
        }

        {
            std::lock_guard<std::mutex> lock(statusMutex_);
            pipelineError_.clear();
        }
        unsupportedSoftwareFrame_.store(false);
        {
            std::lock_guard<std::mutex> lock(outputMutex_);
            directOutputColorSpace_.clear();
        }

        auto nextPlayer = std::make_shared<qtav::Player>();
        const std::uint64_t generation = playerGeneration_.load();
        bindCallbacks(nextPlayer, generation);
        if (mediaPath_.compare(0, 7, "http://") == 0
            || mediaPath_.compare(0, 8, "https://") == 0) {
            applyRemoteTlsOptions(*nextPlayer);
        }
        std::string error;
        if (!configurePipeline(nextPlayer, error)) {
            setStatus(error);
            logMessage(ANDROID_LOG_ERROR, error);
            releasePipelineResources();
            return;
        }
        player_ = nextPlayer;
        activateRenderer(
            renderer_,
            vulkanInterop_,
            openGLInterop_,
            generation);
        duration_.store(0);
        decodedVideoFrames_.store(0);
        dolbyVisionFrames_.store(0);
        latestVideoTimestamp_.store(InvalidTimestamp);
        renderAttempts_.store(0);
        renderedVideoFrames_.store(0);
        renderQueueDrops_.store(0);
        timedRenderCount_.store(0);
        totalRenderMicroseconds_.store(0);
        maximumRenderMicroseconds_.store(0);
        directPresentedFrames_.store(0);
        hardwareBufferImports_.store(0);
        hardwareBufferImportCacheHits_.store(0);
        maximumCachedHardwareBufferImports_.store(0);
        unconvertedYcbcrImports_.store(0);
        interopCodecOutputsQueued_.store(0);
        interopImagesAcquired_.store(0);
        interopImagesImported_.store(0);
        interopStaleImagesDropped_.store(0);
        interopMaximumPendingImages_.store(0);
        openGLHdrSamplingStatus_.store(static_cast<int>(
            qtav::MediaCodecOpenGLHdrSamplingStatus::Disabled));
        openGLHardwareBufferFormat_.store(0);
        openGLHdrSupportLogged_.store(false);
        resetPresentationTiming(generation);
        setStatus(
            "Opening " + mediaLabel_ + " · " + activeMode());

        if (mediaDescriptor_ >= 0) {
            // FFmpeg's fd protocol duplicates the already-authorized SAF
            // descriptor instead of reopening its /proc/self/fd symlink,
            // which Android storage policy may reject. Rewind it before each
            // pipeline rebuild because dup() shares the file offset.
            lseek(mediaDescriptor_, 0, SEEK_SET);
            player_->setProperty(
                "avformat.fd",
                std::to_string(mediaDescriptor_));
        }
        player_->setMedia(mediaPath_);
        if (startPosition > 0) {
            std::weak_ptr<qtav::Player> weakPlayer = player_;
            player_->prepare(
                startPosition,
                [weakPlayer, startPlaying](
                    std::int64_t,
                    bool*) {
                    if (startPlaying) {
                        if (auto preparedPlayer = weakPlayer.lock()) {
                            preparedPlayer->setState(
                                qtav::State::Playing);
                        }
                    }
                });
        } else if (startPlaying) {
            player_->setState(qtav::State::Playing);
        } else {
            player_->prepare();
        }
    }

    void presentDirectSurfaceFrame(
        const qtav::VideoFrame& frame,
        std::uint64_t generation)
    {
        if (!directEnabled_.load()) {
            return;
        }
        std::lock_guard<std::mutex> lock(directMutex_);
        if (!directEnabled_.load() || !directSurface_) {
            return;
        }
        qtav::MediaCodecFrame output =
            qtav::mediaCodecFrame(frame, directSurface_);
        const bool presented = output && output.present();
        if (presented) {
            recordPresentation(frame, generation);
            const std::uint64_t presented =
                directPresentedFrames_.fetch_add(1) + 1;
            if (presented == 1) {
                logMessage(
                    ANDROID_LOG_INFO,
                    "First MediaCodec frame presented to the Surface");
            }
        } else {
            const std::string message =
                "MediaCodec could not present a direct-Surface frame";
            if (setPipelineErrorOnce(message)) {
                logMessage(ANDROID_LOG_ERROR, message);
            }
        }
    }

    void releasePlayerAndPipeline()
    {
        playerGeneration_.fetch_add(1);
        renderEnabled_.store(false);
        directEnabled_.store(false);
        deactivateRenderer();
        if (player_) {
            player_->setRenderCallback({});
            player_->setVideoFrameScheduler({});
            player_->onVideoFrame({});
        }
        if (vulkanInterop_) {
            vulkanInterop_->flush();
        }
        if (openGLInterop_) {
            openGLInterop_->flush();
        }
        if (player_) {
            player_->setState(qtav::State::Stopped);
            player_->waitFor(qtav::State::Stopped, 5'000);
        }
        {
            std::lock_guard<std::mutex> lock(renderMutex_);
        }
        {
            std::lock_guard<std::mutex> lock(directMutex_);
            directSurface_ = {};
        }
        if (player_) {
            player_
                ->onStateChanged({})
                .onMediaStatus({})
                .onEvent({})
                .onAudioFrame({})
                .setVideoRenderAPI({})
                .setAudioSink({})
                .setAudioFrameConverter({});
            player_.reset();
        }
        releasePipelineResources();
    }

    void activateRenderer(
        const std::shared_ptr<qtav::VideoRenderAPI>& renderer,
        const std::shared_ptr<qtav::MediaCodecVulkanInterop>& vulkanInterop,
        const std::shared_ptr<qtav::MediaCodecOpenGLInterop>& openGLInterop,
        std::uint64_t generation)
    {
        std::lock_guard<std::mutex> lock(renderScheduleMutex_);
        scheduledRenderer_ = renderer;
        scheduledVulkanInterop_ = vulkanInterop;
        scheduledOpenGLInterop_ = openGLInterop;
        renderGeneration_ = generation;
        renderFrames_.clear();
        pendingRenderFrames_.clear();
        scheduledRenderDeadlines_.clear();
        scheduledRenderReservations_ = 0;
        scheduledRenderFramesInFlight_ = 0;
        renderFrameQueued_ = false;
        renderRedrawRequested_ = false;
    }

    void clearScheduledRenderFrames()
    {
        {
            std::lock_guard<std::mutex> lock(renderScheduleMutex_);
            renderFrames_.clear();
            pendingRenderFrames_.clear();
            scheduledRenderDeadlines_.clear();
            scheduledRenderReservations_ = 0;
            scheduledRenderFramesInFlight_ = 0;
            renderFrameQueued_ = false;
            renderRedrawRequested_ = false;
        }
        renderScheduleChanged_.notify_all();
    }

    void deactivateRenderer()
    {
        std::lock_guard<std::mutex> lock(renderScheduleMutex_);
        scheduledRenderer_.reset();
        scheduledVulkanInterop_.reset();
        scheduledOpenGLInterop_.reset();
        renderGeneration_ = 0;
        renderFrames_.clear();
        pendingRenderFrames_.clear();
        scheduledRenderDeadlines_.clear();
        scheduledRenderReservations_ = 0;
        scheduledRenderFramesInFlight_ = 0;
        renderFrameQueued_ = false;
        renderRedrawRequested_ = false;
        renderScheduleChanged_.notify_all();
    }

    void enqueueRenderFrame(
        const qtav::VideoFrame& frame,
        std::uint64_t generation)
    {
        if (!frame || !renderEnabled_.load()
            || unsupportedSoftwareFrame_.load()
            || playerGeneration_.load() != generation) {
            return;
        }
        {
            std::unique_lock<std::mutex> lock(renderScheduleMutex_);
            renderScheduleChanged_.wait(lock, [this, generation] {
                return renderThreadStopping_
                    || renderGeneration_ != generation
                    || !scheduledRenderer_
                    || renderFrames_.size()
                            + pendingRenderFrames_.size()
                            + scheduledRenderReservations_
                            + scheduledRenderFramesInFlight_
                        < MaximumQueuedRenderFrames;
            });
            if (renderThreadStopping_
                || renderGeneration_ != generation
                || !scheduledRenderer_) {
                return;
            }
            // Keep the reference-counted MediaCodec output token associated
            // with the exact frame that was released to AImageReader. The
            // player's current frame may advance before the asynchronous
            // image-arrival callback asks us to retry.
            renderFrames_.push_back(frame);
            renderFrameQueued_ = true;
        }
        renderScheduleChanged_.notify_one();
    }

    bool reserveScheduledRenderFrame(std::uint64_t generation)
    {
        std::unique_lock<std::mutex> lock(renderScheduleMutex_);
        renderScheduleChanged_.wait(lock, [this, generation] {
            return renderThreadStopping_
                || renderGeneration_ != generation
                || !scheduledRenderer_
                || renderFrames_.size()
                        + pendingRenderFrames_.size()
                        + scheduledRenderReservations_
                        + scheduledRenderFramesInFlight_
                    < MaximumQueuedRenderFrames;
        });
        if (renderThreadStopping_
            || renderGeneration_ != generation
            || !scheduledRenderer_) {
            return false;
        }
        ++scheduledRenderReservations_;
        return true;
    }

    void cancelScheduledRenderFrame()
    {
        {
            std::lock_guard<std::mutex> lock(renderScheduleMutex_);
            if (scheduledRenderReservations_ > 0) {
                --scheduledRenderReservations_;
            }
        }
        renderScheduleChanged_.notify_all();
    }

    void commitScheduledRenderFrame(
        const qtav::VideoFrame& frame,
        std::uint64_t generation,
        std::int64_t monotonicNanoseconds)
    {
        {
            std::lock_guard<std::mutex> lock(renderScheduleMutex_);
            if (scheduledRenderReservations_ > 0) {
                --scheduledRenderReservations_;
            }
            if (!renderThreadStopping_
                && renderGeneration_ == generation
                && scheduledRenderer_) {
                scheduledRenderDeadlines_[frame.timestamp()] =
                    monotonicNanoseconds;
                renderFrames_.push_back(frame);
                renderFrameQueued_ = true;
            }
        }
        renderScheduleChanged_.notify_all();
    }

    void requestRender(std::uint64_t generation)
    {
        if (!renderEnabled_.load()
            || unsupportedSoftwareFrame_.load()
            || playerGeneration_.load() != generation) {
            return;
        }
        {
            std::lock_guard<std::mutex> lock(renderScheduleMutex_);
            if (renderThreadStopping_
                || renderGeneration_ != generation
                || !scheduledRenderer_) {
                return;
            }
            renderRedrawRequested_ = true;
        }
        renderScheduleChanged_.notify_one();
    }

    void runRenderThread()
    {
        for (;;) {
            std::shared_ptr<qtav::VideoRenderAPI> renderer;
            std::shared_ptr<qtav::MediaCodecVulkanInterop> vulkanInterop;
            std::shared_ptr<qtav::MediaCodecOpenGLInterop> openGLInterop;
            std::uint64_t generation = 0;
            bool retryPending = false;
            bool startQueuedFrame = false;
            {
                std::unique_lock<std::mutex> lock(renderScheduleMutex_);
                renderScheduleChanged_.wait(lock, [this] {
                    return renderThreadStopping_
                        || renderFrameQueued_
                        || !renderFrames_.empty()
                        || renderRedrawRequested_;
                });
                if (renderThreadStopping_) {
                    return;
                }
                retryPending = renderRedrawRequested_;
                startQueuedFrame = renderFrameQueued_
                    || !renderFrames_.empty();
                renderRedrawRequested_ = false;
                renderFrameQueued_ = false;
                renderer = scheduledRenderer_;
                vulkanInterop = scheduledVulkanInterop_;
                openGLInterop = scheduledOpenGLInterop_;
                generation = renderGeneration_;
            }

            enum class AttemptResult {
                Discarded,
                Pending,
                Presented,
            };
            const auto attemptFrame =
                [this, &renderer, &vulkanInterop, &openGLInterop, generation](
                    qtav::VideoFrame& frame) {
                if (!renderEnabled_.load()
                    || unsupportedSoftwareFrame_.load()
                    || playerGeneration_.load() != generation
                    || !renderer || !frame) {
                    return AttemptResult::Discarded;
                }

                {
                    std::unique_lock<std::mutex> lock(
                        renderScheduleMutex_);
                    const auto scheduled =
                        scheduledRenderDeadlines_.find(
                            frame.timestamp());
                    if (scheduled
                        != scheduledRenderDeadlines_.end()) {
                        const auto deadline =
                            std::chrono::steady_clock::time_point(
                                std::chrono::duration_cast<
                                    std::chrono::steady_clock::duration>(
                                    std::chrono::nanoseconds(
                                        scheduled->second)));
                        renderScheduleChanged_.wait_until(
                            lock,
                            deadline,
                            [this, generation] {
                                return renderThreadStopping_
                                    || renderGeneration_ != generation;
                            });
                        if (renderThreadStopping_
                            || renderGeneration_ != generation) {
                            return AttemptResult::Discarded;
                        }
                    }
                }

                bool rendered = false;
                const auto renderStart =
                    std::chrono::steady_clock::now();
                {
                    std::lock_guard<std::mutex> renderLock(
                        renderMutex_);
                    if (!renderEnabled_.load()
                        || unsupportedSoftwareFrame_.load()
                        || playerGeneration_.load() != generation) {
                        return AttemptResult::Discarded;
                    }
                    renderAttempts_.fetch_add(1);
                    rendered = renderer->render(frame);
                }

                if (!rendered) {
                    return renderEnabled_.load()
                        ? AttemptResult::Pending
                        : AttemptResult::Discarded;
                }
                const auto renderMicroseconds =
                    static_cast<std::uint64_t>(
                        std::max<std::int64_t>(
                            0,
                            std::chrono::duration_cast<
                                std::chrono::microseconds>(
                                std::chrono::steady_clock::now()
                                - renderStart)
                                .count()));
                timedRenderCount_.fetch_add(1);
                totalRenderMicroseconds_.fetch_add(renderMicroseconds);
                auto maximum = maximumRenderMicroseconds_.load();
                while (maximum < renderMicroseconds
                       && !maximumRenderMicroseconds_
                               .compare_exchange_weak(
                                   maximum,
                                   renderMicroseconds)) {
                }

                {
                    std::lock_guard<std::mutex> lock(
                        renderScheduleMutex_);
                    scheduledRenderDeadlines_.erase(
                        frame.timestamp());
                }

                recordPresentation(frame, generation);
                if (vulkanInterop) {
                    const auto statistics = vulkanInterop->statistics();
                    hardwareBufferImports_.store(
                        statistics.hardwareBufferImports);
                    hardwareBufferImportCacheHits_.store(
                        statistics.hardwareBufferImportCacheHits);
                    maximumCachedHardwareBufferImports_.store(
                        statistics.maximumCachedHardwareBufferImports);
                    unconvertedYcbcrImports_.store(
                        statistics.unconvertedYcbcrImports);
                    interopCodecOutputsQueued_.store(
                        statistics.codecOutputsQueued);
                    interopImagesAcquired_.store(
                        statistics.imagesAcquired);
                    interopImagesImported_.store(
                        statistics.imagesImported);
                    interopStaleImagesDropped_.store(
                        statistics.staleImagesDropped);
                    interopMaximumPendingImages_.store(
                        statistics.maximumPendingImages);
                }
                if (openGLInterop) {
                    const auto statistics = openGLInterop->statistics();
                    interopCodecOutputsQueued_.store(
                        statistics.codecOutputsQueued);
                    interopImagesAcquired_.store(
                        statistics.imagesLatched);
                    interopImagesImported_.store(
                        statistics.textureAttachments);
                    unconvertedYcbcrImports_.store(
                        statistics.rawYcbcrImports);
                    interopStaleImagesDropped_.store(
                        statistics.staleFramesDropped);
                    interopMaximumPendingImages_.store(
                        statistics.maximumPendingFrames);
                    openGLHdrSamplingStatus_.store(
                        static_cast<int>(statistics.hdrSamplingStatus));
                    openGLHardwareBufferFormat_.store(
                        statistics.lastHardwareBufferFormat);
                    if (statistics.hdrSamplingStatus
                            == qtav::MediaCodecOpenGLHdrSamplingStatus::Supported
                        && !openGLHdrSupportLogged_.exchange(true)) {
                        logMessage(
                            ANDROID_LOG_INFO,
                            "OpenGL raw AHardwareBuffer/EGLImage YCbCr path active; format "
                                + std::to_string(
                                    statistics.lastHardwareBufferFormat));
                    }
                }
                const std::uint64_t renderedCount =
                    renderedVideoFrames_.fetch_add(1) + 1;
                if (renderedCount == 1) {
                    logMessage(
                        ANDROID_LOG_INFO,
                        "First video frame rendered and presented");
                }
                return AttemptResult::Presented;
            };

            // A codec output has already been released once it enters the
            // pending queue. If its AImage never correlates, do not let that
            // stale token occupy every pipeline slot or get retried after the
            // interop's 250 ms correlation window has retired it.
            std::uint64_t stalePendingFrames = 0;
            {
                const std::int64_t latest =
                    latestVideoTimestamp_.load();
                std::lock_guard<std::mutex> lock(
                    renderScheduleMutex_);
                pendingRenderFrames_.erase(
                    std::remove_if(
                        pendingRenderFrames_.begin(),
                        pendingRenderFrames_.end(),
                        [latest, &stalePendingFrames](
                            const qtav::VideoFrame& frame) {
                            const bool stale = frame
                                && latest > frame.timestamp()
                                && latest - frame.timestamp()
                                    > MaximumPendingRenderAgeMilliseconds;
                            if (stale) {
                                ++stalePendingFrames;
                            }
                            return stale;
                        }),
                    pendingRenderFrames_.end());
            }
            if (stalePendingFrames > 0) {
                renderQueueDrops_.fetch_add(stalePendingFrames);
                renderScheduleChanged_.notify_all();
            }

            // AImageReader may coalesce notifications, so retry every exact
            // released output that was pending when this redraw arrived once.
            // Do not do this for a decoder callback alone: its image normally
            // does not exist yet.
            if (retryPending) {
                std::size_t pendingAttempts = 0;
                {
                    std::lock_guard<std::mutex> lock(
                        renderScheduleMutex_);
                    pendingAttempts = pendingRenderFrames_.size();
                }
                while (pendingAttempts > 0) {
                    --pendingAttempts;
                    qtav::VideoFrame frame;
                    {
                        std::lock_guard<std::mutex> lock(
                            renderScheduleMutex_);
                        if (renderGeneration_ != generation
                            || scheduledRenderer_ != renderer
                            || pendingRenderFrames_.empty()) {
                            break;
                        }
                        frame = std::move(
                            pendingRenderFrames_.front());
                        pendingRenderFrames_.pop_front();
                        ++scheduledRenderFramesInFlight_;
                    }
                    const AttemptResult result = attemptFrame(frame);
                    {
                        std::lock_guard<std::mutex> lock(
                            renderScheduleMutex_);
                        if (scheduledRenderFramesInFlight_ > 0) {
                            --scheduledRenderFramesInFlight_;
                        }
                        if (renderGeneration_ == generation
                            && scheduledRenderer_ == renderer
                            && result == AttemptResult::Pending) {
                            pendingRenderFrames_.push_back(
                                std::move(frame));
                        } else if (frame) {
                            scheduledRenderDeadlines_.erase(
                                frame.timestamp());
                        }
                    }
                    renderScheduleChanged_.notify_all();
                }
            }

            if (startQueuedFrame) {
                qtav::VideoFrame frame;
                bool removedQueuedFrame = false;
                {
                    std::lock_guard<std::mutex> lock(
                        renderScheduleMutex_);
                    if (renderGeneration_ == generation
                        && scheduledRenderer_ == renderer
                        && !renderFrames_.empty()
                        && pendingRenderFrames_.size()
                            < MaximumPendingRenderFrames) {
                        frame = std::move(renderFrames_.front());
                        renderFrames_.pop_front();
                        ++scheduledRenderFramesInFlight_;
                        removedQueuedFrame = true;
                    }
                }
                const AttemptResult result = frame
                    ? attemptFrame(frame)
                    : AttemptResult::Discarded;
                if (removedQueuedFrame) {
                    std::lock_guard<std::mutex> lock(
                        renderScheduleMutex_);
                    if (scheduledRenderFramesInFlight_ > 0) {
                        --scheduledRenderFramesInFlight_;
                    }
                    if (renderGeneration_ == generation
                        && scheduledRenderer_ == renderer
                        && result == AttemptResult::Pending
                        && pendingRenderFrames_.size()
                            < MaximumPendingRenderFrames) {
                        pendingRenderFrames_.push_back(
                            std::move(frame));
                    } else if (frame) {
                        scheduledRenderDeadlines_.erase(
                            frame.timestamp());
                    }
                }
                if (removedQueuedFrame) {
                    renderScheduleChanged_.notify_all();
                }
            }
        }
    }

    void stopRenderThread()
    {
        {
            std::lock_guard<std::mutex> lock(renderScheduleMutex_);
            renderThreadStopping_ = true;
            renderFrameQueued_ = false;
            renderRedrawRequested_ = false;
            scheduledRenderer_.reset();
            scheduledVulkanInterop_.reset();
            scheduledOpenGLInterop_.reset();
            renderFrames_.clear();
            pendingRenderFrames_.clear();
            scheduledRenderDeadlines_.clear();
            scheduledRenderReservations_ = 0;
            scheduledRenderFramesInFlight_ = 0;
        }
        renderScheduleChanged_.notify_all();
        if (renderThread_.joinable()) {
            renderThread_.join();
        }
    }

    void releasePipelineResources()
    {
        if (vulkanRenderer_) {
            vulkanRenderer_->close();
        }
        if (openGLRenderer_) {
            openGLRenderer_->close();
        }
        renderer_.reset();
        vulkanRenderer_.reset();
        openGLRenderer_.reset();
        vulkanInterop_.reset();
        openGLInterop_.reset();
        audioSink_.reset();
        audioConverter_.reset();
        vulkanContext_.reset();
        setActiveMode({});
    }

    std::int64_t currentPositionLocked() const
    {
        return player_
            ? std::max<std::int64_t>(0, player_->position())
            : savedPosition_;
    }

    std::int64_t resumePositionLocked() const
    {
        const std::int64_t observedVideoTimestamp =
            latestVideoTimestamp_.load();
        const std::int64_t latestVideoPosition =
            observedVideoTimestamp == InvalidTimestamp
            ? 0
            : observedVideoTimestamp;
        return std::max({
            currentPositionLocked(),
            savedPosition_,
            latestVideoPosition,
        });
    }

    void closeMediaDescriptor() noexcept
    {
        if (mediaDescriptor_ >= 0) {
            close(mediaDescriptor_);
            mediaDescriptor_ = -1;
        }
    }

    void setStatus(std::string status)
    {
        std::lock_guard<std::mutex> lock(statusMutex_);
        status_ = std::move(status);
    }

    void setPlaybackStatus(const char* prefix)
    {
        std::lock_guard<std::mutex> lock(statusMutex_);
        if (!pipelineError_.empty()) {
            status_ = pipelineError_;
            return;
        }
        status_ = prefix ? prefix : "";
        if (!activeMode_.empty()) {
            status_ += " · " + activeMode_;
        }
    }

    void setActiveMode(std::string mode)
    {
        std::lock_guard<std::mutex> lock(statusMutex_);
        activeMode_ = std::move(mode);
    }

    bool setPipelineErrorOnce(const std::string& message)
    {
        std::lock_guard<std::mutex> lock(statusMutex_);
        if (!pipelineError_.empty()) {
            return false;
        }
        pipelineError_ = message;
        status_ = message;
        return true;
    }

    std::string activeMode() const
    {
        std::lock_guard<std::mutex> lock(statusMutex_);
        return activeMode_;
    }

    JavaVM* javaVM_ = nullptr;
    mutable std::mutex commandMutex_;
    mutable std::mutex statusMutex_;
    mutable std::mutex outputMutex_;
    std::mutex renderMutex_;
    std::mutex directMutex_;
    std::mutex renderScheduleMutex_;
    mutable std::mutex timingMutex_;
    std::condition_variable renderScheduleChanged_;
    std::thread renderThread_;
    std::shared_ptr<qtav::VideoRenderAPI> scheduledRenderer_;
    std::shared_ptr<qtav::MediaCodecVulkanInterop>
        scheduledVulkanInterop_;
    std::shared_ptr<qtav::MediaCodecOpenGLInterop>
        scheduledOpenGLInterop_;
    std::deque<qtav::VideoFrame> renderFrames_;
    std::deque<qtav::VideoFrame> pendingRenderFrames_;
    std::unordered_map<std::int64_t, std::int64_t>
        scheduledRenderDeadlines_;
    std::size_t scheduledRenderReservations_ = 0;
    std::size_t scheduledRenderFramesInFlight_ = 0;
    std::uint64_t renderGeneration_ = 0;
    bool renderFrameQueued_ = false;
    bool renderRedrawRequested_ = false;
    bool renderThreadStopping_ = false;
    std::shared_ptr<qtav::Player> player_;
    ANativeWindow* window_ = nullptr;
    std::string directOutputColorSpace_;
    PlayerOptions options_;
    std::atomic<bool> userWantsPlaying_ { false };
    std::atomic<bool> renderEnabled_ { false };
    std::atomic<bool> directEnabled_ { false };
    std::atomic<std::int64_t> duration_ { 0 };
    std::atomic<std::uint64_t> videoSizePacked_ { 0 };
    std::atomic<std::uint64_t> playerGeneration_ { 0 };
    std::atomic<std::uint64_t> decodedVideoFrames_ { 0 };
    std::atomic<std::uint64_t> dolbyVisionFrames_ { 0 };
    std::atomic<std::int64_t> latestVideoTimestamp_ {
        InvalidTimestamp
    };
    std::atomic<std::uint64_t> renderAttempts_ { 0 };
    std::atomic<std::uint64_t> renderedVideoFrames_ { 0 };
    std::atomic<std::uint64_t> renderQueueDrops_ { 0 };
    std::atomic<std::uint64_t> timedRenderCount_ { 0 };
    std::atomic<std::uint64_t> totalRenderMicroseconds_ { 0 };
    std::atomic<std::uint64_t> maximumRenderMicroseconds_ { 0 };
    std::atomic<std::uint64_t> directPresentedFrames_ { 0 };
    std::atomic<std::uint64_t> hardwareBufferImports_ { 0 };
    std::atomic<std::uint64_t> hardwareBufferImportCacheHits_ { 0 };
    std::atomic<std::uint64_t>
        maximumCachedHardwareBufferImports_ { 0 };
    std::atomic<std::uint64_t> unconvertedYcbcrImports_ { 0 };
    std::atomic<std::uint64_t> interopCodecOutputsQueued_ { 0 };
    std::atomic<std::uint64_t> interopImagesAcquired_ { 0 };
    std::atomic<std::uint64_t> interopImagesImported_ { 0 };
    std::atomic<std::uint64_t> interopStaleImagesDropped_ { 0 };
    std::atomic<std::uint64_t> interopMaximumPendingImages_ { 0 };
    std::atomic<int> openGLHdrSamplingStatus_ {
        static_cast<int>(
            qtav::MediaCodecOpenGLHdrSamplingStatus::Disabled)
    };
    std::atomic<std::uint32_t> openGLHardwareBufferFormat_ { 0 };
    std::atomic<bool> openGLHdrSupportLogged_ { false };
    std::atomic<int> requestedFrameRateMilliHertz_ { 0 };
    std::atomic<int> frameRateRequestResult_ { 0 };
    std::atomic<std::uint64_t> sourceFramesSkipped_ { 0 };
    std::atomic<std::uint64_t> presentationStalls_ { 0 };
    std::atomic<std::uint64_t> presentationCatchups_ { 0 };
    std::atomic<std::uint64_t>
        maximumPresentationGapMilliseconds_ { 0 };
    std::atomic<bool> unsupportedSoftwareFrame_ { false };
    ANativeWindow* frameRateWindow_ = nullptr;
    std::uint64_t timingGeneration_ = 0;
    std::array<std::int64_t, FrameRateSampleCount> frameRateDeltas_ {};
    std::size_t frameRateDeltaCount_ = 0;
    std::int64_t lastCallbackTimestamp_ = InvalidTimestamp;
    std::int64_t lastPresentedTimestamp_ = InvalidTimestamp;
    std::chrono::steady_clock::time_point lastPresentationTime_;
    std::deque<std::chrono::steady_clock::time_point>
        presentationTimes_;
    int presentationFpsMilliHertz_ = 0;
    std::int64_t savedPosition_ = 0;
    int displayRotation_ = 0;
    int mediaDescriptor_ = -1;
    std::string mediaLabel_;
    std::string mediaPath_;
    std::string activeMode_;
    std::string status_;
    std::string pipelineError_;

    std::shared_ptr<qtav::VideoRenderAPI> renderer_;
    std::shared_ptr<qtav::AndroidVulkanVideoRenderer> vulkanRenderer_;
    std::shared_ptr<qtav::AndroidOpenGLVideoRenderer> openGLRenderer_;
    std::shared_ptr<qtav::MediaCodecVulkanInterop> vulkanInterop_;
    std::shared_ptr<qtav::MediaCodecOpenGLInterop> openGLInterop_;
    qtav::MediaCodecSurface directSurface_;
    std::shared_ptr<qtav::AAudioAudioSink> audioSink_;
    std::shared_ptr<qtav::SwresampleAudioConverter> audioConverter_;
    std::unique_ptr<qtav::android_player::AndroidVulkanContext>
        vulkanContext_;
};

AndroidPlayerController* controllerFromHandle(jlong handle) noexcept
{
    return reinterpret_cast<AndroidPlayerController*>(
        static_cast<std::uintptr_t>(handle));
}

jlong handleFromController(AndroidPlayerController* controller) noexcept
{
    return static_cast<jlong>(
        reinterpret_cast<std::uintptr_t>(controller));
}

} // namespace

extern "C" JNIEXPORT jint JNICALL JNI_OnLoad(JavaVM* javaVM, void*)
{
    GlobalJavaVM = javaVM;
    setenv("SSL_CERT_DIR", "/system/etc/security/cacerts", 0);
    return JNI_VERSION_1_6;
}

extern "C" JNIEXPORT jlong JNICALL
Java_org_qtav_core_player_QtAVPlayerActivity_nativeCreate(
    JNIEnv* environment,
    jobject,
    jstring caBundlePath)
{
    GlobalCaBundlePath = fromJavaString(environment, caBundlePath);
    logMessage(
        GlobalCaBundlePath.empty() ? ANDROID_LOG_WARN : ANDROID_LOG_INFO,
        GlobalCaBundlePath.empty()
            ? "Android system CA bundle is unavailable"
            : "Using Android system CA bundle: " + GlobalCaBundlePath);
    return handleFromController(
        new AndroidPlayerController(GlobalJavaVM));
}

extern "C" JNIEXPORT void JNICALL
Java_org_qtav_core_player_QtAVPlayerActivity_nativeDestroy(
    JNIEnv*,
    jobject,
    jlong handle)
{
    delete controllerFromHandle(handle);
}

extern "C" JNIEXPORT void JNICALL
Java_org_qtav_core_player_QtAVPlayerActivity_nativeSetSurface(
    JNIEnv* environment,
    jobject,
    jlong handle,
    jobject surface,
    jint displayRotation)
{
    AndroidPlayerController* controller = controllerFromHandle(handle);
    if (!controller) {
        return;
    }
    ANativeWindow* window = surface
        ? ANativeWindow_fromSurface(environment, surface)
        : nullptr;
    // ANativeWindow_fromSurface returns an acquired reference. The
    // controller takes ownership and releases it on replacement/destruction.
    controller->setSurface(window, displayRotation);
}

extern "C" JNIEXPORT void JNICALL
Java_org_qtav_core_player_QtAVPlayerActivity_nativeOpenMedia(
    JNIEnv* environment,
    jobject,
    jlong handle,
    jstring label,
    jstring path,
    jint descriptor)
{
    if (AndroidPlayerController* controller =
            controllerFromHandle(handle)) {
        controller->openMedia(
            fromJavaString(environment, label),
            fromJavaString(environment, path),
            descriptor);
    } else if (descriptor >= 0) {
        close(descriptor);
    }
}

extern "C" JNIEXPORT void JNICALL
Java_org_qtav_core_player_QtAVPlayerActivity_nativeSetOptions(
    JNIEnv*,
    jobject,
    jlong handle,
    jboolean vulkan,
    jboolean hdr,
    jboolean zeroCopy,
    jboolean hardwareDecode)
{
    if (AndroidPlayerController* controller =
            controllerFromHandle(handle)) {
        controller->setOptions({
            vulkan == JNI_TRUE,
            hdr == JNI_TRUE,
            zeroCopy == JNI_TRUE,
            hardwareDecode == JNI_TRUE,
        });
    }
}

extern "C" JNIEXPORT void JNICALL
Java_org_qtav_core_player_QtAVPlayerActivity_nativeTogglePlayback(
    JNIEnv*,
    jobject,
    jlong handle)
{
    if (AndroidPlayerController* controller =
            controllerFromHandle(handle)) {
        controller->togglePlayback();
    }
}

extern "C" JNIEXPORT void JNICALL
Java_org_qtav_core_player_QtAVPlayerActivity_nativeStop(
    JNIEnv*,
    jobject,
    jlong handle)
{
    if (AndroidPlayerController* controller =
            controllerFromHandle(handle)) {
        controller->stop();
    }
}

extern "C" JNIEXPORT void JNICALL
Java_org_qtav_core_player_QtAVPlayerActivity_nativeSeek(
    JNIEnv*,
    jobject,
    jlong handle,
    jlong position)
{
    if (AndroidPlayerController* controller =
            controllerFromHandle(handle)) {
        controller->seek(position);
    }
}

extern "C" JNIEXPORT jlong JNICALL
Java_org_qtav_core_player_QtAVPlayerActivity_nativeGetPosition(
    JNIEnv*,
    jobject,
    jlong handle)
{
    if (AndroidPlayerController* controller =
            controllerFromHandle(handle)) {
        return controller->position();
    }
    return 0;
}

extern "C" JNIEXPORT jlong JNICALL
Java_org_qtav_core_player_QtAVPlayerActivity_nativeGetDuration(
    JNIEnv*,
    jobject,
    jlong handle)
{
    if (AndroidPlayerController* controller =
            controllerFromHandle(handle)) {
        return controller->duration();
    }
    return 0;
}

extern "C" JNIEXPORT jlong JNICALL
Java_org_qtav_core_player_QtAVPlayerActivity_nativeGetVideoSize(
    JNIEnv*,
    jobject,
    jlong handle)
{
    if (AndroidPlayerController* controller =
            controllerFromHandle(handle)) {
        return static_cast<jlong>(controller->videoSizePacked());
    }
    return 0;
}

extern "C" JNIEXPORT jboolean JNICALL
Java_org_qtav_core_player_QtAVPlayerActivity_nativeIsPlaying(
    JNIEnv*,
    jobject,
    jlong handle)
{
    if (AndroidPlayerController* controller =
            controllerFromHandle(handle)) {
        return controller->isPlaying() ? JNI_TRUE : JNI_FALSE;
    }
    return JNI_FALSE;
}

extern "C" JNIEXPORT jint JNICALL
Java_org_qtav_core_player_QtAVPlayerActivity_nativeGetRequestedFrameRate(
    JNIEnv*,
    jobject,
    jlong handle)
{
    if (AndroidPlayerController* controller =
            controllerFromHandle(handle)) {
        return controller->requestedFrameRateMilliHertz();
    }
    return 0;
}

extern "C" JNIEXPORT jstring JNICALL
Java_org_qtav_core_player_QtAVPlayerActivity_nativeGetStatus(
    JNIEnv* environment,
    jobject,
    jlong handle)
{
    std::string status;
    if (AndroidPlayerController* controller =
            controllerFromHandle(handle)) {
        status = controller->status();
    }
    return environment->NewStringUTF(status.c_str());
}

extern "C" JNIEXPORT jstring JNICALL
Java_org_qtav_core_player_QtAVPlayerActivity_nativeGetOutputColorSpace(
    JNIEnv* environment,
    jobject,
    jlong handle)
{
    std::string colorSpace = "unavailable";
    if (AndroidPlayerController* controller =
            controllerFromHandle(handle)) {
        colorSpace = controller->outputColorSpace();
    }
    return environment->NewStringUTF(colorSpace.c_str());
}
