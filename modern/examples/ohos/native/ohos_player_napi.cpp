// SPDX-License-Identifier: LGPL-2.1-or-later

#include "ohos_vulkan_context.h"

#include <ace/xcomponent/native_interface_xcomponent.h>
#include <hilog/log.h>
#include <node_api.h>

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cmath>
#include <condition_variable>
#include <cstddef>
#include <cstdint>
#include <deque>
#include <memory>
#include <mutex>
#include <sstream>
#include <string>
#include <thread>
#include <utility>
#include <vector>

#include <unistd.h>

#include <qtav/atempo_audio_time_stretcher.h>
#include <qtav/mobile_video_renderer.h>
#include <qtav/ohcodec_hardware_decoder.h>
#include <qtav/ohcodec_opengl_interop.h>
#include <qtav/ohcodec_vulkan_interop.h>
#include <qtav/ohos_opengl_video_renderer.h>
#include <qtav/ohaudio_audio_sink.h>
#include <qtav/player.h>
#include <qtav/swresample_audio_converter.h>

namespace qtav::ohos_player {
namespace {

constexpr std::uint32_t LogDomain = 0xD003901;
constexpr const char* LogTag = "QtAVPlayerOHOS";
constexpr std::chrono::seconds FpsWindow { 2 };
constexpr std::chrono::seconds FpsStaleAfter { 2 };

enum class DecodePreference {
    Auto,
    Hardware,
    Software,
};

enum class RenderPreference {
    Auto,
    Vulkan,
    OpenGLES,
};

enum class HdrPreference {
    Auto,
    RequireHdr,
    Sdr,
};

const char* decodePreferenceName(DecodePreference value) noexcept
{
    switch (value) {
    case DecodePreference::Auto:
        return "auto";
    case DecodePreference::Hardware:
        return "hardware";
    case DecodePreference::Software:
        return "software";
    }
    return "auto";
}

const char* renderPreferenceName(RenderPreference value) noexcept
{
    switch (value) {
    case RenderPreference::Auto:
        return "auto";
    case RenderPreference::Vulkan:
        return "vulkan";
    case RenderPreference::OpenGLES:
        return "opengles";
    }
    return "auto";
}

const char* hdrPreferenceName(HdrPreference value) noexcept
{
    switch (value) {
    case HdrPreference::Auto:
        return "auto";
    case HdrPreference::RequireHdr:
        return "require-hdr";
    case HdrPreference::Sdr:
        return "sdr";
    }
    return "auto";
}

VulkanOutputPreference vulkanOutputPreference(
    HdrPreference value) noexcept
{
    switch (value) {
    case HdrPreference::RequireHdr:
        return VulkanOutputPreference::RequireHdr;
    case HdrPreference::Sdr:
        return VulkanOutputPreference::SdrOnly;
    case HdrPreference::Auto:
    default:
        return VulkanOutputPreference::PreferHdr;
    }
}

OpenGLOutputPreference openGLOutputPreference(
    HdrPreference value) noexcept
{
    switch (value) {
    case HdrPreference::RequireHdr:
        return OpenGLOutputPreference::RequireHdr;
    case HdrPreference::Sdr:
        return OpenGLOutputPreference::SdrOnly;
    case HdrPreference::Auto:
    default:
        return OpenGLOutputPreference::PreferHdr;
    }
}

const char* openGLColorSpaceName(
    OpenGLOutputColorSpace value) noexcept
{
    switch (value) {
    case OpenGLOutputColorSpace::HDR10PQ:
        return "BT.2020/PQ";
    case OpenGLOutputColorSpace::HDR10HLG:
        return "BT.2020/HLG";
    case OpenGLOutputColorSpace::SdrSrgb:
    default:
        return "sRGB";
    }
}

const char* vulkanColorSpaceName(VkColorSpaceKHR value) noexcept
{
    switch (value) {
    case VK_COLOR_SPACE_HDR10_ST2084_EXT:
        return "BT.2020/PQ";
    case VK_COLOR_SPACE_HDR10_HLG_EXT:
        return "BT.2020/HLG";
    case VK_COLOR_SPACE_BT2020_LINEAR_EXT:
        return "BT.2020/linear";
    case VK_COLOR_SPACE_EXTENDED_SRGB_LINEAR_EXT:
        return "extended-sRGB/linear";
    case VK_COLOR_SPACE_SRGB_NONLINEAR_KHR:
    default:
        return "sRGB";
    }
}

void logMessage(LogLevel level, const std::string& message)
{
    OH_LOG_Print(
        LOG_APP,
        level,
        LogDomain,
        LogTag,
        "%{public}s",
        message.c_str());
}

const char* stateName(State state) noexcept
{
    switch (state) {
    case State::Stopped:
        return "stopped";
    case State::Playing:
        return "playing";
    case State::Paused:
        return "paused";
    }
    return "unknown";
}

const char* statusName(MediaStatus status) noexcept
{
    switch (status) {
    case MediaStatus::NoMedia:
        return "no-media";
    case MediaStatus::Loading:
        return "loading";
    case MediaStatus::Loaded:
        return "loaded";
    case MediaStatus::Buffering:
        return "buffering";
    case MediaStatus::EndOfMedia:
        return "end-of-media";
    case MediaStatus::Invalid:
        return "invalid";
    }
    return "unknown";
}

std::string trackLabel(const TrackInfo& track)
{
    std::ostringstream result;
    result << (track.title.empty() ? "Track" : track.title);
    if (!track.language.empty() && track.language != "und") {
        result << " · " << track.language;
    }
    if (!track.codec.empty()) {
        result << " · " << track.codec;
    }
    if (track.type == MediaType::Audio && track.channels > 0) {
        result << " · " << track.channels << "ch";
    }
    if (track.external) {
        result << " · external";
    }
    return result.str();
}

struct PlayerSnapshot {
    bool surfaceReady = false;
    bool playing = false;
    bool seekable = false;
    std::string source;
    std::string state;
    std::string mediaStatus;
    std::string subtitle;
    std::string videoCodec;
    std::string audioCodec;
    std::string renderAPI;
    std::string renderPreference;
    std::string decodePreference;
    std::string hdrPreference;
    std::string decoderAPI;
    std::string inputColorSpace;
    std::string outputColorSpace;
    std::string outputFormat;
    std::string error;
    std::int64_t position = 0;
    std::int64_t duration = 0;
    double playbackRate = 1.0;
    double bufferingProgress = 1.0;
    double fps = 0.0;
    int width = 0;
    int height = 0;
    int activeAudioTrack = -1;
    int activeSubtitleTrack = -1;
    std::uint64_t decodedFrames = 0;
    std::uint64_t hardwareFrames = 0;
    std::uint64_t softwareFrames = 0;
    std::uint64_t renderedFrames = 0;
    std::uint64_t droppedFrames = 0;
    std::uint64_t opaqueExternalImports = 0;
    std::uint64_t externalFormatWorkaroundImports = 0;
    std::uint64_t externalNormalizationPasses = 0;
    std::uint64_t nativeBuffersAcquired = 0;
    std::uint64_t frameAvailableCallbacks = 0;
    std::uint64_t outputsReleasedAfterGpu = 0;
    std::int32_t lastVulkanSourceFormat = 0;
    std::uint64_t lastExternalFormat = 0;
    bool hdrInput = false;
    bool dolbyVisionInput = false;
    bool hdrOutput = false;
    bool toneMappedToSdr = false;
    std::vector<TrackInfo> audioTracks;
    std::vector<TrackInfo> subtitleTracks;
};

struct PlayerProgressSnapshot {
    bool playing = false;
    std::string mediaStatus;
    std::string subtitle;
    std::string error;
    std::int64_t position = 0;
    double bufferingProgress = 1.0;
};

class PlayerSession final {
public:
    PlayerSession()
    {
        audioSink_ = std::make_shared<OHAudioAudioSink>();
        player_.setProperty("avcodec.video.threads", "4");
        player_
            .setAudioFrameConverter(
                std::make_shared<SwresampleAudioConverter>())
            .setAudioTimeStretcher(
                std::make_shared<AtempoAudioTimeStretcher>())
            .setAudioSink(audioSink_)
            .onStateChanged([this](State state) {
                state_.store(state, std::memory_order_release);
            })
            .onMediaStatus(
                [this](MediaStatus, MediaStatus status) {
                    mediaStatus_.store(status, std::memory_order_release);
                    if (status == MediaStatus::EndOfMedia) {
                        wantsPlaying_.store(false, std::memory_order_release);
                    }
                    if (status == MediaStatus::Invalid) {
                        setError("QtAVCore could not open or decode the media");
                    }
                    return false;
                })
            .onPacketBufferStatus(
                [this](const PacketBufferStatus& status) {
                    bufferingProgressMilli_.store(
                        static_cast<int>(std::lround(
                            std::clamp(status.progress, 0.0, 1.0)
                            * 1'000.0)),
                        std::memory_order_release);
                })
            .onEvent([this](const MediaEvent& event) {
                const std::string message = event.category + ": "
                    + event.detail;
                logMessage(
                    event.error == 0 ? LOG_INFO : LOG_ERROR,
                    message);
                if (event.error != 0) {
                    setError(message);
                }
                return false;
            })
            .onVideoFrame([this](const VideoFrame& frame, int) {
                decodedFrames_.fetch_add(1, std::memory_order_relaxed);
                if (frame.hasHardwareFrame()
                    && frame.hardwareFrame().deviceType()
                        == HardwareDeviceType::OHCodec) {
                    hardwareFrames_.fetch_add(1, std::memory_order_relaxed);
                    std::lock_guard<std::mutex> lock(statusMutex_);
                    decoderAPI_ = "ohcodec";
                } else {
                    softwareFrames_.fetch_add(1, std::memory_order_relaxed);
                    std::lock_guard<std::mutex> lock(statusMutex_);
                    decoderAPI_ = "software";
                }
                const VideoColorSpace colorSpace = frame.colorSpaceInfo();
                if (colorSpace.isHdr()) {
                    hdrInput_.store(true, std::memory_order_relaxed);
                }
                if (frame.hasDolbyVisionMetadata()) {
                    dolbyVisionInput_.store(true, std::memory_order_relaxed);
                    hdrInput_.store(true, std::memory_order_relaxed);
                }
                const std::string inputColorSpace = frame.colorSpace();
                if (!inputColorSpace.empty()) {
                    std::lock_guard<std::mutex> lock(statusMutex_);
                    inputColorSpace_ = inputColorSpace;
                }
                if (frame.width() > 0 && frame.height() > 0) {
                    videoWidth_.store(
                        frame.width(),
                        std::memory_order_relaxed);
                    videoHeight_.store(
                        frame.height(),
                        std::memory_order_relaxed);
                }
            })
            .onSubtitleFrame(
                [this](const SubtitleFrame& frame, int) {
                    std::lock_guard<std::mutex> lock(subtitleMutex_);
                    subtitleText_ = frame.text();
                    subtitleStart_ = frame.timestamp();
                    subtitleEnd_ = frame.timestamp()
                        + std::max<std::int64_t>(
                            frame.duration(),
                            1'000);
                });

        renderWorker_ = std::thread([this] { renderLoop(); });
    }

    ~PlayerSession()
    {
        {
            std::lock_guard<std::mutex> lock(renderRequestMutex_);
            renderQuit_ = true;
            renderRequested_ = true;
        }
        renderCondition_.notify_all();
        if (renderWorker_.joinable()) {
            renderWorker_.join();
        }

        player_.setState(State::Stopped);
        player_
            .onStateChanged({})
            .onMediaStatus({})
            .onPacketBufferStatus({})
            .onEvent({})
            .onVideoFrame({})
            .onSubtitleFrame({})
            .setRenderCallback({})
            .setVideoRenderAPI({})
            .setAudioSink({})
            .setAudioTimeStretcher({})
            .setAudioFrameConverter({});
        {
            std::lock_guard<std::mutex> lock(pipelineMutex_);
            closeRendererLocked();
        }
        closeRetainedDescriptors();
    }

    void createSurface(
        OH_NativeXComponent* component,
        OHNativeWindow* window)
    {
        if (!component || !window) {
            setError("XComponent did not provide an OHNativeWindow");
            return;
        }

        std::uint64_t width = 0;
        std::uint64_t height = 0;
        if (OH_NativeXComponent_GetXComponentSize(
                component,
                window,
                &width,
                &height)
            != OH_NATIVEXCOMPONENT_RESULT_SUCCESS) {
            setError("Could not query the XComponent surface size");
            return;
        }

        std::lock_guard<std::mutex> lock(pipelineMutex_);
        closeRendererLocked();
        window_ = window;
        renderConfig_ = {};
        renderConfig_.surfaceSize = {
            static_cast<int>(width),
            static_cast<int>(height),
        };
        renderConfig_.aspectRatio = VideoAspectRatioMode::Fit;
        if (!createRendererLocked()) {
            window_ = nullptr;
            surfaceReady_.store(false, std::memory_order_release);
            return;
        }
        surfaceReady_.store(true, std::memory_order_release);
        if (wantsPlaying_.load(std::memory_order_acquire)
            && mediaStatus_.load(std::memory_order_acquire)
                != MediaStatus::NoMedia) {
            player_.setState(State::Playing);
        }
    }

    void changeSurface(
        OH_NativeXComponent* component,
        OHNativeWindow* window)
    {
        if (!component || !window) {
            return;
        }
        std::uint64_t width = 0;
        std::uint64_t height = 0;
        if (OH_NativeXComponent_GetXComponentSize(
                component,
                window,
                &width,
                &height)
            != OH_NATIVEXCOMPONENT_RESULT_SUCCESS) {
            return;
        }

        std::lock_guard<std::mutex> lock(pipelineMutex_);
        if (!selector_ || window != window_) {
            return;
        }
        renderConfig_.surfaceSize = {
            static_cast<int>(width),
            static_cast<int>(height),
        };
        renderConfig_.aspectRatio = VideoAspectRatioMode::Fit;
        selector_->suspendSurface();
        if (!selector_->configure(renderConfig_)
            || !selector_->recreateSurface()) {
            setError("Could not recreate the resized video surface");
            return;
        }
        if (!configureDecoderLocked()) {
            setError("Could not rebind the decoder to the resized surface");
            return;
        }
        requestRender();
    }

    void releaseSurface()
    {
        const bool shouldPause =
            wantsPlaying_.load(std::memory_order_acquire);
        if (shouldPause) {
            player_.setState(State::Paused);
        }
        std::lock_guard<std::mutex> lock(pipelineMutex_);
        closeRendererLocked();
        window_ = nullptr;
        surfaceReady_.store(false, std::memory_order_release);
    }

    bool openUrl(const std::string& url)
    {
        if (url.empty()) {
            setError("The media URL is empty");
            return false;
        }
        {
            std::lock_guard<std::mutex> lock(statusMutex_);
            source_ = url;
            error_.clear();
        }
        resetMediaObservations();
        wantsPlaying_.store(true, std::memory_order_release);
        player_.setMedia(url);
        player_.setState(State::Playing);
        return true;
    }

    bool openLocalDescriptor(int descriptor, const std::string& name)
    {
        if (descriptor < 0) {
            setError("The document picker returned an invalid descriptor");
            return false;
        }
        const int retained = dup(descriptor);
        if (retained < 0) {
            setError("Could not retain the selected local file");
            return false;
        }
        {
            std::lock_guard<std::mutex> lock(descriptorMutex_);
            retainedDescriptors_.push_back(retained);
        }
        {
            std::lock_guard<std::mutex> lock(statusMutex_);
            source_ = name.empty() ? "Local file" : name;
            error_.clear();
        }
        resetMediaObservations();
        wantsPlaying_.store(true, std::memory_order_release);
        player_.setMedia("/proc/self/fd/" + std::to_string(retained));
        player_.setState(State::Playing);
        return true;
    }

    void setPlaying(bool playing)
    {
        wantsPlaying_.store(playing, std::memory_order_release);
        player_.setState(playing ? State::Playing : State::Paused);
    }

    void stop()
    {
        wantsPlaying_.store(false, std::memory_order_release);
        clearSubtitle();
        player_.setState(State::Stopped);
        closeRetainedDescriptors();
    }

    bool seek(std::int64_t position)
    {
        clearSubtitle();
        return player_.seek(
            std::max<std::int64_t>(0, position),
            SeekFlag::Accurate);
    }

    bool setPlaybackRate(double rate)
    {
        if (!std::isfinite(rate) || rate < 0.25 || rate > 4.0) {
            setError("Playback rate must be between 0.25x and 4.0x");
            return false;
        }
        player_.setPlaybackRate(static_cast<float>(rate));
        return true;
    }

    bool setDecodePreference(const std::string& value)
    {
        DecodePreference preference;
        if (value == "auto") {
            preference = DecodePreference::Auto;
        } else if (value == "hardware") {
            preference = DecodePreference::Hardware;
        } else if (value == "software") {
            preference = DecodePreference::Software;
        } else {
            setError("Unknown decode preference: " + value);
            return false;
        }
        std::lock_guard<std::mutex> lock(pipelineMutex_);
        if (decodePreference_ == preference) {
            return true;
        }
        decodePreference_ = preference;
        return window_
            ? rebuildRendererLocked()
            : configureDecoderLocked();
    }

    bool setRenderPreference(const std::string& value)
    {
        RenderPreference preference;
        if (value == "auto") {
            preference = RenderPreference::Auto;
        } else if (value == "vulkan") {
            preference = RenderPreference::Vulkan;
        } else if (value == "opengles") {
            preference = RenderPreference::OpenGLES;
        } else {
            setError("Unknown renderer preference: " + value);
            return false;
        }
        std::lock_guard<std::mutex> lock(pipelineMutex_);
        if (renderPreference_ == preference) {
            return true;
        }
        renderPreference_ = preference;
        return rebuildRendererLocked();
    }

    bool setHdrPreference(const std::string& value)
    {
        HdrPreference preference;
        if (value == "auto") {
            preference = HdrPreference::Auto;
        } else if (value == "require-hdr") {
            preference = HdrPreference::RequireHdr;
        } else if (value == "sdr") {
            preference = HdrPreference::Sdr;
        } else {
            setError("Unknown HDR output preference: " + value);
            return false;
        }
        std::lock_guard<std::mutex> lock(pipelineMutex_);
        if (hdrPreference_ == preference) {
            return true;
        }
        hdrPreference_ = preference;
        return rebuildRendererLocked();
    }

    bool selectTrack(MediaType type, int selector)
    {
        if (type != MediaType::Audio && type != MediaType::Subtitle) {
            return false;
        }
        if (type == MediaType::Subtitle) {
            clearSubtitle();
        }
        const bool accepted = player_.setActiveTrack(type, selector);
        if (!accepted) {
            setError("QtAVCore rejected the requested track selection");
        }
        return accepted;
    }

    PlayerSnapshot snapshot() const
    {
        std::lock_guard<std::mutex> pipelineLock(pipelineMutex_);
        PlayerSnapshot result;
        result.surfaceReady =
            surfaceReady_.load(std::memory_order_acquire);
        result.playing = state_.load(std::memory_order_acquire)
            == State::Playing;
        result.state = stateName(state_.load(std::memory_order_acquire));
        result.mediaStatus = statusName(
            mediaStatus_.load(std::memory_order_acquire));
        result.position = std::max<std::int64_t>(0, player_.position());
        result.playbackRate = player_.playbackRate();
        result.bufferingProgress = static_cast<double>(
            bufferingProgressMilli_.load(std::memory_order_acquire))
            / 1'000.0;
        result.decodedFrames =
            decodedFrames_.load(std::memory_order_relaxed);
        result.hardwareFrames =
            hardwareFrames_.load(std::memory_order_relaxed);
        result.softwareFrames =
            softwareFrames_.load(std::memory_order_relaxed);
        result.renderedFrames =
            renderedFrames_.load(std::memory_order_relaxed);
        result.hdrInput = hdrInput_.load(std::memory_order_relaxed);
        result.dolbyVisionInput =
            dolbyVisionInput_.load(std::memory_order_relaxed);
        result.fps = presentationFps();

        const MediaInfo info = player_.mediaInfo();
        result.duration = std::max<std::int64_t>(0, info.duration);
        result.seekable = info.seekable;
        result.activeAudioTrack = info.activeAudioTrack;
        result.activeSubtitleTrack = info.activeSubtitleTrack;
        for (const TrackInfo& track : info.tracks) {
            if (track.type == MediaType::Audio) {
                result.audioTracks.push_back(track);
                if (track.index == info.activeAudioTrack) {
                    result.audioCodec = track.codec;
                }
            } else if (track.type == MediaType::Subtitle) {
                result.subtitleTracks.push_back(track);
            } else if (track.type == MediaType::Video
                       && track.index == info.activeVideoTrack) {
                result.videoCodec = track.codec;
                result.width = track.width;
                result.height = track.height;
            }
        }
        if (result.width <= 0 || result.height <= 0) {
            result.width = videoWidth_.load(std::memory_order_relaxed);
            result.height = videoHeight_.load(std::memory_order_relaxed);
        }

        const PlaybackStatistics statistics = player_.playbackStatistics();
        result.droppedFrames = statistics.videoQueueOverflowDrops
            + statistics.lateVideoDrops;
        if (vulkanInterop_) {
            const OHCodecVulkanInteropStatistics interopStatistics =
                vulkanInterop_->statistics();
            result.opaqueExternalImports =
                interopStatistics.opaqueExternalImports;
            result.externalFormatWorkaroundImports =
                interopStatistics.externalFormatWorkaroundImports;
            result.externalNormalizationPasses =
                interopStatistics.normalizationPasses;
            result.nativeBuffersAcquired =
                interopStatistics.nativeBuffersAcquired;
            result.frameAvailableCallbacks =
                interopStatistics.frameAvailableCallbacks;
            result.outputsReleasedAfterGpu =
                interopStatistics.outputsReleasedAfterGpu;
            result.lastVulkanSourceFormat = static_cast<std::int32_t>(
                interopStatistics.lastVulkanFormat);
            result.lastExternalFormat =
                interopStatistics.lastExternalFormat;
        }
        {
            std::lock_guard<std::mutex> lock(statusMutex_);
            result.source = source_;
            result.error = error_;
            result.renderAPI = renderAPI_;
            result.decoderAPI = decoderAPI_;
            result.inputColorSpace = inputColorSpace_;
            result.outputColorSpace = outputColorSpace_;
            result.outputFormat = outputFormat_;
            result.hdrOutput = hdrOutput_;
        }
        result.decodePreference = decodePreferenceName(decodePreference_);
        result.renderPreference = renderPreferenceName(renderPreference_);
        result.hdrPreference = hdrPreferenceName(hdrPreference_);
        result.toneMappedToSdr = result.hdrInput && !result.hdrOutput;
        {
            std::lock_guard<std::mutex> lock(subtitleMutex_);
            if (result.activeSubtitleTrack >= 0
                && result.position >= subtitleStart_
                && result.position <= subtitleEnd_) {
                result.subtitle = subtitleText_;
            }
        }
        return result;
    }

    PlayerProgressSnapshot progressSnapshot() const
    {
        PlayerProgressSnapshot result;
        result.playing = state_.load(std::memory_order_acquire)
            == State::Playing;
        result.mediaStatus = statusName(
            mediaStatus_.load(std::memory_order_acquire));
        result.position = std::max<std::int64_t>(0, player_.position());
        result.bufferingProgress = static_cast<double>(
            bufferingProgressMilli_.load(std::memory_order_acquire))
            / 1'000.0;
        {
            std::lock_guard<std::mutex> lock(statusMutex_);
            result.error = error_;
        }
        {
            std::lock_guard<std::mutex> lock(subtitleMutex_);
            if (result.position >= subtitleStart_
                && result.position <= subtitleEnd_) {
                result.subtitle = subtitleText_;
            }
        }
        return result;
    }

private:
    bool createRendererLocked()
    {
        MobileRendererSelectorConfig config;
        config.maximumRecoveryAttempts = 2;
        config.preferredAPI =
            renderPreference_ == RenderPreference::OpenGLES
            ? MobileRenderAPI::OpenGLES
            : MobileRenderAPI::Vulkan;
        if (renderPreference_ != RenderPreference::OpenGLES) {
            config.vulkan = [this] {
            openGLRenderer_.reset();
            openGLInterop_.reset();
            auto context = std::make_unique<
                qtav::ohos_example::OHOSVulkanContext>();
            std::string error;
            if (!context->create(window_, error)) {
                return MobileRendererCandidate {
                    {},
                    error.empty()
                        ? "Vulkan is unavailable"
                        : std::move(error),
                };
            }
            std::shared_ptr<OHCodecVulkanInterop> interop;
            std::string interopDetail;
            if (decodePreference_ != DecodePreference::Software) {
                if (context->nativeBufferExternalMemoryEnabled()
                    && context->foreignQueueFamilyEnabled()
                    && context->syncFdSemaphoreEnabled()) {
                    OHCodecVulkanInteropConfig interopConfig;
                    interopConfig.ohosExternalMemoryEnabled = true;
                    interopConfig.foreignQueueFamilyEnabled = true;
                    interopConfig.syncFdSemaphoreEnabled = true;
                    interopConfig.samplerYcbcrConversionEnabled =
                        context->samplerYcbcrConversionEnabled();
                    interop = std::make_shared<OHCodecVulkanInterop>(
                        context->borrowed().device,
                        interopConfig);
                    if (!*interop) {
                        interopDetail = interop->lastError();
                        interop.reset();
                    }
                } else {
                    interopDetail =
                        "required OHCodec Vulkan import extensions are unavailable";
                }
                if (!interop
                    && decodePreference_ == DecodePreference::Hardware) {
                    return MobileRendererCandidate {
                        {},
                        "Vulkan hardware decode is unavailable: "
                            + interopDetail,
                    };
                }
            }
            auto renderer =
                std::make_shared<OHOSVulkanVideoRenderer>(
                    context->borrowed(),
                    vulkanOutputPreference(hdrPreference_));
            if (interop) {
                renderer->setHardwareFrameInterop(interop);
            }
            renderer->setEventCallback(
                [this](const VideoRenderEvent& event) {
                    if (event.type
                        == VideoRenderEventType::RedrawRequested) {
                        requestRender();
                    } else if (event.type
                               == VideoRenderEventType::Error) {
                        setError("Vulkan renderer: " + event.detail);
                    }
                });
            if (!renderer->setWindow(window_)) {
                return MobileRendererCandidate {
                    {},
                    hdrPreference_ == HdrPreference::RequireHdr
                        ? "The OHOS Vulkan surface has no supported native HDR format/color-space pair"
                        : "The Vulkan renderer rejected the XComponent window",
                };
            }
            const VkSurfaceFormatKHR surfaceFormat =
                renderer->surfaceFormat();
            std::ostringstream detail;
            detail << context->description() << " · format "
                   << static_cast<int>(surfaceFormat.format)
                   << " · " << vulkanColorSpaceName(surfaceFormat.colorSpace)
                   << (interop ? " · OHCodec" : " · software decode");
            {
                std::lock_guard<std::mutex> lock(statusMutex_);
                outputColorSpace_ =
                    vulkanColorSpaceName(surfaceFormat.colorSpace);
                outputFormat_ = "VkFormat "
                    + std::to_string(
                        static_cast<int>(surfaceFormat.format));
                hdrOutput_ = renderer->hdrOutputActive();
            }
            vulkanRenderer_ = renderer;
            vulkanInterop_ = interop;
            vulkanContext_ = std::move(context);
            return MobileRendererCandidate {
                std::move(renderer),
                detail.str(),
            };
            };
        }
        if (renderPreference_ != RenderPreference::Vulkan) {
            config.openGLES = [this] {
            if (vulkanInterop_) {
                retiredVulkanSurfaceGeneration_ =
                    vulkanInterop_->surface().generation();
            }
            vulkanRenderer_.reset();
            vulkanInterop_.reset();
            vulkanContext_.reset();
            std::shared_ptr<OHCodecOpenGLInterop> interop;
            if (decodePreference_ != DecodePreference::Software) {
                OHCodecOpenGLInteropConfig interopConfig;
                interopConfig.maximumPendingFrames = 4;
                interop =
                    std::make_shared<OHCodecOpenGLInterop>(interopConfig);
                // OHCodecOpenGLInterop creates its OH_NativeImage surface
                // while OpenGLVideoRenderer::open() owns a current EGL
                // context. It is intentionally not valid before that point.
                // Checking operator bool here would reject every hardware
                // OpenGL ES candidate before it had a chance to initialize.
            }
            auto renderer =
                std::make_shared<OHOSOpenGLVideoRenderer>(
                    openGLOutputPreference(hdrPreference_));
            if (interop) {
                renderer->setHardwareFrameInterop(interop);
            }
            renderer->setEventCallback(
                [this](const VideoRenderEvent& event) {
                    if (event.type
                        == VideoRenderEventType::RedrawRequested) {
                        requestRender();
                    } else if (event.type
                               == VideoRenderEventType::Error) {
                        setError("OpenGL ES renderer: " + event.detail);
                    }
                });
            if (!renderer->setWindow(window_)) {
                return MobileRendererCandidate {
                    {},
                    hdrPreference_ == HdrPreference::RequireHdr
                        ? "The OHOS EGL surface rejected both BT.2020/PQ and BT.2020/HLG native HDR color spaces"
                        : "The OpenGL ES renderer rejected the XComponent window",
                };
            }
            const VideoSize size = renderer->surfaceSize();
            const OpenGLOutputColorSpace colorSpace =
                renderer->outputColorSpace();
            const int componentBits = renderer->colorComponentBits();
            {
                std::lock_guard<std::mutex> lock(statusMutex_);
                outputColorSpace_ = openGLColorSpaceName(colorSpace);
                outputFormat_ = componentBits == 10
                    ? "RGB10_A2"
                    : "RGBA8";
                hdrOutput_ = renderer->hdrOutputActive();
            }
            openGLRenderer_ = renderer;
            openGLInterop_ = interop;
            return MobileRendererCandidate {
                std::move(renderer),
                "OpenGL ES 3 "
                    + std::string(componentBits == 10
                            ? "RGB10_A2/"
                            : "RGBA8/")
                    + openGLColorSpaceName(colorSpace) + " "
                    + std::to_string(size.width) + "x"
                    + std::to_string(size.height)
                    + (interop ? " · OHCodec" : " · software decode"),
            };
            };
        }

        auto selector = std::make_shared<MobileVideoRendererSelector>(
            std::move(config));
        selector->setSelectionCallback(
            [this](const MobileRendererSelectionEvent& event) {
                {
                    std::lock_guard<std::mutex> lock(statusMutex_);
                    renderAPI_ = mobileRenderAPIName(event.selectedAPI);
                }
                logMessage(
                    event.type
                            == MobileRendererSelectionEventType::Unavailable
                        ? LOG_ERROR
                        : LOG_INFO,
                    std::string("Renderer ")
                        + mobileRenderAPIName(event.previousAPI) + " -> "
                        + mobileRenderAPIName(event.selectedAPI) + ": "
                        + event.detail);
            });
        selector->setHardwareFrameFallbackCallback(
            [this](const MobileHardwareFrameFallbackEvent& event) {
                return applyHardwareFrameFallbackLocked(event);
            });
        selector->setEventCallback(
            [this](const VideoRenderEvent& event) {
                if (event.type
                    == VideoRenderEventType::RedrawRequested) {
                    requestRender();
                } else if (event.type
                           == VideoRenderEventType::SurfaceLost) {
                    setError(
                        event.detail.empty()
                            ? "The selected renderer lost its video surface"
                            : event.detail);
                } else if (event.type
                           == VideoRenderEventType::Error) {
                    setError(
                        event.detail.empty()
                            ? "The selected renderer reported an error"
                            : event.detail);
                }
            });
        if (!selector->open(renderConfig_)) {
            setError(
                selector->lastError().empty()
                    ? "No OHOS renderer is available"
                    : selector->lastError());
            selector->close();
            return false;
        }
        selector_ = std::move(selector);
        if (!configureDecoderLocked()) {
            selector_->close();
            selector_.reset();
            return false;
        }
        player_
            .setVideoRenderAPI(selector_)
            .setRenderCallback([this](void*) { requestRender(); });
        return true;
    }

    bool rebuildRendererLocked()
    {
        if (!window_) {
            return true;
        }
        const bool resume =
            wantsPlaying_.load(std::memory_order_acquire);
        if (resume) {
            player_.setState(State::Paused);
        }
        closeRendererLocked();
        const bool created = createRendererLocked();
        surfaceReady_.store(created, std::memory_order_release);
        if (created && resume) {
            player_.setState(State::Playing);
        }
        return created;
    }

    OHCodecSurface selectedOHCodecSurfaceLocked() const
    {
        if (!selector_) {
            return {};
        }
        if (selector_->selectedAPI() == MobileRenderAPI::Vulkan
            && vulkanInterop_) {
            return vulkanInterop_->surface();
        }
        if (selector_->selectedAPI() == MobileRenderAPI::OpenGLES
            && openGLInterop_) {
            return openGLInterop_->surface();
        }
        return {};
    }

    bool configureDecoderLocked()
    {
        if (decodePreference_ == DecodePreference::Software) {
            player_.setHardwareDecodeConfig({});
            std::lock_guard<std::mutex> lock(statusMutex_);
            decoderAPI_ = "software";
            return true;
        }

        const OHCodecSurface surface = selectedOHCodecSurfaceLocked();
        if (!surface) {
            if (decodePreference_ == DecodePreference::Auto) {
                player_.setHardwareDecodeConfig({});
                std::lock_guard<std::mutex> lock(statusMutex_);
                decoderAPI_ = "software";
                return true;
            }
            setError(
                "Hardware decode was required, but the selected renderer did not publish an OHCodec surface");
            OHCodecHardwareDecodeOptions options;
            options.allowSoftwareFallback = false;
            player_.setHardwareDecodeConfig(
                ohCodecHardwareDecodeConfig({}, options));
            return false;
        }

        OHCodecHardwareDecodeOptions options;
        options.allowSoftwareFallback =
            decodePreference_ == DecodePreference::Auto;
        options.extraHardwareFrames = 8;
        player_.setHardwareDecodeConfig(
            ohCodecHardwareDecodeConfig(surface, options));
        std::lock_guard<std::mutex> lock(statusMutex_);
        decoderAPI_ = "pending";
        return true;
    }

    MobileHardwareFrameFallbackDecision
    applyHardwareFrameFallbackLocked(
        const MobileHardwareFrameFallbackEvent& event)
    {
        if (event.previousAPI == MobileRenderAPI::Vulkan
            && event.selectedAPI == MobileRenderAPI::OpenGLES
            && event.sourceDevice == HardwareDeviceType::OHCodec
            && openGLInterop_ && *openGLInterop_
            && openGLInterop_->surface()
            && (retiredVulkanSurfaceGeneration_ == 0
                || event.sourceSurfaceGeneration
                    == retiredVulkanSurfaceGeneration_)) {
            OHCodecHardwareDecodeOptions options;
            options.allowSoftwareFallback =
                decodePreference_ == DecodePreference::Auto;
            options.extraHardwareFrames = 6;
            player_.setHardwareDecodeConfig(
                ohCodecHardwareDecodeConfig(
                    openGLInterop_->surface(),
                    options));
            {
                std::lock_guard<std::mutex> lock(statusMutex_);
                decoderAPI_ = "pending";
            }
            return {
                MobileHardwareFrameFallbackRoute::OpenGLESInterop,
                "Rebound OHCodec output to the OpenGL ES native-image surface",
            };
        }
        if (decodePreference_ == DecodePreference::Auto
            && event.selectedAPI != MobileRenderAPI::None) {
            player_.setHardwareDecodeConfig({});
            {
                std::lock_guard<std::mutex> lock(statusMutex_);
                decoderAPI_ = "software";
            }
            return {
                MobileHardwareFrameFallbackRoute::SoftwareDecode,
                "Retained the renderer and reopened subsequent frames with software decode",
            };
        }
        setError(
            "The forced hardware decode path could not follow the renderer fallback");
        return {
            MobileHardwareFrameFallbackRoute::None,
            "Forced hardware decode has no compatible replacement surface",
        };
    }

    void closeRendererLocked()
    {
        player_.setRenderCallback({})
            .setVideoRenderAPI({})
            .setHardwareDecodeConfig({});
        if (selector_) {
            selector_->setHardwareFrameFallbackCallback({});
            selector_->close();
            selector_.reset();
        }
        openGLRenderer_.reset();
        openGLInterop_.reset();
        vulkanRenderer_.reset();
        vulkanInterop_.reset();
        vulkanContext_.reset();
        {
            std::lock_guard<std::mutex> lock(statusMutex_);
            renderAPI_ = "none";
            outputColorSpace_ = "unknown";
            outputFormat_ = "unknown";
            hdrOutput_ = false;
        }
    }

    void requestRender()
    {
        {
            std::lock_guard<std::mutex> lock(renderRequestMutex_);
            renderRequested_ = true;
        }
        renderCondition_.notify_one();
    }

    void renderLoop()
    {
        for (;;) {
            {
                std::unique_lock<std::mutex> lock(renderRequestMutex_);
                renderCondition_.wait(lock, [this] {
                    return renderQuit_ || renderRequested_;
                });
                if (renderQuit_) {
                    return;
                }
                renderRequested_ = false;
            }

            VideoRenderResult result;
            {
                std::lock_guard<std::mutex> lock(pipelineMutex_);
                if (!selector_) {
                    continue;
                }
                result = player_.renderVideoDetailed();
            }
            if (result.status == VideoRenderStatus::Rendered) {
                renderedFrames_.fetch_add(1, std::memory_order_relaxed);
                recordPresentation();
            } else if (result.status == VideoRenderStatus::RendererBusy) {
                const auto delay = std::chrono::milliseconds(
                    std::max<std::uint32_t>(
                        1,
                        result.retryAfterMilliseconds));
                std::unique_lock<std::mutex> lock(renderRequestMutex_);
                renderCondition_.wait_for(lock, delay, [this] {
                    return renderQuit_ || renderRequested_;
                });
                if (renderQuit_) {
                    return;
                }
                renderRequested_ = true;
            } else if (result.status == VideoRenderStatus::SurfaceLost) {
                setError("The video surface was lost; recreate the page or PiP window");
            } else if (result.status == VideoRenderStatus::RendererError) {
                setError(
                    result.detail.empty()
                        ? "The active video renderer failed"
                        : result.detail);
            }
        }
    }

    void recordPresentation()
    {
        const auto now = std::chrono::steady_clock::now();
        std::lock_guard<std::mutex> lock(timingMutex_);
        presentationTimes_.push_back(now);
        const auto windowStart = now - FpsWindow;
        while (presentationTimes_.size() > 1
               && presentationTimes_.front() < windowStart) {
            presentationTimes_.pop_front();
        }
    }

    double presentationFps() const
    {
        const auto now = std::chrono::steady_clock::now();
        std::lock_guard<std::mutex> lock(timingMutex_);
        if (presentationTimes_.size() < 2
            || now - presentationTimes_.back() > FpsStaleAfter) {
            return 0.0;
        }
        const double elapsed = std::chrono::duration<double>(
            presentationTimes_.back() - presentationTimes_.front())
                                   .count();
        return elapsed > 0.0
            ? static_cast<double>(presentationTimes_.size() - 1)
                / elapsed
            : 0.0;
    }

    void resetMediaObservations()
    {
        mediaStatus_.store(MediaStatus::Loading, std::memory_order_release);
        decodedFrames_.store(0, std::memory_order_relaxed);
        hardwareFrames_.store(0, std::memory_order_relaxed);
        softwareFrames_.store(0, std::memory_order_relaxed);
        renderedFrames_.store(0, std::memory_order_relaxed);
        hdrInput_.store(false, std::memory_order_relaxed);
        dolbyVisionInput_.store(false, std::memory_order_relaxed);
        videoWidth_.store(0, std::memory_order_relaxed);
        videoHeight_.store(0, std::memory_order_relaxed);
        bufferingProgressMilli_.store(0, std::memory_order_release);
        {
            std::lock_guard<std::mutex> lock(timingMutex_);
            presentationTimes_.clear();
        }
        {
            std::lock_guard<std::mutex> lock(statusMutex_);
            decoderAPI_ = decodePreference_ == DecodePreference::Software
                ? "software"
                : "pending";
            inputColorSpace_ = "unknown";
        }
        clearSubtitle();
    }

    void clearSubtitle()
    {
        std::lock_guard<std::mutex> lock(subtitleMutex_);
        subtitleText_.clear();
        subtitleStart_ = 0;
        subtitleEnd_ = 0;
    }

    void closeRetainedDescriptors()
    {
        std::vector<int> descriptors;
        {
            std::lock_guard<std::mutex> lock(descriptorMutex_);
            descriptors.swap(retainedDescriptors_);
        }
        for (const int descriptor : descriptors) {
            close(descriptor);
        }
    }

    void setError(std::string error)
    {
        logMessage(LOG_ERROR, error);
        std::lock_guard<std::mutex> lock(statusMutex_);
        error_ = std::move(error);
    }

    mutable std::mutex pipelineMutex_;
    Player player_;
    std::shared_ptr<OHAudioAudioSink> audioSink_;
    OHNativeWindow* window_ = nullptr;
    VideoRenderConfig renderConfig_;
    std::shared_ptr<MobileVideoRendererSelector> selector_;
    std::shared_ptr<OHOSVulkanVideoRenderer> vulkanRenderer_;
    std::shared_ptr<OHCodecVulkanInterop> vulkanInterop_;
    std::shared_ptr<OHOSOpenGLVideoRenderer> openGLRenderer_;
    std::shared_ptr<OHCodecOpenGLInterop> openGLInterop_;
    std::unique_ptr<qtav::ohos_example::OHOSVulkanContext>
        vulkanContext_;
    DecodePreference decodePreference_ = DecodePreference::Auto;
    RenderPreference renderPreference_ = RenderPreference::Auto;
    HdrPreference hdrPreference_ = HdrPreference::Auto;
    std::uint32_t retiredVulkanSurfaceGeneration_ = 0;

    std::thread renderWorker_;
    std::mutex renderRequestMutex_;
    std::condition_variable renderCondition_;
    bool renderRequested_ = false;
    bool renderQuit_ = false;

    std::atomic<bool> surfaceReady_ { false };
    std::atomic<bool> wantsPlaying_ { false };
    std::atomic<State> state_ { State::Stopped };
    std::atomic<MediaStatus> mediaStatus_ { MediaStatus::NoMedia };
    std::atomic<int> bufferingProgressMilli_ { 1'000 };
    std::atomic<int> videoWidth_ { 0 };
    std::atomic<int> videoHeight_ { 0 };
    std::atomic<std::uint64_t> decodedFrames_ { 0 };
    std::atomic<std::uint64_t> hardwareFrames_ { 0 };
    std::atomic<std::uint64_t> softwareFrames_ { 0 };
    std::atomic<std::uint64_t> renderedFrames_ { 0 };
    std::atomic<bool> hdrInput_ { false };
    std::atomic<bool> dolbyVisionInput_ { false };

    mutable std::mutex statusMutex_;
    std::string source_;
    std::string renderAPI_ = "none";
    std::string decoderAPI_ = "pending";
    std::string inputColorSpace_ = "unknown";
    std::string outputColorSpace_ = "unknown";
    std::string outputFormat_ = "unknown";
    bool hdrOutput_ = false;
    std::string error_;

    mutable std::mutex subtitleMutex_;
    std::string subtitleText_;
    std::int64_t subtitleStart_ = 0;
    std::int64_t subtitleEnd_ = 0;

    mutable std::mutex timingMutex_;
    std::deque<std::chrono::steady_clock::time_point>
        presentationTimes_;

    std::mutex descriptorMutex_;
    std::vector<int> retainedDescriptors_;
};

PlayerSession& session()
{
    static PlayerSession value;
    return value;
}

void onSurfaceCreated(
    OH_NativeXComponent* component,
    void* window)
{
    session().createSurface(
        component,
        static_cast<OHNativeWindow*>(window));
}

void onSurfaceChanged(
    OH_NativeXComponent* component,
    void* window)
{
    session().changeSurface(
        component,
        static_cast<OHNativeWindow*>(window));
}

void onSurfaceDestroyed(OH_NativeXComponent*, void*)
{
    session().releaseSurface();
}

void dispatchTouchEvent(OH_NativeXComponent*, void*)
{
}

OH_NativeXComponent_Callback XComponentCallbacks {
    onSurfaceCreated,
    onSurfaceChanged,
    onSurfaceDestroyed,
    dispatchTouchEvent,
};

napi_value undefinedValue(napi_env env)
{
    napi_value result = nullptr;
    napi_get_undefined(env, &result);
    return result;
}

napi_value booleanValue(napi_env env, bool value)
{
    napi_value result = nullptr;
    napi_get_boolean(env, value, &result);
    return result;
}

bool stringValue(
    napi_env env,
    napi_value value,
    std::string& result)
{
    std::size_t length = 0;
    if (napi_get_value_string_utf8(
            env,
            value,
            nullptr,
            0,
            &length)
        != napi_ok) {
        return false;
    }
    std::vector<char> buffer(length + 1, '\0');
    std::size_t copied = 0;
    if (napi_get_value_string_utf8(
            env,
            value,
            buffer.data(),
            buffer.size(),
            &copied)
        != napi_ok) {
        return false;
    }
    result.assign(buffer.data(), copied);
    return true;
}

void setNamedString(
    napi_env env,
    napi_value object,
    const char* name,
    const std::string& value)
{
    napi_value property = nullptr;
    napi_create_string_utf8(
        env,
        value.c_str(),
        value.size(),
        &property);
    napi_set_named_property(env, object, name, property);
}

void setNamedBoolean(
    napi_env env,
    napi_value object,
    const char* name,
    bool value)
{
    napi_value property = nullptr;
    napi_get_boolean(env, value, &property);
    napi_set_named_property(env, object, name, property);
}

void setNamedNumber(
    napi_env env,
    napi_value object,
    const char* name,
    double value)
{
    napi_value property = nullptr;
    napi_create_double(env, value, &property);
    napi_set_named_property(env, object, name, property);
}

napi_value trackArray(
    napi_env env,
    const std::vector<TrackInfo>& tracks)
{
    napi_value result = nullptr;
    napi_create_array_with_length(env, tracks.size(), &result);
    for (std::size_t i = 0; i < tracks.size(); ++i) {
        const TrackInfo& track = tracks[i];
        napi_value item = nullptr;
        napi_create_object(env, &item);
        setNamedNumber(env, item, "index", track.index);
        setNamedString(env, item, "label", trackLabel(track));
        setNamedString(env, item, "codec", track.codec);
        setNamedString(env, item, "language", track.language);
        setNamedString(env, item, "title", track.title);
        setNamedNumber(env, item, "bitRate", track.bitRate);
        setNamedNumber(env, item, "channels", track.channels);
        setNamedNumber(env, item, "sampleRate", track.sampleRate);
        setNamedBoolean(env, item, "external", track.external);
        napi_set_element(
            env,
            result,
            static_cast<std::uint32_t>(i),
            item);
    }
    return result;
}

napi_value openUrl(napi_env env, napi_callback_info info)
{
    std::size_t argumentCount = 1;
    napi_value arguments[1] { nullptr };
    std::string url;
    if (napi_get_cb_info(
            env,
            info,
            &argumentCount,
            arguments,
            nullptr,
            nullptr)
            != napi_ok
        || argumentCount != 1
        || !stringValue(env, arguments[0], url)) {
        return booleanValue(env, false);
    }
    return booleanValue(env, session().openUrl(url));
}

napi_value openLocalFd(napi_env env, napi_callback_info info)
{
    std::size_t argumentCount = 2;
    napi_value arguments[2] { nullptr, nullptr };
    std::int32_t descriptor = -1;
    std::string name;
    if (napi_get_cb_info(
            env,
            info,
            &argumentCount,
            arguments,
            nullptr,
            nullptr)
            != napi_ok
        || argumentCount != 2
        || napi_get_value_int32(env, arguments[0], &descriptor)
            != napi_ok
        || !stringValue(env, arguments[1], name)) {
        return booleanValue(env, false);
    }
    return booleanValue(
        env,
        session().openLocalDescriptor(descriptor, name));
}

napi_value setPlaying(napi_env env, napi_callback_info info)
{
    std::size_t argumentCount = 1;
    napi_value arguments[1] { nullptr };
    bool playing = false;
    if (napi_get_cb_info(
            env,
            info,
            &argumentCount,
            arguments,
            nullptr,
            nullptr)
            == napi_ok
        && argumentCount == 1
        && napi_get_value_bool(env, arguments[0], &playing)
            == napi_ok) {
        session().setPlaying(playing);
    }
    return undefinedValue(env);
}

napi_value stop(napi_env env, napi_callback_info)
{
    session().stop();
    return undefinedValue(env);
}

napi_value seek(napi_env env, napi_callback_info info)
{
    std::size_t argumentCount = 1;
    napi_value arguments[1] { nullptr };
    std::int64_t position = 0;
    if (napi_get_cb_info(
            env,
            info,
            &argumentCount,
            arguments,
            nullptr,
            nullptr)
            != napi_ok
        || argumentCount != 1
        || napi_get_value_int64(env, arguments[0], &position)
            != napi_ok) {
        return booleanValue(env, false);
    }
    return booleanValue(env, session().seek(position));
}

napi_value setRate(napi_env env, napi_callback_info info)
{
    std::size_t argumentCount = 1;
    napi_value arguments[1] { nullptr };
    double rate = 1.0;
    if (napi_get_cb_info(
            env,
            info,
            &argumentCount,
            arguments,
            nullptr,
            nullptr)
            != napi_ok
        || argumentCount != 1
        || napi_get_value_double(env, arguments[0], &rate)
            != napi_ok) {
        return booleanValue(env, false);
    }
    return booleanValue(env, session().setPlaybackRate(rate));
}

napi_value setDecodeMode(napi_env env, napi_callback_info info)
{
    std::size_t argumentCount = 1;
    napi_value arguments[1] { nullptr };
    std::string value;
    if (napi_get_cb_info(
            env,
            info,
            &argumentCount,
            arguments,
            nullptr,
            nullptr)
            != napi_ok
        || argumentCount != 1
        || !stringValue(env, arguments[0], value)) {
        return booleanValue(env, false);
    }
    return booleanValue(env, session().setDecodePreference(value));
}

napi_value setRenderMode(napi_env env, napi_callback_info info)
{
    std::size_t argumentCount = 1;
    napi_value arguments[1] { nullptr };
    std::string value;
    if (napi_get_cb_info(
            env,
            info,
            &argumentCount,
            arguments,
            nullptr,
            nullptr)
            != napi_ok
        || argumentCount != 1
        || !stringValue(env, arguments[0], value)) {
        return booleanValue(env, false);
    }
    return booleanValue(env, session().setRenderPreference(value));
}

napi_value setHdrMode(napi_env env, napi_callback_info info)
{
    std::size_t argumentCount = 1;
    napi_value arguments[1] { nullptr };
    std::string value;
    if (napi_get_cb_info(
            env,
            info,
            &argumentCount,
            arguments,
            nullptr,
            nullptr)
            != napi_ok
        || argumentCount != 1
        || !stringValue(env, arguments[0], value)) {
        return booleanValue(env, false);
    }
    return booleanValue(env, session().setHdrPreference(value));
}

napi_value selectAudioTrack(napi_env env, napi_callback_info info)
{
    std::size_t argumentCount = 1;
    napi_value arguments[1] { nullptr };
    std::int32_t selector = -1;
    if (napi_get_cb_info(
            env,
            info,
            &argumentCount,
            arguments,
            nullptr,
            nullptr)
            != napi_ok
        || argumentCount != 1
        || napi_get_value_int32(env, arguments[0], &selector)
            != napi_ok) {
        return booleanValue(env, false);
    }
    return booleanValue(
        env,
        session().selectTrack(MediaType::Audio, selector));
}

napi_value selectSubtitleTrack(
    napi_env env,
    napi_callback_info info)
{
    std::size_t argumentCount = 1;
    napi_value arguments[1] { nullptr };
    std::int32_t selector = -1;
    if (napi_get_cb_info(
            env,
            info,
            &argumentCount,
            arguments,
            nullptr,
            nullptr)
            != napi_ok
        || argumentCount != 1
        || napi_get_value_int32(env, arguments[0], &selector)
            != napi_ok) {
        return booleanValue(env, false);
    }
    return booleanValue(
        env,
        session().selectTrack(MediaType::Subtitle, selector));
}

napi_value snapshot(napi_env env, napi_callback_info)
{
    const PlayerSnapshot snapshot = session().snapshot();
    napi_value result = nullptr;
    napi_create_object(env, &result);
    setNamedBoolean(env, result, "surfaceReady", snapshot.surfaceReady);
    setNamedBoolean(env, result, "playing", snapshot.playing);
    setNamedBoolean(env, result, "seekable", snapshot.seekable);
    setNamedString(env, result, "source", snapshot.source);
    setNamedString(env, result, "state", snapshot.state);
    setNamedString(env, result, "mediaStatus", snapshot.mediaStatus);
    setNamedString(env, result, "subtitle", snapshot.subtitle);
    setNamedString(env, result, "videoCodec", snapshot.videoCodec);
    setNamedString(env, result, "audioCodec", snapshot.audioCodec);
    setNamedString(env, result, "renderAPI", snapshot.renderAPI);
    setNamedString(
        env,
        result,
        "renderPreference",
        snapshot.renderPreference);
    setNamedString(
        env,
        result,
        "decodePreference",
        snapshot.decodePreference);
    setNamedString(
        env,
        result,
        "hdrPreference",
        snapshot.hdrPreference);
    setNamedString(env, result, "decoderAPI", snapshot.decoderAPI);
    setNamedString(
        env,
        result,
        "inputColorSpace",
        snapshot.inputColorSpace);
    setNamedString(
        env,
        result,
        "outputColorSpace",
        snapshot.outputColorSpace);
    setNamedString(
        env,
        result,
        "outputFormat",
        snapshot.outputFormat);
    setNamedString(env, result, "error", snapshot.error);
    setNamedBoolean(env, result, "hdrInput", snapshot.hdrInput);
    setNamedBoolean(
        env,
        result,
        "dolbyVisionInput",
        snapshot.dolbyVisionInput);
    setNamedBoolean(env, result, "hdrOutput", snapshot.hdrOutput);
    setNamedBoolean(
        env,
        result,
        "toneMappedToSdr",
        snapshot.toneMappedToSdr);
    setNamedNumber(env, result, "positionMs", snapshot.position);
    setNamedNumber(env, result, "durationMs", snapshot.duration);
    setNamedNumber(env, result, "playbackRate", snapshot.playbackRate);
    setNamedNumber(
        env,
        result,
        "bufferingProgress",
        snapshot.bufferingProgress);
    setNamedNumber(env, result, "fps", snapshot.fps);
    setNamedNumber(env, result, "width", snapshot.width);
    setNamedNumber(env, result, "height", snapshot.height);
    setNamedNumber(
        env,
        result,
        "activeAudioTrack",
        snapshot.activeAudioTrack);
    setNamedNumber(
        env,
        result,
        "activeSubtitleTrack",
        snapshot.activeSubtitleTrack);
    setNamedNumber(
        env,
        result,
        "decodedFrames",
        static_cast<double>(snapshot.decodedFrames));
    setNamedNumber(
        env,
        result,
        "hardwareFrames",
        static_cast<double>(snapshot.hardwareFrames));
    setNamedNumber(
        env,
        result,
        "softwareFrames",
        static_cast<double>(snapshot.softwareFrames));
    setNamedNumber(
        env,
        result,
        "renderedFrames",
        static_cast<double>(snapshot.renderedFrames));
    setNamedNumber(
        env,
        result,
        "droppedFrames",
        static_cast<double>(snapshot.droppedFrames));
    setNamedNumber(
        env,
        result,
        "opaqueExternalImports",
        static_cast<double>(snapshot.opaqueExternalImports));
    setNamedNumber(
        env,
        result,
        "externalFormatWorkaroundImports",
        static_cast<double>(snapshot.externalFormatWorkaroundImports));
    setNamedNumber(
        env,
        result,
        "externalNormalizationPasses",
        static_cast<double>(snapshot.externalNormalizationPasses));
    setNamedNumber(
        env,
        result,
        "nativeBuffersAcquired",
        static_cast<double>(snapshot.nativeBuffersAcquired));
    setNamedNumber(
        env,
        result,
        "frameAvailableCallbacks",
        static_cast<double>(snapshot.frameAvailableCallbacks));
    setNamedNumber(
        env,
        result,
        "outputsReleasedAfterGpu",
        static_cast<double>(snapshot.outputsReleasedAfterGpu));
    setNamedNumber(
        env,
        result,
        "lastVulkanSourceFormat",
        snapshot.lastVulkanSourceFormat);
    setNamedNumber(
        env,
        result,
        "lastExternalFormat",
        static_cast<double>(snapshot.lastExternalFormat));
    napi_set_named_property(
        env,
        result,
        "audioTracks",
        trackArray(env, snapshot.audioTracks));
    napi_set_named_property(
        env,
        result,
        "subtitleTracks",
        trackArray(env, snapshot.subtitleTracks));
    return result;
}

napi_value progressSnapshot(napi_env env, napi_callback_info)
{
    const PlayerProgressSnapshot snapshot =
        session().progressSnapshot();
    napi_value result = nullptr;
    napi_create_object(env, &result);
    setNamedBoolean(env, result, "playing", snapshot.playing);
    setNamedString(env, result, "mediaStatus", snapshot.mediaStatus);
    setNamedString(env, result, "subtitle", snapshot.subtitle);
    setNamedString(env, result, "error", snapshot.error);
    setNamedNumber(env, result, "positionMs", snapshot.position);
    setNamedNumber(
        env,
        result,
        "bufferingProgress",
        snapshot.bufferingProgress);
    return result;
}

napi_value init(napi_env env, napi_value exports)
{
    napi_property_descriptor properties[] {
        { "openUrl", nullptr, openUrl, nullptr, nullptr, nullptr,
          napi_default, nullptr },
        { "openLocalFd", nullptr, openLocalFd, nullptr, nullptr, nullptr,
          napi_default, nullptr },
        { "setPlaying", nullptr, setPlaying, nullptr, nullptr, nullptr,
          napi_default, nullptr },
        { "stop", nullptr, stop, nullptr, nullptr, nullptr,
          napi_default, nullptr },
        { "seek", nullptr, seek, nullptr, nullptr, nullptr,
          napi_default, nullptr },
        { "setRate", nullptr, setRate, nullptr, nullptr, nullptr,
          napi_default, nullptr },
        { "setDecodeMode", nullptr, setDecodeMode, nullptr, nullptr,
          nullptr, napi_default, nullptr },
        { "setRenderMode", nullptr, setRenderMode, nullptr, nullptr,
          nullptr, napi_default, nullptr },
        { "setHdrMode", nullptr, setHdrMode, nullptr, nullptr, nullptr,
          napi_default, nullptr },
        { "selectAudioTrack", nullptr, selectAudioTrack, nullptr, nullptr,
          nullptr, napi_default, nullptr },
        { "selectSubtitleTrack", nullptr, selectSubtitleTrack, nullptr,
          nullptr, nullptr, napi_default, nullptr },
        { "snapshot", nullptr, snapshot, nullptr, nullptr, nullptr,
          napi_default, nullptr },
        { "progressSnapshot", nullptr, progressSnapshot, nullptr, nullptr,
          nullptr, napi_default, nullptr },
    };
    napi_define_properties(
        env,
        exports,
        sizeof(properties) / sizeof(properties[0]),
        properties);
    logMessage(LOG_INFO, "Native player module initialized");

    napi_value exportInstance = nullptr;
    OH_NativeXComponent* component = nullptr;
    if (napi_get_named_property(
            env,
            exports,
            OH_NATIVE_XCOMPONENT_OBJ,
            &exportInstance)
            == napi_ok
        && napi_unwrap(
               env,
               exportInstance,
               reinterpret_cast<void**>(&component))
            == napi_ok
        && component) {
        const int32_t result = OH_NativeXComponent_RegisterCallback(
            component,
            &XComponentCallbacks);
        if (result != OH_NATIVEXCOMPONENT_RESULT_SUCCESS) {
            logMessage(
                LOG_ERROR,
                "Could not register XComponent callbacks");
        } else {
            logMessage(LOG_INFO, "XComponent callbacks registered");
        }
    } else {
        logMessage(LOG_ERROR, "Could not unwrap Native XComponent");
    }
    return exports;
}

} // namespace
} // namespace qtav::ohos_player

NAPI_MODULE(qtav_player, qtav::ohos_player::init)
