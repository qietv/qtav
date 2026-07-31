// SPDX-License-Identifier: LGPL-2.1-or-later

#include <qtav/player.h>
#include <qtav/audio_converter.h>
#include <qtav/audio_sink.h>
#include <qtav/video_render_api.h>

#include <algorithm>
#include <atomic>
#include <chrono>
#include <condition_variable>
#include <cstdlib>
#include <deque>
#include <mutex>
#include <optional>
#include <stdexcept>
#include <thread>
#include <unordered_map>
#include <utility>
#include <vector>

#include "frame_internal.h"
#include "hardware_decode_device_internal.h"

extern "C" {
#include <libavcodec/avcodec.h>
#include <libavformat/avformat.h>
#include <libavutil/avutil.h>
#include <libavutil/dict.h>
#include <libavutil/error.h>
#include <libavutil/frame.h>
#include <libavutil/hwcontext.h>
#include <libavutil/pixdesc.h>
}

#if LIBAVCODEC_VERSION_MAJOR < 62
#  error "QtAVCore requires FFmpeg 8 or newer"
#endif

namespace qtav {
namespace {

using Clock = std::chrono::steady_clock;
using Milliseconds = std::chrono::milliseconds;

constexpr std::int64_t kMaximumQueuedAudioMilliseconds = 500;
constexpr std::int64_t kMaximumVideoDecodeLeadMilliseconds = 250;
constexpr std::int64_t kMaximumVideoPrerollWaitMilliseconds = 500;
constexpr std::size_t kMinimumVideoPrerollFrames = 6;
constexpr std::size_t kMaximumQueuedSoftwareVideoFrames = 8;
constexpr std::size_t kMaximumQueuedHardwareVideoFrames = 16;
constexpr std::int64_t kLateVideoFrameThresholdMilliseconds =
    kMaximumVideoDecodeLeadMilliseconds;
constexpr std::int64_t kHttpReadTimeoutMicroseconds = 15'000'000;

std::string ffmpegError(int error)
{
    char text[AV_ERROR_MAX_STRING_SIZE] {};
    av_strerror(error, text, sizeof(text));
    return text;
}

std::string dictionaryValue(const AVDictionary* dictionary, const char* key)
{
    const auto* entry = av_dict_get(dictionary, key, nullptr, 0);
    return entry && entry->value ? entry->value : "";
}

MediaType mediaTypeFromFFmpeg(AVMediaType type)
{
    switch (type) {
    case AVMEDIA_TYPE_AUDIO:
        return MediaType::Audio;
    case AVMEDIA_TYPE_VIDEO:
        return MediaType::Video;
    case AVMEDIA_TYPE_SUBTITLE:
        return MediaType::Subtitle;
    default:
        return MediaType::Unknown;
    }
}

std::int64_t toMilliseconds(std::int64_t value, AVRational timeBase)
{
    if (value == AV_NOPTS_VALUE) {
        return 0;
    }
    return av_rescale_q(value, timeBase, AVRational { 1, 1000 });
}

bool sameAudioFormat(const AudioFormat& left, const AudioFormat& right)
{
    return left.sampleRate == right.sampleRate
        && left.channels == right.channels
        && left.sampleFormat == right.sampleFormat
        && left.channelLayout == right.channelLayout;
}

AVHWDeviceType ffmpegHardwareDeviceType(HardwareDeviceType type) noexcept
{
    switch (type) {
    case HardwareDeviceType::D3D11:
        return AV_HWDEVICE_TYPE_D3D11VA;
    case HardwareDeviceType::VideoToolbox:
        return AV_HWDEVICE_TYPE_VIDEOTOOLBOX;
    case HardwareDeviceType::VAAPI:
        return AV_HWDEVICE_TYPE_VAAPI;
    case HardwareDeviceType::MediaCodec:
        return AV_HWDEVICE_TYPE_MEDIACODEC;
    case HardwareDeviceType::Vulkan:
        return AV_HWDEVICE_TYPE_VULKAN;
    case HardwareDeviceType::Unknown:
    case HardwareDeviceType::OpenGL:
    case HardwareDeviceType::Metal:
        return AV_HWDEVICE_TYPE_NONE;
    }
    return AV_HWDEVICE_TYPE_NONE;
}

const AVCodec* decoderForWrapper(
    AVCodecID codecId,
    AVMediaType mediaType,
    const std::string& wrapper) noexcept
{
    if (wrapper.empty()) {
        return nullptr;
    }
    void* opaque = nullptr;
    while (const AVCodec* codec = av_codec_iterate(&opaque)) {
        if (!av_codec_is_decoder(codec)
            || codec->id != codecId
            || codec->type != mediaType
            || !codec->wrapper_name
            || wrapper != codec->wrapper_name) {
            continue;
        }
        return codec;
    }
    return nullptr;
}

bool isSoftwarePixelFormat(AVPixelFormat format) noexcept
{
    const auto* descriptor = av_pix_fmt_desc_get(format);
    return descriptor && (descriptor->flags & AV_PIX_FMT_FLAG_HWACCEL) == 0;
}

bool isHttpUrl(const std::string& url)
{
    return url.compare(0, 7, "http://") == 0
        || url.compare(0, 8, "https://") == 0;
}

void applyHttpRecoveryDefaults(
    const std::string& url,
    AVDictionary** options)
{
    if (!isHttpUrl(url)) {
        return;
    }
    av_dict_set_int(
        options,
        "rw_timeout",
        kHttpReadTimeoutMicroseconds,
        0);
    av_dict_set(options, "reconnect", "1", 0);
    av_dict_set(options, "reconnect_on_network_error", "1", 0);
    av_dict_set(options, "reconnect_delay_max", "2", 0);
    av_dict_set(options, "reconnect_max_retries", "5", 0);
    av_dict_set(options, "reconnect_delay_total_max", "10", 0);
}

} // namespace

class Player::Impl {
public:
    Impl()
    {
        audioSinkCallbackBridge_->owner = this;
        avformat_network_init();
        audioWorker_ = std::thread([this] { runAudioOutput(); });
        presentationWorker_ = std::thread([this] { runPresentation(); });
        worker_ = std::thread([this] { run(); });
    }

    ~Impl()
    {
        quitting_.store(true, std::memory_order_release);
        interruptEpoch_.fetch_add(1, std::memory_order_acq_rel);
        presentationGeneration_.fetch_add(1, std::memory_order_acq_rel);
        // Pair shutdown with each condition variable's wait mutex so a waiter
        // cannot observe the old predicate and go to sleep after notification.
        {
            std::lock_guard<std::mutex> lock(mutex_);
        }
        controlChanged_.notify_all();
        {
            std::lock_guard<std::mutex> lock(audioQueueMutex_);
        }
        audioQueueChanged_.notify_all();
        audioQueueSpace_.notify_all();
        audioQueueDrained_.notify_all();
        {
            std::lock_guard<std::mutex> lock(presentationMutex_);
        }
        presentationChanged_.notify_all();
        presentationDrained_.notify_all();
        {
            std::lock_guard<std::mutex> lock(audioSinkCallbackBridge_->mutex);
            audioSinkCallbackBridge_->owner = nullptr;
        }
        if (worker_.joinable()) {
            worker_.join();
        }
        if (audioWorker_.joinable()) {
            audioWorker_.join();
        }
        if (presentationWorker_.joinable()) {
            presentationWorker_.join();
        }
        replaceAudioSink(nullptr, nullptr, audioSinkSerial_);
        media_.reset();
        avformat_network_deinit();
    }

    struct Decoder {
        AVCodecContext* context = nullptr;
        std::shared_ptr<AVCodecContext> contextLifetime;
        AVStream* stream = nullptr;
        int streamIndex = -1;
        MediaType type = MediaType::Unknown;
        HardwareDeviceType hardwareDeviceType = HardwareDeviceType::Unknown;
        AVPixelFormat hardwarePixelFormat = AV_PIX_FMT_NONE;
        std::uintptr_t hardwareNativeIdentity = 0;
        std::uint32_t hardwareSurfaceGeneration = 0;
        bool allowSoftwareFallback = false;
        bool hardwareFallbackUsed = false;
        bool hardwareFallbackReported = false;

        Decoder() = default;
        Decoder(const Decoder&) = delete;
        Decoder& operator=(const Decoder&) = delete;

        ~Decoder()
        {
            reset();
        }

        void reset()
        {
            context = nullptr;
            contextLifetime.reset();
            stream = nullptr;
            streamIndex = -1;
            type = MediaType::Unknown;
            hardwareDeviceType = HardwareDeviceType::Unknown;
            hardwarePixelFormat = AV_PIX_FMT_NONE;
            hardwareNativeIdentity = 0;
            hardwareSurfaceGeneration = 0;
            allowSoftwareFallback = false;
            hardwareFallbackUsed = false;
            hardwareFallbackReported = false;
        }

        bool valid() const noexcept
        {
            return context && stream && streamIndex >= 0;
        }
    };

    struct InterruptContext {
        Impl* owner = nullptr;
        std::uint64_t epoch = 0;
    };

    struct MediaContext {
        AVFormatContext* format = nullptr;
        Decoder video;
        Decoder audio;
        std::int64_t startTimeUs = 0;

        void reset()
        {
            video.reset();
            audio.reset();
            avformat_close_input(&format);
            startTimeUs = 0;
        }
    };

    struct PrepareRequest {
        std::uint64_t id = 0;
        std::int64_t position = 0;
        SeekFlag flags = SeekFlag::FromStart;
        PrepareCallback callback;
    };

    struct SeekRequest {
        std::uint64_t id = 0;
        std::int64_t position = 0;
        SeekFlag flags = SeekFlag::FromStart;
        SeekCallback callback;
    };

    struct AudioSinkCallbackBridge {
        std::mutex mutex;
        Impl* owner = nullptr;
    };

    struct QueuedAudioFrame {
        AudioFrame frame;
        int track = -1;
        std::uint64_t generation = 0;
        std::int64_t duration = 0;
    };

    struct PresentationItem {
        enum class Type {
            Audio,
            Video,
        };

        Type type = Type::Video;
        AudioFrame audio;
        VideoFrame video;
        int track = -1;
        std::uint64_t generation = 0;

        std::int64_t timestamp() const noexcept
        {
            return type == Type::Audio ? audio.timestamp() : video.timestamp();
        }
    };

    struct CachedAudioClock {
        bool valid = false;
        std::uint64_t sinkSerial = 0;
        std::int64_t position = 0;
        std::int64_t submittedUntil = 0;
        Clock::time_point sampledAt = Clock::now();
    };

    void setMedia(std::string value)
    {
        {
            std::lock_guard<std::mutex> lock(mutex_);
            url_ = std::move(value);
            ++mediaSerial_;
            loadedSerial_ = 0;
            prepareRequest_.reset();
            seekRequest_.reset();
            if (url_.empty()) {
                requestedState_ = State::Stopped;
            }
        }
        resetPlaybackQueues();
        interruptEpoch_.fetch_add(1, std::memory_order_acq_rel);
        controlChanged_.notify_all();
    }

    std::string url() const
    {
        std::lock_guard<std::mutex> lock(mutex_);
        return url_;
    }

    void prepare(
        std::int64_t startPosition,
        PrepareCallback callback,
        SeekFlag flags)
    {
        {
            std::lock_guard<std::mutex> lock(mutex_);
            prepareRequest_ = PrepareRequest {
                ++requestSerial_,
                std::max<std::int64_t>(0, startPosition),
                flags,
                std::move(callback),
            };
            requestedState_ = State::Paused;
        }
        resetPlaybackQueues();
        interruptEpoch_.fetch_add(1, std::memory_order_acq_rel);
        controlChanged_.notify_all();
    }

    bool seek(
        std::int64_t target,
        SeekFlag flags,
        SeekCallback callback)
    {
        const auto current = hasFlag(flags, SeekFlag::FromNow)
            ? position()
            : static_cast<std::int64_t>(0);
        {
            std::lock_guard<std::mutex> lock(mutex_);
            if (!hasOpenMedia_ || currentState_ == State::Stopped) {
                return false;
            }
            if (hasFlag(flags, SeekFlag::FromNow)) {
                target += current;
            }
            target = std::max<std::int64_t>(0, target);
            if (mediaInfo_.duration > 0) {
                target = std::min(target, mediaInfo_.duration);
            }
            seekRequest_ = SeekRequest {
                ++requestSerial_,
                target,
                flags,
                std::move(callback),
            };
        }
        invalidatePlaybackQueues();
        interruptEpoch_.fetch_add(1, std::memory_order_acq_rel);
        controlChanged_.notify_all();
        return true;
    }

    void setState(State value)
    {
        {
            std::lock_guard<std::mutex> lock(mutex_);
            requestedState_ = value;
            if (value == State::Stopped) {
                prepareRequest_.reset();
                seekRequest_.reset();
            }
        }
        if (value == State::Stopped) {
            resetPlaybackQueues();
            interruptEpoch_.fetch_add(1, std::memory_order_acq_rel);
        }
        controlChanged_.notify_all();
        audioQueueChanged_.notify_all();
        audioQueueSpace_.notify_all();
        presentationChanged_.notify_all();
    }

    State state() const
    {
        std::lock_guard<std::mutex> lock(mutex_);
        return currentState_;
    }

    bool waitFor(State value, long timeoutMs)
    {
        std::unique_lock<std::mutex> lock(mutex_);
        const auto predicate = [this, value] {
            return currentState_ == value
                || quitting_.load(std::memory_order_acquire);
        };
        if (timeoutMs < 0) {
            stateChanged_.wait(lock, predicate);
            return currentState_ == value;
        }
        return stateChanged_.wait_for(
                   lock,
                   Milliseconds(timeoutMs),
                   predicate)
            && currentState_ == value;
    }

    MediaStatus mediaStatus() const
    {
        std::lock_guard<std::mutex> lock(mutex_);
        return status_;
    }

    MediaInfo mediaInfo() const
    {
        std::lock_guard<std::mutex> lock(mutex_);
        return mediaInfo_;
    }

    std::int64_t position() const
    {
        if (const auto audioPosition = audioClockPosition()) {
            return clampPosition(*audioPosition);
        }
        std::lock_guard<std::mutex> lock(mutex_);
        return clockPositionLocked();
    }

    void onStateChanged(StateCallback callback)
    {
        std::lock_guard<std::mutex> lock(mutex_);
        stateCallback_ = std::move(callback);
    }

    void onMediaStatus(StatusCallback callback)
    {
        std::lock_guard<std::mutex> lock(mutex_);
        statusCallback_ = std::move(callback);
    }

    void onEvent(EventCallback callback)
    {
        std::lock_guard<std::mutex> lock(mutex_);
        eventCallback_ = std::move(callback);
    }

    void onVideoFrame(VideoFrameCallback callback)
    {
        std::lock_guard<std::mutex> lock(mutex_);
        videoFrameCallback_ = std::move(callback);
    }

    void onAudioFrame(AudioFrameCallback callback)
    {
        std::lock_guard<std::mutex> lock(mutex_);
        audioFrameCallback_ = std::move(callback);
    }

    void setAudioSink(std::shared_ptr<AudioSink> sink)
    {
        {
            std::lock_guard<std::mutex> lock(mutex_);
            audioSink_ = std::move(sink);
            ++audioSinkSerial_;
        }
        controlChanged_.notify_all();
    }

    void setAudioFrameConverter(
        std::shared_ptr<AudioFrameConverter> converter)
    {
        {
            std::lock_guard<std::mutex> lock(mutex_);
            audioFrameConverter_ = std::move(converter);
            ++audioSinkSerial_;
        }
        controlChanged_.notify_all();
    }

    void setHardwareDecodeConfig(HardwareDecodeConfig config)
    {
        {
            std::lock_guard<std::mutex> lock(mutex_);
            if (hardwareDecodeConfig_.deviceType == config.deviceType
                && hardwareDecodeConfig_.allowSoftwareFallback
                    == config.allowSoftwareFallback
                && hardwareDecodeConfig_.device == config.device
                && hardwareDecodeConfig_.extraHardwareFrames
                    == config.extraHardwareFrames
                && hardwareDecodeConfig_.requireSuppliedDevice
                    == config.requireSuppliedDevice
                && hardwareDecodeConfig_.decoderWrapper
                    == config.decoderWrapper
                && hardwareDecodeConfig_.surfaceGeneration
                    == config.surfaceGeneration) {
                return;
            }
            hardwareDecodeConfig_ = config;
            ++mediaSerial_;
            loadedSerial_ = 0;
        }
        resetPlaybackQueues();
        interruptEpoch_.fetch_add(1, std::memory_order_acq_rel);
        controlChanged_.notify_all();
    }

    HardwareDecodeConfig hardwareDecodeConfig() const
    {
        std::lock_guard<std::mutex> lock(mutex_);
        return hardwareDecodeConfig_;
    }

    void setRenderCallback(RenderCallback callback)
    {
        std::lock_guard<std::mutex> lock(mutex_);
        renderCallback_ = std::move(callback);
    }

    void setVideoRenderer(VideoRenderer renderer)
    {
        std::lock_guard<std::mutex> lock(mutex_);
        videoRenderer_ = std::move(renderer);
    }

    void setVideoRenderAPI(
        std::shared_ptr<VideoRenderAPI> renderer,
        void* opaque)
    {
        std::lock_guard<std::mutex> lock(mutex_);
        if (renderer) {
            videoRenderAPIs_[opaque] = std::move(renderer);
        } else {
            videoRenderAPIs_.erase(opaque);
        }
    }

    double renderVideo(void* opaque)
    {
        VideoFrame frame;
        VideoRenderer renderer;
        std::shared_ptr<VideoRenderAPI> renderAPI;
        {
            // Rendering is a real-time, retryable operation. A seek or output
            // transition may briefly own the player state lock; waiting here
            // would stall the native render thread (and any UI operation
            // waiting for its surface lock) behind playback control work.
            std::unique_lock<std::mutex> lock(
                mutex_,
                std::try_to_lock);
            if (!lock.owns_lock()) {
                return -1.0;
            }
            frame = currentVideoFrame_;
            renderer = videoRenderer_;
            const auto found = videoRenderAPIs_.find(opaque);
            if (found != videoRenderAPIs_.end()) {
                renderAPI = found->second;
            }
        }
        if (!frame) {
            return -1.0;
        }
        if (renderAPI) {
            if (!renderAPI->render(frame)) {
                return -1.0;
            }
        } else if (renderer) {
            renderer(frame, opaque);
        }
        return static_cast<double>(frame.timestamp()) / 1000.0;
    }

    void setPlaybackRate(float value)
    {
        if (!(value > 0.0F)) {
            return;
        }
        const auto current = position();
        {
            std::lock_guard<std::mutex> lock(mutex_);
            playbackRate_ = value;
            resetClockLocked(current);
        }
        controlChanged_.notify_all();
    }

    float playbackRate() const
    {
        std::lock_guard<std::mutex> lock(mutex_);
        return playbackRate_;
    }

    void setLoop(int count)
    {
        std::lock_guard<std::mutex> lock(mutex_);
        loopCount_ = count;
        loopsCompleted_ = 0;
    }

    void setRange(std::int64_t start, std::int64_t end)
    {
        std::lock_guard<std::mutex> lock(mutex_);
        rangeStart_ = std::max<std::int64_t>(0, start);
        rangeEnd_ = end <= 0 ? MediaEnd : std::max(rangeStart_, end);
        loopsCompleted_ = 0;
    }

    void setProperty(std::string key, std::string value)
    {
        const auto originalKey = key;
        const auto originalValue = value;
        {
            std::lock_guard<std::mutex> lock(mutex_);
            properties_[std::move(key)] = std::move(value);
        }

        try {
            if (originalKey == "speed" || originalKey == "playbackRate") {
                setPlaybackRate(std::stof(originalValue));
            } else if (originalKey == "loop") {
                setLoop(std::stoi(originalValue));
            }
        } catch (const std::exception&) {
            publishEvent({
                "property.error",
                "Invalid value for property '" + originalKey + "'",
                AVERROR(EINVAL),
            });
        }
    }

    std::string property(
        const std::string& key,
        std::string defaultValue) const
    {
        std::lock_guard<std::mutex> lock(mutex_);
        const auto found = properties_.find(key);
        return found == properties_.end() ? std::move(defaultValue)
                                          : found->second;
    }

private:
    static int interruptCallback(void* opaque)
    {
        const auto* context = static_cast<InterruptContext*>(opaque);
        if (!context || !context->owner) {
            return 0;
        }
        return context->owner->quitting_.load(std::memory_order_acquire)
                || context->owner->interruptEpoch_.load(
                       std::memory_order_acquire)
                    != context->epoch
            ? 1
            : 0;
    }

    static AVPixelFormat selectHardwarePixelFormat(
        AVCodecContext* context,
        const AVPixelFormat* formats)
    {
        auto* decoder =
            context ? static_cast<Decoder*>(context->opaque) : nullptr;
        if (!decoder || !formats) {
            return AV_PIX_FMT_NONE;
        }
        for (const auto* format = formats; *format != AV_PIX_FMT_NONE;
             ++format) {
            if (*format == decoder->hardwarePixelFormat) {
                return *format;
            }
        }
        if (!decoder->allowSoftwareFallback) {
            return AV_PIX_FMT_NONE;
        }
        decoder->hardwareFallbackUsed = true;
        for (const auto* format = formats; *format != AV_PIX_FMT_NONE;
             ++format) {
            if (isSoftwarePixelFormat(*format)) {
                return *format;
            }
        }
        return AV_PIX_FMT_NONE;
    }

    void run()
    {
        while (!quitting_.load(std::memory_order_acquire)) {
            std::unique_lock<std::mutex> lock(mutex_);
            controlChanged_.wait(lock, [this] {
                return quitting_.load(std::memory_order_acquire)
                    || hasWorkLocked();
            });
            if (quitting_.load(std::memory_order_acquire)) {
                break;
            }

            if (appliedAudioSinkSerial_ != audioSinkSerial_) {
                const auto sink = audioSink_;
                const auto converter = audioFrameConverter_;
                const auto serial = audioSinkSerial_;
                lock.unlock();
                replaceAudioSink(sink, converter, serial);
                continue;
            }

            if (requestedState_ == State::Stopped) {
                const bool needsStop =
                    hasOpenMedia_ || currentState_ != State::Stopped
                    || status_ == MediaStatus::Loading;
                lock.unlock();
                if (needsStop) {
                    stopPlayback(false);
                }
                continue;
            }

            if (url_.empty()) {
                lock.unlock();
                stopPlayback(false);
                continue;
            }

            if (!hasOpenMedia_ || loadedSerial_ != mediaSerial_) {
                const auto serial = mediaSerial_;
                const auto url = url_;
                const auto prepare = prepareRequest_;
                lock.unlock();
                openForPlayback(serial, url, prepare);
                continue;
            }

            if (seekRequest_) {
                const auto request = std::move(*seekRequest_);
                seekRequest_.reset();
                lock.unlock();
                handleSeek(std::move(request));
                continue;
            }

            if (prepareRequest_) {
                const auto request = std::move(*prepareRequest_);
                prepareRequest_.reset();
                lock.unlock();
                handlePrepare(std::move(request));
                continue;
            }

            if (currentState_ != requestedState_) {
                const auto requested = requestedState_;
                const auto wasPlaying = currentState_ == State::Playing;
                lock.unlock();
                const auto transitionPosition =
                    wasPlaying ? position() : currentPosition();
                setAudioSinkPaused(requested != State::Playing);
                {
                    std::lock_guard<std::mutex> stateLock(mutex_);
                    if (wasPlaying) {
                        currentPosition_ = transitionPosition;
                    }
                    if (requested == State::Playing) {
                        if (!currentVideoFrame_ && !audioSinkOpen_) {
                            primeOutputWaitLocked(
                                currentPosition_,
                                presentationGeneration_.load(
                                    std::memory_order_acquire),
                                media_.video.valid()
                                    && media_.audio.valid(),
                                false);
                        } else {
                            resetClockLocked(currentPosition_);
                        }
                    }
                }
                publishState(requested);
                audioQueueChanged_.notify_all();
                audioQueueSpace_.notify_all();
                presentationChanged_.notify_all();
                continue;
            }

            if (currentState_ != State::Playing) {
                continue;
            }

            lock.unlock();
            const auto result = readAndDecodeOnePacket();
            if (result == DecodeResult::End || reachedRangeEnd_) {
                reachedRangeEnd_ = false;
                handlePlaybackEnd();
            } else if (result == DecodeResult::Error) {
                bool controlPending = false;
                {
                    std::lock_guard<std::mutex> stateLock(mutex_);
                    controlPending = requestedState_ != State::Playing
                        || loadedSerial_ != mediaSerial_ || seekRequest_.has_value();
                }
                if (!controlPending) {
                    stopPlayback(false, true);
                }
            }
        }
    }

    bool hasWorkLocked() const
    {
        if (appliedAudioSinkSerial_ != audioSinkSerial_) {
            return true;
        }
        if (requestedState_ == State::Stopped) {
            return hasOpenMedia_ || currentState_ != State::Stopped
                || status_ == MediaStatus::Loading
                || (url_.empty() && status_ != MediaStatus::NoMedia);
        }
        if (url_.empty()) {
            return currentState_ != State::Stopped
                || status_ != MediaStatus::NoMedia;
        }
        if (!hasOpenMedia_ || loadedSerial_ != mediaSerial_) {
            return true;
        }
        if (seekRequest_ || prepareRequest_
            || currentState_ != requestedState_) {
            return true;
        }
        return currentState_ == State::Playing;
    }

    void openForPlayback(
        std::uint64_t serial,
        const std::string& mediaUrl,
        std::optional<PrepareRequest> prepare)
    {
        resetPlaybackQueues();
        closeAudioSink(true);
        media_.reset();
        {
            std::lock_guard<std::mutex> lock(mutex_);
            hasOpenMedia_ = false;
            mediaInfo_ = {};
            currentVideoFrame_ = {};
            currentPosition_ = 0;
            loopsCompleted_ = 0;
        }
        publishStatus(MediaStatus::Loading);

        interrupt_.owner = this;
        interrupt_.epoch = interruptEpoch_.load(std::memory_order_acquire);

        auto* format = avformat_alloc_context();
        if (!format) {
            failOpen(AVERROR(ENOMEM), "Could not allocate the FFmpeg format context");
            return;
        }
        format->interrupt_callback = { &Impl::interruptCallback, &interrupt_ };

        AVDictionary* options = nullptr;
        applyHttpRecoveryDefaults(mediaUrl, &options);
        {
            std::lock_guard<std::mutex> lock(mutex_);
            for (const auto& [key, value] : properties_) {
                constexpr const char prefix[] = "avformat.";
                if (key.compare(0, sizeof(prefix) - 1, prefix) == 0) {
                    av_dict_set(
                        &options,
                        key.c_str() + sizeof(prefix) - 1,
                        value.c_str(),
                        0);
                }
            }
        }

        int error =
            avformat_open_input(&format, mediaUrl.c_str(), nullptr, &options);
        av_dict_free(&options);
        if (error < 0) {
            if (format) {
                avformat_close_input(&format);
            }
            if (wasCanceled(serial)) {
                return;
            }
            failOpen(error, "Could not open media '" + mediaUrl + "'");
            return;
        }

        media_.format = format;
        error = avformat_find_stream_info(media_.format, nullptr);
        if (error < 0) {
            if (wasCanceled(serial)) {
                media_.reset();
                return;
            }
            failOpen(error, "Could not read media stream information");
            media_.reset();
            return;
        }

        HardwareDecodeConfig hardwareDecodeConfig;
        {
            std::lock_guard<std::mutex> lock(mutex_);
            hardwareDecodeConfig = hardwareDecodeConfig_;
        }
        openBestDecoder(
            AVMEDIA_TYPE_VIDEO,
            media_.video,
            hardwareDecodeConfig);
        openBestDecoder(AVMEDIA_TYPE_AUDIO, media_.audio);
        if (!media_.video.valid() && !media_.audio.valid()) {
            failOpen(
                AVERROR_DECODER_NOT_FOUND,
                "No supported audio or video decoder was found");
            media_.reset();
            return;
        }

        const auto info = buildMediaInfo(mediaUrl);
        media_.startTimeUs =
            media_.format->start_time == AV_NOPTS_VALUE
            ? 0
            : media_.format->start_time;

        {
            std::lock_guard<std::mutex> lock(mutex_);
            if (serial != mediaSerial_ || requestedState_ == State::Stopped) {
                media_.reset();
                return;
            }
            mediaInfo_ = info;
            hasOpenMedia_ = true;
            loadedSerial_ = serial;
            currentPosition_ = prepare ? prepare->position : rangeStart_;
            resetClockLocked(currentPosition_);
            if (prepareRequest_ && prepare
                && prepareRequest_->id == prepare->id) {
                prepareRequest_.reset();
            }
        }

        std::int64_t preparedPosition = currentPosition();
        if (preparedPosition > 0) {
            const int seekError =
                seekMedia(preparedPosition, prepare ? prepare->flags
                                                    : SeekFlag::FromStart);
            if (seekError < 0) {
                publishEvent({
                    "seek.error",
                    "Initial seek failed: " + ffmpegError(seekError),
                    seekError,
                });
                preparedPosition = -1;
            }
        }

        publishStatus(MediaStatus::Loaded);
        State requested;
        {
            std::lock_guard<std::mutex> lock(mutex_);
            requested = requestedState_;
            if (requested == State::Playing) {
                const auto position =
                    std::max<std::int64_t>(0, preparedPosition);
                primeOutputWaitLocked(
                    position,
                    presentationGeneration_.load(
                        std::memory_order_acquire),
                    media_.video.valid() && media_.audio.valid(),
                    false);
            }
        }
        publishState(requested);

        if (prepare && prepare->callback) {
            bool boost = true;
            prepare->callback(preparedPosition, &boost);
        }
    }

    bool openBestDecoder(
        AVMediaType type,
        Decoder& result,
        HardwareDecodeConfig hardwareDecodeConfig = {})
    {
        result.reset();
        const AVCodec* softwareDecoder = nullptr;
        const int streamIndex =
            av_find_best_stream(
                media_.format,
                type,
                -1,
                -1,
                &softwareDecoder,
                0);
        if (streamIndex < 0 || !softwareDecoder) {
            return false;
        }

        auto* stream = media_.format->streams[streamIndex];
        const auto requestedDevice =
            type == AVMEDIA_TYPE_VIDEO
            ? hardwareDecodeConfig.deviceType
            : HardwareDeviceType::Unknown;
        const auto ffmpegDevice = ffmpegHardwareDeviceType(requestedDevice);
        const bool suppliedDeviceMismatch =
            hardwareDecodeConfig.device
            && hardwareDecodeConfig.device.deviceType() != requestedDevice;
        const bool requiredDeviceMissing =
            hardwareDecodeConfig.requireSuppliedDevice
            && !hardwareDecodeConfig.device;
        const AVCodec* hardwareDecoder = softwareDecoder;
        if (requestedDevice != HardwareDeviceType::Unknown
            && !hardwareDecodeConfig.decoderWrapper.empty()) {
            hardwareDecoder = decoderForWrapper(
                stream->codecpar->codec_id,
                type,
                hardwareDecodeConfig.decoderWrapper);
        }
        const AVCodecHWConfig* selectedHardwareConfig = nullptr;
        if (ffmpegDevice != AV_HWDEVICE_TYPE_NONE && hardwareDecoder) {
            for (int index = 0;; ++index) {
                const auto* candidate =
                    avcodec_get_hw_config(hardwareDecoder, index);
                if (!candidate) {
                    break;
                }
                if (candidate->device_type == ffmpegDevice
                    && (candidate->methods
                        & AV_CODEC_HW_CONFIG_METHOD_HW_DEVICE_CTX)
                        != 0) {
                    selectedHardwareConfig = candidate;
                    break;
                }
            }
        }

        auto openContext =
            [&](const AVCodec* decoder, bool hardware, int& error) {
            if (!decoder) {
                error = AVERROR_DECODER_NOT_FOUND;
                return static_cast<AVCodecContext*>(nullptr);
            }
            auto* context = avcodec_alloc_context3(decoder);
            if (!context) {
                error = AVERROR(ENOMEM);
                return static_cast<AVCodecContext*>(nullptr);
            }
            error =
                avcodec_parameters_to_context(context, stream->codecpar);
            if (error >= 0) {
                context->pkt_timebase = stream->time_base;
            }
            if (error >= 0 && hardware) {
                result.hardwareDeviceType = requestedDevice;
                result.hardwarePixelFormat =
                    selectedHardwareConfig->pix_fmt;
                result.hardwareNativeIdentity =
                    hardwareDecodeConfig.device.nativeIdentity();
                result.hardwareSurfaceGeneration =
                    hardwareDecodeConfig.surfaceGeneration;
                result.allowSoftwareFallback =
                    hardwareDecodeConfig.allowSoftwareFallback;
                context->opaque = &result;
                context->get_format = &Impl::selectHardwarePixelFormat;
                context->extra_hw_frames = std::clamp(
                    hardwareDecodeConfig.extraHardwareFrames,
                    0,
                    64);
                if (hardwareDecodeConfig.device) {
                    context->hw_device_ctx =
                        detail::HardwareDecodeDevicePrivate::contextRef(
                            hardwareDecodeConfig.device);
                    if (!context->hw_device_ctx) {
                        error = AVERROR(ENOMEM);
                    }
                } else {
                    error = av_hwdevice_ctx_create(
                        &context->hw_device_ctx,
                        ffmpegDevice,
                        nullptr,
                        nullptr,
                        0);
                }
            }
            if (error >= 0) {
                error = avcodec_open2(context, decoder, nullptr);
            }
            if (error < 0) {
                avcodec_free_context(&context);
            }
            return context;
        };

        int error = 0;
        AVCodecContext* context = nullptr;
        if (requestedDevice != HardwareDeviceType::Unknown) {
            if (suppliedDeviceMismatch || requiredDeviceMissing) {
                error = AVERROR(EINVAL);
            } else if (!hardwareDecoder
                || ffmpegDevice == AV_HWDEVICE_TYPE_NONE
                || !selectedHardwareConfig) {
                error = AVERROR(ENOSYS);
            } else {
                context = openContext(hardwareDecoder, true, error);
            }
            if (!context && hardwareDecodeConfig.allowSoftwareFallback) {
                const std::string requestedDecoder =
                    hardwareDecoder && hardwareDecoder->name
                    ? hardwareDecoder->name
                    : hardwareDecodeConfig.decoderWrapper.empty()
                        ? softwareDecoder->name
                        : hardwareDecodeConfig.decoderWrapper;
                publishEvent({
                    "decoder.hardware.fallback",
                    "Hardware decode is unavailable for decoder '"
                        + requestedDecoder
                        + "'; using software decode: "
                        + ffmpegError(error),
                    error,
                });
                result.reset();
                context = openContext(softwareDecoder, false, error);
            }
        } else {
            context = openContext(softwareDecoder, false, error);
        }

        if (!context) {
            const std::string requestedDecoder =
                hardwareDecoder && hardwareDecoder->name
                ? hardwareDecoder->name
                : hardwareDecodeConfig.decoderWrapper.empty()
                    ? softwareDecoder->name
                    : hardwareDecodeConfig.decoderWrapper;
            publishEvent({
                requestedDevice == HardwareDeviceType::Unknown
                    ? "decoder.error"
                    : "decoder.hardware.error",
                "Could not open "
                    + std::string(
                        requestedDevice == HardwareDeviceType::Unknown
                            ? ""
                            : "the requested hardware path for ")
                    + "decoder '" + requestedDecoder + "': "
                    + ffmpegError(error),
                error,
            });
            return false;
        }

        result.contextLifetime = std::shared_ptr<AVCodecContext>(
            context,
            [](AVCodecContext* ownedContext) {
                avcodec_free_context(&ownedContext);
            });
        result.context = result.contextLifetime.get();
        result.stream = stream;
        result.streamIndex = streamIndex;
        result.type = mediaTypeFromFFmpeg(type);
        return true;
    }

    MediaInfo buildMediaInfo(const std::string& mediaUrl) const
    {
        MediaInfo result;
        result.url = mediaUrl;
        result.startTime =
            media_.format->start_time == AV_NOPTS_VALUE
            ? 0
            : media_.format->start_time / 1000;
        result.duration =
            media_.format->duration == AV_NOPTS_VALUE
            ? 0
            : media_.format->duration / 1000;
        result.seekable =
            !media_.format->pb
            || (media_.format->pb->seekable & AVIO_SEEKABLE_NORMAL) != 0;
        result.activeVideoTrack = media_.video.streamIndex;
        result.activeAudioTrack = media_.audio.streamIndex;

        result.tracks.reserve(media_.format->nb_streams);
        for (unsigned index = 0; index < media_.format->nb_streams; ++index) {
            const auto* stream = media_.format->streams[index];
            const auto* parameters = stream->codecpar;
            TrackInfo track;
            track.index = static_cast<int>(index);
            track.type = mediaTypeFromFFmpeg(parameters->codec_type);
            track.codec = avcodec_get_name(parameters->codec_id);
            if (const auto* descriptor =
                    avcodec_descriptor_get(parameters->codec_id)) {
                track.codecDescription =
                    descriptor->long_name ? descriptor->long_name : "";
            }
            track.language = dictionaryValue(stream->metadata, "language");
            track.title = dictionaryValue(stream->metadata, "title");
            track.bitRate = parameters->bit_rate;
            track.width = parameters->width;
            track.height = parameters->height;
            track.sampleRate = parameters->sample_rate;
            track.channels = parameters->ch_layout.nb_channels;
            result.tracks.push_back(std::move(track));
        }
        return result;
    }

    void handleSeek(SeekRequest request)
    {
        const int error = seekMedia(request.position, request.flags);
        const auto result = error < 0 ? static_cast<std::int64_t>(-1)
                                     : request.position;
        if (error < 0) {
            publishEvent({
                "seek.error",
                "Seek failed: " + ffmpegError(error),
                error,
            });
        }
        if (request.callback) {
            request.callback(result);
        }
    }

    void handlePrepare(PrepareRequest request)
    {
        const int error = seekMedia(request.position, request.flags);
        const auto result = error < 0 ? static_cast<std::int64_t>(-1)
                                     : request.position;
        if (error < 0) {
            publishEvent({
                "prepare.error",
                "Prepare seek failed: " + ffmpegError(error),
                error,
            });
        }
        setAudioSinkPaused(true);
        publishState(State::Paused);
        if (request.callback) {
            bool boost = error >= 0;
            request.callback(result, &boost);
        }
    }

    int seekMedia(std::int64_t targetMs, SeekFlag flags)
    {
        if (!media_.format) {
            return AVERROR(EINVAL);
        }

        const auto previousPosition = position();
        resetPlaybackQueues();
        flushAudioSink();
        bool waitForData = false;
        {
            std::lock_guard<std::mutex> lock(mutex_);
            currentPosition_ = targetMs;
            currentVideoFrame_ = {};
            resetClockLocked(targetMs);
            waitForData = currentState_ == State::Playing
                && requestedState_ == State::Playing;
            if (waitForData) {
                primeOutputWaitLocked(
                    targetMs,
                    presentationGeneration_.load(
                        std::memory_order_acquire),
                    media_.video.valid() && media_.audio.valid(),
                    audioSinkOpen_ && audioSinkHasClock_);
            }
        }
        if (waitForData) {
            publishStatus(MediaStatus::Buffering);
        }

        interrupt_.epoch = interruptEpoch_.load(std::memory_order_acquire);
        const auto timestamp =
            media_.startTimeUs + targetMs * static_cast<std::int64_t>(1000);
        int ffmpegFlags = AVSEEK_FLAG_BACKWARD;
        if (hasFlag(flags, SeekFlag::AnyFrame)) {
            ffmpegFlags |= AVSEEK_FLAG_ANY;
        }
        if (!hasFlag(flags, SeekFlag::KeyFrame)) {
            ffmpegFlags &= ~AVSEEK_FLAG_BACKWARD;
        }

        const int error = avformat_seek_file(
            media_.format,
            -1,
            std::numeric_limits<std::int64_t>::min(),
            timestamp,
            std::numeric_limits<std::int64_t>::max(),
            ffmpegFlags);
        if (error < 0) {
            bool publishLoaded = false;
            {
                std::lock_guard<std::mutex> lock(mutex_);
                const bool superseded = seekRequest_.has_value();
                waitingForOutput_ = false;
                outputWaitPrimed_ = false;
                outputWaitGeneration_ = 0;
                outputWaitRequiresDeviceClock_ = false;
                if (!superseded) {
                    currentPosition_ = previousPosition;
                    resetClockLocked(previousPosition);
                    publishLoaded = status_ == MediaStatus::Buffering;
                }
            }
            if (publishLoaded) {
                publishStatus(MediaStatus::Loaded);
            }
            return error;
        }
        if (media_.video.valid()) {
            avcodec_flush_buffers(media_.video.context);
        }
        if (media_.audio.valid()) {
            avcodec_flush_buffers(media_.audio.context);
        }
        return 0;
    }

    enum class DecodeResult {
        Continue,
        End,
        Error,
    };

    DecodeResult readAndDecodeOnePacket()
    {
        AVPacket* packet = av_packet_alloc();
        if (!packet) {
            publishEvent({
                "decode.error",
                "Could not allocate an FFmpeg packet",
                AVERROR(ENOMEM),
            });
            return DecodeResult::Error;
        }

        const int error = av_read_frame(media_.format, packet);
        if (error == AVERROR_EOF) {
            av_packet_free(&packet);
            flushDecoder(media_.video);
            flushDecoder(media_.audio);
            return DecodeResult::End;
        }
        if (error < 0) {
            av_packet_free(&packet);
            if (error != AVERROR_EXIT) {
                publishEvent({
                    "reader.error",
                    "Could not read media packet: " + ffmpegError(error),
                    error,
                });
            }
            return DecodeResult::Error;
        }

        bool ok = true;
        if (packet->stream_index == media_.video.streamIndex) {
            ok = decodePacket(media_.video, packet);
        } else if (packet->stream_index == media_.audio.streamIndex) {
            ok = decodePacket(media_.audio, packet);
        }
        av_packet_free(&packet);
        return ok ? DecodeResult::Continue : DecodeResult::Error;
    }

    bool decodePacket(Decoder& decoder, const AVPacket* packet)
    {
        if (!decoder.valid()) {
            return true;
        }

        int error = avcodec_send_packet(decoder.context, packet);
        if (error == AVERROR(EAGAIN)) {
            if (!receiveFrames(decoder)) {
                return false;
            }
            error = avcodec_send_packet(decoder.context, packet);
        }
        if (error < 0) {
            publishEvent({
                "decode.error",
                "Could not submit packet to decoder: " + ffmpegError(error),
                error,
            });
            return false;
        }
        return receiveFrames(decoder);
    }

    void flushDecoder(Decoder& decoder)
    {
        if (!decoder.valid()) {
            return;
        }
        const int error = avcodec_send_packet(decoder.context, nullptr);
        if (error >= 0 || error == AVERROR_EOF) {
            receiveFrames(decoder);
        }
    }

    bool receiveFrames(Decoder& decoder)
    {
        AVFrame* frame = av_frame_alloc();
        if (!frame) {
            return false;
        }

        bool result = true;
        while (true) {
            const int error = avcodec_receive_frame(decoder.context, frame);
            if (error == AVERROR(EAGAIN) || error == AVERROR_EOF) {
                break;
            }
            if (error < 0) {
                publishEvent({
                    "decode.error",
                    "Could not receive decoded frame: " + ffmpegError(error),
                    error,
                });
                result = false;
                break;
            }
            if (!deliverFrame(decoder, frame)) {
                result = false;
                break;
            }
            av_frame_unref(frame);
        }
        av_frame_free(&frame);
        return result;
    }

    bool deliverFrame(Decoder& decoder, const AVFrame* frame)
    {
        if (decoder.hardwareFallbackUsed
            && !decoder.hardwareFallbackReported) {
            decoder.hardwareFallbackReported = true;
            publishEvent({
                "decoder.hardware.fallback",
                "Hardware pixel-format negotiation failed; using software "
                "decode",
                AVERROR(ENOSYS),
            });
        }

        std::int64_t timestampMs = 0;
        if (frame->best_effort_timestamp != AV_NOPTS_VALUE) {
            const auto absoluteUs = av_rescale_q(
                frame->best_effort_timestamp,
                decoder.stream->time_base,
                AV_TIME_BASE_Q);
            timestampMs =
                std::max<std::int64_t>(0, (absoluteUs - media_.startTimeUs) / 1000);
        } else {
            std::lock_guard<std::mutex> lock(mutex_);
            timestampMs = currentPosition_;
        }

        const auto frameDuration = frame->duration;
        std::int64_t durationMs =
            toMilliseconds(frameDuration, decoder.stream->time_base);
        if (durationMs <= 0 && decoder.type == MediaType::Audio
            && frame->sample_rate > 0) {
            durationMs =
                static_cast<std::int64_t>(frame->nb_samples) * 1000
                / frame->sample_rate;
        }

        std::int64_t rangeStart;
        std::int64_t rangeEnd;
        {
            std::lock_guard<std::mutex> lock(mutex_);
            rangeStart = rangeStart_;
            rangeEnd = rangeEnd_;
        }
        if (timestampMs < rangeStart) {
            return true;
        }
        if (rangeEnd != MediaEnd && timestampMs >= rangeEnd) {
            reachedRangeEnd_ = true;
            return true;
        }

        const auto generation =
            presentationGeneration_.load(std::memory_order_acquire);

        if (decoder.type == MediaType::Video) {
            if (!waitForVideoDecodeWindow(timestampMs, generation)) {
                return false;
            }
            const auto hardwareDeviceType =
                frame->format == decoder.hardwarePixelFormat
                ? decoder.hardwareDeviceType
                : HardwareDeviceType::Unknown;
            auto video = detail::FrameFactory::video(
                frame,
                timestampMs,
                durationMs,
                hardwareDeviceType,
                hardwareDeviceType
                        != HardwareDeviceType::Unknown
                    ? decoder.hardwareNativeIdentity
                    : 0,
                hardwareDeviceType
                        != HardwareDeviceType::Unknown
                    ? decoder.hardwareSurfaceGeneration
                    : 0,
                hardwareDeviceType
                        != HardwareDeviceType::Unknown
                    ? decoder.contextLifetime
                    : std::shared_ptr<void> {});
            enqueuePresentation(PresentationItem {
                PresentationItem::Type::Video,
                {},
                std::move(video),
                decoder.streamIndex,
                generation,
            });
        } else if (decoder.type == MediaType::Audio) {
            auto audio =
                detail::FrameFactory::audio(frame, timestampMs, durationMs);
            return enqueueAudioFrame(
                std::move(audio),
                decoder.streamIndex,
                generation);
        }
        return true;
    }

    enum class PresentationWaitResult {
        Cancelled,
        Ready,
        EarlierItemQueued,
    };

    bool hasEarlierQueuedPresentation(
        std::int64_t timestamp,
        std::uint64_t generation) const
    {
        std::lock_guard<std::mutex> lock(presentationMutex_);
        return std::any_of(
            presentationQueue_.begin(),
            presentationQueue_.end(),
            [timestamp, generation](const PresentationItem& item) {
                return item.generation == generation
                    && item.timestamp() < timestamp;
            });
    }

    PresentationWaitResult waitUntilPresentation(
        std::int64_t timestampMs,
        std::uint64_t generation,
        bool yieldToEarlierItem = true,
        bool claimOutputPrime = true)
    {
        std::unique_lock<std::mutex> lock(mutex_);
        while (true) {
            if (quitting_.load(std::memory_order_acquire)
                || generation
                    != presentationGeneration_.load(std::memory_order_acquire)
                || requestedState_ == State::Stopped || seekRequest_) {
                return PresentationWaitResult::Cancelled;
            }
            lock.unlock();
            refreshAudioClock(generation);
            const auto audioPosition = audioClockPosition();
            const bool earlierItemQueued =
                yieldToEarlierItem
                && hasEarlierQueuedPresentation(
                    timestampMs,
                    generation);
            lock.lock();
            if (quitting_.load(std::memory_order_acquire)
                || generation
                    != presentationGeneration_.load(std::memory_order_acquire)
                || requestedState_ == State::Stopped || seekRequest_) {
                return PresentationWaitResult::Cancelled;
            }
            if (earlierItemQueued) {
                return PresentationWaitResult::EarlierItemQueued;
            }
            const auto current =
                audioPosition ? clampPositionLocked(*audioPosition)
                              : clockPositionLocked();
            if (requestedState_ != State::Playing) {
                controlChanged_.wait(lock, [this, generation] {
                    return quitting_.load(std::memory_order_acquire)
                        || generation
                            != presentationGeneration_.load(
                                std::memory_order_acquire)
                        || requestedState_ != State::Paused;
                });
                continue;
            }
            if (videoPrerollPending_
                && videoPrerollGeneration_ == generation) {
                if (Clock::now() >= videoPrerollDeadline_) {
                    videoPrerollPending_ = false;
                    videoPrerollGeneration_ = 0;
                    videoPrerollFrameCount_ = 0;
                } else {
                    controlChanged_.wait_until(
                        lock,
                        videoPrerollDeadline_);
                    continue;
                }
            }
            if (waitingForOutput_ && !outputWaitPrimed_) {
                if (claimOutputPrime) {
                    outputWaitPrimed_ = true;
                }
                return PresentationWaitResult::Ready;
            }
            const auto delta = timestampMs - current;
            if (delta <= 2) {
                return PresentationWaitResult::Ready;
            }
            const auto waitMs = std::clamp<std::int64_t>(
                static_cast<std::int64_t>(
                    static_cast<double>(delta) / playbackRate_),
                1,
                yieldToEarlierItem ? 10 : 100);
            controlChanged_.wait_for(lock, Milliseconds(waitMs));
        }
    }

    bool waitForVideoDecodeWindow(
        std::int64_t timestampMs,
        std::uint64_t generation)
    {
        std::unique_lock<std::mutex> lock(mutex_);
        while (true) {
            if (quitting_.load(std::memory_order_acquire)
                || generation
                    != presentationGeneration_.load(std::memory_order_acquire)
                || requestedState_ != State::Playing || seekRequest_) {
                return false;
            }
            lock.unlock();
            refreshAudioClock(generation);
            const auto audioPosition = audioClockPosition();
            lock.lock();
            if (quitting_.load(std::memory_order_acquire)
                || generation
                    != presentationGeneration_.load(std::memory_order_acquire)
                || requestedState_ != State::Playing || seekRequest_) {
                return false;
            }
            if (waitingForOutput_ && !outputWaitPrimed_) {
                return true;
            }
            const auto current =
                audioPosition ? clampPositionLocked(*audioPosition)
                              : clockPositionLocked();
            const auto lead = timestampMs - current;
            if (lead <= kMaximumVideoDecodeLeadMilliseconds) {
                return true;
            }
            controlChanged_.wait_for(
                lock,
                Milliseconds(std::clamp<std::int64_t>(
                    lead - kMaximumVideoDecodeLeadMilliseconds,
                    1,
                    50)));
        }
    }

    bool enqueueAudioFrame(
        AudioFrame frame,
        int track,
        std::uint64_t generation)
    {
        const auto duration =
            std::max<std::int64_t>(1, frame.duration());
        while (true) {
            if (quitting_.load(std::memory_order_acquire)
                || generation
                    != presentationGeneration_.load(std::memory_order_acquire)) {
                return false;
            }
            {
                std::lock_guard<std::mutex> lock(mutex_);
                if (requestedState_ != State::Playing || seekRequest_) {
                    return false;
                }
            }

            std::unique_lock<std::mutex> lock(audioQueueMutex_);
            const bool fits =
                audioQueue_.empty()
                || queuedAudioDuration_ + duration
                    <= kMaximumQueuedAudioMilliseconds;
            if (fits) {
                audioQueue_.push_back({
                    std::move(frame),
                    track,
                    generation,
                    duration,
                });
                queuedAudioDuration_ += duration;
                lock.unlock();
                audioQueueChanged_.notify_one();
                return true;
            }
            audioQueueSpace_.wait_for(lock, Milliseconds(20));
        }
    }

    void enqueuePresentation(PresentationItem item)
    {
        if (item.generation
            != presentationGeneration_.load(std::memory_order_acquire)) {
            return;
        }
        if (item.type == PresentationItem::Type::Video) {
            completeVideoPreroll(item.generation);
        }

        std::lock_guard<std::mutex> lock(presentationMutex_);
        const auto maximumQueuedVideoFrames =
            item.type == PresentationItem::Type::Video
                && item.video.hasHardwareFrame()
            ? kMaximumQueuedHardwareVideoFrames
            : kMaximumQueuedSoftwareVideoFrames;
        if (item.type == PresentationItem::Type::Video
            && queuedVideoFrames_ >= maximumQueuedVideoFrames) {
            // Keep the contiguous near-term presentation window. Evicting its
            // oldest frame lets decode bursts, especially after seek, replace
            // frames that are about to be shown with farther-future frames.
            return;
        }
        const auto insertion = std::upper_bound(
            presentationQueue_.begin(),
            presentationQueue_.end(),
            item.timestamp(),
            [](std::int64_t timestamp, const PresentationItem& candidate) {
                return timestamp < candidate.timestamp();
            });
        if (item.type == PresentationItem::Type::Video) {
            ++queuedVideoFrames_;
        }
        presentationQueue_.insert(insertion, std::move(item));
        presentationChanged_.notify_one();
    }

    bool requestedPlaying() const
    {
        std::lock_guard<std::mutex> lock(mutex_);
        return requestedState_ == State::Playing
            && currentState_ == State::Playing && !seekRequest_;
    }

    bool hasActiveAudioDeviceClock() const
    {
        std::lock_guard<std::mutex> lock(mutex_);
        return audioSinkOpen_ && audioSinkHasClock_;
    }

    void runAudioOutput()
    {
        while (!quitting_.load(std::memory_order_acquire)) {
            QueuedAudioFrame queued;
            {
                std::unique_lock<std::mutex> lock(audioQueueMutex_);
                audioQueueChanged_.wait(lock, [this] {
                    return quitting_.load(std::memory_order_acquire)
                        || !audioQueue_.empty();
                });
                if (quitting_.load(std::memory_order_acquire)) {
                    break;
                }
                if (!requestedPlaying()) {
                    audioQueueChanged_.wait_for(lock, Milliseconds(20));
                    continue;
                }
                queued = std::move(audioQueue_.front());
                audioQueue_.pop_front();
                queuedAudioDuration_ =
                    std::max<std::int64_t>(
                        0,
                        queuedAudioDuration_ - queued.duration);
                audioFrameInFlight_ = true;
            }
            audioQueueSpace_.notify_all();

            bool accepted = queued.generation
                == presentationGeneration_.load(std::memory_order_acquire);
            if (accepted) {
                accepted = waitForVideoPreroll(queued.generation);
            }
            if (accepted) {
                deliverAudioToSink(queued.frame, queued.generation);
                if (!hasActiveAudioDeviceClock()) {
                    accepted =
                        waitUntilPresentation(
                            queued.frame.timestamp(),
                            queued.generation,
                            true,
                            false)
                        == PresentationWaitResult::Ready;
                }
            }
            if (accepted
                && queued.generation
                    == presentationGeneration_.load(
                        std::memory_order_acquire)) {
                enqueuePresentation(PresentationItem {
                    PresentationItem::Type::Audio,
                    std::move(queued.frame),
                    {},
                    queued.track,
                    queued.generation,
                });
            }

            {
                std::lock_guard<std::mutex> lock(audioQueueMutex_);
                audioFrameInFlight_ = false;
            }
            audioQueueSpace_.notify_all();
            audioQueueDrained_.notify_all();
        }

        {
            std::lock_guard<std::mutex> lock(audioQueueMutex_);
            audioFrameInFlight_ = false;
        }
        audioQueueDrained_.notify_all();
    }

    bool hasTimelyNewerQueuedVideo(
        std::int64_t timestamp,
        std::int64_t current,
        std::uint64_t generation) const
    {
        std::lock_guard<std::mutex> lock(presentationMutex_);
        return std::any_of(
            presentationQueue_.begin(),
            presentationQueue_.end(),
            [timestamp, current, generation](
                const PresentationItem& item) {
                return item.type == PresentationItem::Type::Video
                    && item.generation == generation
                    && item.timestamp() > timestamp
                    && current - item.timestamp()
                        <= kLateVideoFrameThresholdMilliseconds;
            });
    }

    void present(PresentationItem& item)
    {
        if (item.generation
            != presentationGeneration_.load(std::memory_order_acquire)) {
            return;
        }

        if (item.type == PresentationItem::Type::Audio) {
            AudioFrameCallback callback;
            {
                std::lock_guard<std::mutex> lock(mutex_);
                if (item.generation
                    != presentationGeneration_.load(
                        std::memory_order_acquire)) {
                    return;
                }
                currentPosition_ =
                    std::max(currentPosition_, item.audio.timestamp());
                callback = audioFrameCallback_;
            }
            if (callback) {
                callback(item.audio, item.track);
            }
            if (!hasActiveAudioDeviceClock()) {
                resumeClockAfterOutput(
                    item.generation,
                    item.audio.timestamp());
            }
            return;
        }

        VideoFrameCallback frameCallback;
        RenderCallback renderCallback;
        std::vector<void*> renderKeys;
        {
            std::lock_guard<std::mutex> lock(mutex_);
            if (item.generation
                != presentationGeneration_.load(std::memory_order_acquire)) {
                return;
            }
            currentPosition_ =
                std::max(currentPosition_, item.video.timestamp());
            currentVideoFrame_ = item.video;
            frameCallback = videoFrameCallback_;
            renderCallback = renderCallback_;
            const bool scheduleLegacyRenderer = videoRenderer_
                && videoRenderAPIs_.find(nullptr) == videoRenderAPIs_.end();
            renderKeys.reserve(
                videoRenderAPIs_.size()
                + (scheduleLegacyRenderer ? 1U : 0U));
            if (scheduleLegacyRenderer) {
                renderKeys.push_back(nullptr);
            }
            for (const auto& entry : videoRenderAPIs_) {
                renderKeys.push_back(entry.first);
            }
        }
        if (frameCallback) {
            frameCallback(item.video, item.track);
        }
        if (!renderCallback
            || item.generation
                != presentationGeneration_.load(std::memory_order_acquire)) {
            if (!hasActiveAudioDeviceClock()) {
                resumeClockAfterOutput(
                    item.generation,
                    item.video.timestamp());
            }
            return;
        }
        if (renderKeys.empty()) {
            renderCallback(nullptr);
        } else {
            for (void* key : renderKeys) {
                renderCallback(key);
            }
        }
        if (!hasActiveAudioDeviceClock()) {
            resumeClockAfterOutput(
                item.generation,
                item.video.timestamp());
        }
    }

    void runPresentation()
    {
        while (!quitting_.load(std::memory_order_acquire)) {
            PresentationItem item;
            {
                std::unique_lock<std::mutex> lock(presentationMutex_);
                presentationChanged_.wait(lock, [this] {
                    return quitting_.load(std::memory_order_acquire)
                        || !presentationQueue_.empty();
                });
                if (quitting_.load(std::memory_order_acquire)) {
                    break;
                }
                item = std::move(presentationQueue_.front());
                presentationQueue_.pop_front();
                if (item.type == PresentationItem::Type::Video) {
                    --queuedVideoFrames_;
                }
                presentationInFlight_ = true;
            }

            const auto waitResult = waitUntilPresentation(
                item.timestamp(),
                item.generation);
            if (waitResult
                == PresentationWaitResult::EarlierItemQueued) {
                enqueuePresentation(std::move(item));
            } else if (waitResult == PresentationWaitResult::Ready) {
                const auto current = position();
                const bool dropLateVideo =
                    item.type == PresentationItem::Type::Video
                    && current - item.timestamp()
                        > kLateVideoFrameThresholdMilliseconds
                    && hasTimelyNewerQueuedVideo(
                        item.timestamp(),
                        current,
                        item.generation);
                if (!dropLateVideo) {
                    present(item);
                }
            }

            {
                std::lock_guard<std::mutex> lock(presentationMutex_);
                presentationInFlight_ = false;
            }
            presentationDrained_.notify_all();
        }

        {
            std::lock_guard<std::mutex> lock(presentationMutex_);
            presentationInFlight_ = false;
        }
        presentationDrained_.notify_all();
    }

    void invalidateAudioClock()
    {
        {
            std::lock_guard<std::mutex> lock(audioClockMutex_);
            cachedAudioClock_ = {};
        }
        controlChanged_.notify_all();
        presentationChanged_.notify_all();
    }

    void invalidatePlaybackQueues()
    {
        presentationGeneration_.fetch_add(1, std::memory_order_acq_rel);
        invalidateAudioClock();
        audioQueueChanged_.notify_all();
        audioQueueSpace_.notify_all();
        audioQueueDrained_.notify_all();
        presentationChanged_.notify_all();
        presentationDrained_.notify_all();
    }

    void resetPlaybackQueues()
    {
        invalidatePlaybackQueues();
        {
            std::lock_guard<std::mutex> lock(audioQueueMutex_);
            audioQueue_.clear();
            queuedAudioDuration_ = 0;
        }
        {
            std::lock_guard<std::mutex> lock(presentationMutex_);
            presentationQueue_.clear();
            queuedVideoFrames_ = 0;
        }
        {
            std::lock_guard<std::mutex> lock(mutex_);
            currentVideoFrame_ = {};
            waitingForOutput_ = false;
            outputWaitPrimed_ = false;
            outputWaitGeneration_ = 0;
            outputWaitRequiresDeviceClock_ = false;
            videoPrerollPending_ = false;
            videoPrerollGeneration_ = 0;
            videoPrerollFrameCount_ = 0;
        }
        audioQueueChanged_.notify_all();
        audioQueueSpace_.notify_all();
        audioQueueDrained_.notify_all();
        presentationChanged_.notify_all();
        presentationDrained_.notify_all();
    }

    void beginOutputWait(
        std::uint64_t generation,
        std::int64_t position)
    {
        bool publishBuffering = false;
        {
            std::lock_guard<std::mutex> lock(mutex_);
            if (generation
                    != presentationGeneration_.load(
                        std::memory_order_acquire)
                || currentState_ != State::Playing
                || requestedState_ != State::Playing
                || seekRequest_) {
                return;
            }
            primeOutputWaitLocked(
                position,
                generation,
                false,
                audioSinkOpen_ && audioSinkHasClock_);
            publishBuffering = status_ == MediaStatus::Loaded;
        }
        if (publishBuffering) {
            publishStatus(MediaStatus::Buffering);
        }
        controlChanged_.notify_all();
        presentationChanged_.notify_all();
    }

    void primeOutputWaitLocked(
        std::int64_t position,
        std::uint64_t generation,
        bool waitForVideo,
        bool requireDeviceClock)
    {
        currentPosition_ = clampPositionLocked(position);
        resetClockLocked(currentPosition_);
        waitingForOutput_ = true;
        outputWaitPrimed_ = false;
        outputWaitGeneration_ = generation;
        outputWaitRequiresDeviceClock_ = requireDeviceClock;
        videoPrerollPending_ = waitForVideo;
        videoPrerollGeneration_ = waitForVideo ? generation : 0;
        videoPrerollFrameCount_ = 0;
        videoPrerollDeadline_ =
            Clock::now()
            + Milliseconds(kMaximumVideoPrerollWaitMilliseconds);
    }

    bool waitForVideoPreroll(std::uint64_t generation)
    {
        std::unique_lock<std::mutex> lock(mutex_);
        while (videoPrerollPending_
               && videoPrerollGeneration_ == generation) {
            if (quitting_.load(std::memory_order_acquire)
                || generation
                    != presentationGeneration_.load(
                        std::memory_order_acquire)
                || requestedState_ != State::Playing
                || seekRequest_) {
                return false;
            }
            if (Clock::now() >= videoPrerollDeadline_) {
                videoPrerollPending_ = false;
                videoPrerollGeneration_ = 0;
                videoPrerollFrameCount_ = 0;
                return true;
            }
            controlChanged_.wait_until(lock, videoPrerollDeadline_);
        }
        return generation
            == presentationGeneration_.load(std::memory_order_acquire)
            && requestedState_ == State::Playing && !seekRequest_;
    }

    void completeVideoPreroll(std::uint64_t generation)
    {
        {
            std::lock_guard<std::mutex> lock(mutex_);
            if (!videoPrerollPending_
                || videoPrerollGeneration_ != generation) {
                return;
            }
            ++videoPrerollFrameCount_;
            if (videoPrerollFrameCount_
                < kMinimumVideoPrerollFrames) {
                return;
            }
            videoPrerollPending_ = false;
            videoPrerollGeneration_ = 0;
            videoPrerollFrameCount_ = 0;
        }
        controlChanged_.notify_all();
        audioQueueChanged_.notify_all();
    }

    void resumeClockAfterOutput(
        std::uint64_t generation,
        std::int64_t position)
    {
        bool publishLoaded = false;
        {
            std::lock_guard<std::mutex> lock(mutex_);
            if (!waitingForOutput_
                || outputWaitGeneration_ != generation
                || seekRequest_) {
                return;
            }
            currentPosition_ = std::max(
                currentPosition_,
                clampPositionLocked(position));
            waitingForOutput_ = false;
            outputWaitPrimed_ = false;
            outputWaitGeneration_ = 0;
            outputWaitRequiresDeviceClock_ = false;
            videoPrerollPending_ = false;
            videoPrerollGeneration_ = 0;
            videoPrerollFrameCount_ = 0;
            resetClockLocked(currentPosition_);
            publishLoaded = status_ == MediaStatus::Buffering;
        }
        if (publishLoaded) {
            publishStatus(MediaStatus::Loaded);
        }
    }

    bool waitForAudioQueueDrained(std::uint64_t generation)
    {
        while (!quitting_.load(std::memory_order_acquire)
               && generation
                   == presentationGeneration_.load(std::memory_order_acquire)) {
            {
                std::lock_guard<std::mutex> lock(audioQueueMutex_);
                if (audioQueue_.empty() && !audioFrameInFlight_) {
                    return true;
                }
            }
            if (!requestedPlaying()) {
                return false;
            }
            std::unique_lock<std::mutex> lock(audioQueueMutex_);
            audioQueueDrained_.wait_for(lock, Milliseconds(20));
        }
        return false;
    }

    bool waitForPresentationDrained(std::uint64_t generation)
    {
        while (!quitting_.load(std::memory_order_acquire)
               && generation
                   == presentationGeneration_.load(std::memory_order_acquire)) {
            {
                std::lock_guard<std::mutex> lock(presentationMutex_);
                if (presentationQueue_.empty() && !presentationInFlight_) {
                    return true;
                }
            }
            if (!requestedPlaying()) {
                return false;
            }
            std::unique_lock<std::mutex> lock(presentationMutex_);
            presentationDrained_.wait_for(lock, Milliseconds(20));
        }
        return false;
    }

    void cacheAudioClockSample(
        const AudioSinkClock& value,
        std::uint64_t serial,
        std::int64_t submittedUntil,
        std::uint64_t generation)
    {
        if (generation
            != presentationGeneration_.load(std::memory_order_acquire)) {
            return;
        }
        std::optional<std::int64_t> validPosition;
        {
            std::lock_guard<std::mutex> lock(audioClockMutex_);
            if (cachedAudioClock_.sinkSerial != serial) {
                cachedAudioClock_ = {};
                cachedAudioClock_.sinkSerial = serial;
            }
            cachedAudioClock_.submittedUntil = std::max(
                cachedAudioClock_.submittedUntil,
                submittedUntil);
            if (value.valid) {
                const auto now = Clock::now();
                auto position =
                    std::max<std::int64_t>(0, value.positionMilliseconds);
                if (cachedAudioClock_.valid) {
                    const auto elapsed = std::max<std::int64_t>(
                        0,
                        std::chrono::duration_cast<Milliseconds>(
                            now - cachedAudioClock_.sampledAt)
                            .count());
                    auto extrapolated =
                        cachedAudioClock_.position + elapsed;
                    if (cachedAudioClock_.submittedUntil > 0) {
                        extrapolated = std::min(
                            extrapolated,
                            cachedAudioClock_.submittedUntil);
                    }
                    position = std::max(position, extrapolated);
                }
                if (cachedAudioClock_.submittedUntil > 0) {
                    position = std::min(
                        position,
                        cachedAudioClock_.submittedUntil);
                }
                cachedAudioClock_.valid = true;
                cachedAudioClock_.position = position;
                cachedAudioClock_.submittedUntil = std::max(
                    cachedAudioClock_.submittedUntil,
                    cachedAudioClock_.position);
                cachedAudioClock_.sampledAt = now;
                validPosition = cachedAudioClock_.position;
            }
        }
        if (validPosition) {
            resumeClockAfterOutput(generation, *validPosition);
        } else if (submittedUntil > 0) {
            bool useFallbackClock = false;
            {
                std::lock_guard<std::mutex> lock(mutex_);
                useFallbackClock = waitingForOutput_
                    && outputWaitGeneration_ == generation
                    && !outputWaitRequiresDeviceClock_;
            }
            if (useFallbackClock) {
                resumeClockAfterOutput(generation, submittedUntil);
            }
        }
        controlChanged_.notify_all();
        presentationChanged_.notify_all();
    }

    void completeCachedAudioClock()
    {
        {
            std::lock_guard<std::mutex> lock(audioClockMutex_);
            if (!cachedAudioClock_.valid) {
                return;
            }
            cachedAudioClock_.position = std::max(
                cachedAudioClock_.position,
                cachedAudioClock_.submittedUntil);
            cachedAudioClock_.sampledAt = Clock::now();
        }
        controlChanged_.notify_all();
        presentationChanged_.notify_all();
    }

    void replaceAudioSink(
        const std::shared_ptr<AudioSink>& sink,
        const std::shared_ptr<AudioFrameConverter>& converter,
        std::uint64_t serial)
    {
        invalidateAudioClock();
        std::shared_ptr<AudioSink> previous;
        std::shared_ptr<AudioFrameConverter> previousConverter;
        bool previousOpen = false;
        bool previousConverterOpen = false;
        {
            std::lock_guard<std::mutex> lock(mutex_);
            previous = activeAudioSink_;
            previousConverter = activeAudioFrameConverter_;
            previousOpen = audioSinkOpen_;
            previousConverterOpen = audioFrameConverterOpen_;
            activeAudioSink_.reset();
            activeAudioFrameConverter_.reset();
            audioSinkOpen_ = false;
            audioSinkOpenAttempted_ = false;
            audioSinkHasClock_ = false;
            audioFrameConverterOpen_ = false;
        }

        if (previous || previousConverter) {
            std::lock_guard<std::mutex> sinkLock(audioSinkCallMutex_);
            if (previous) {
                previous->setEventCallback({});
                if (previousOpen) {
                    previous->flush();
                }
            }
            if (previousConverter && previousConverterOpen) {
                previousConverter->reset();
                previousConverter->close();
            }
            if (previous && previousOpen) {
                previous->close();
            }
        }

        {
            std::lock_guard<std::mutex> lock(mutex_);
            activeAudioSink_ = sink;
            activeAudioFrameConverter_ = converter;
            appliedAudioSinkSerial_ = serial;
        }
        if (sink) {
            sink->setEventCallback(
                [bridge = audioSinkCallbackBridge_,
                 weakSink = std::weak_ptr<AudioSink>(sink),
                 serial](const AudioSinkEvent& event) {
                    if (const auto source = weakSink.lock()) {
                        std::lock_guard<std::mutex> lock(bridge->mutex);
                        if (bridge->owner) {
                            bridge->owner->publishAudioSinkEvent(
                                source.get(),
                                serial,
                                event);
                        }
                    }
                });
        }
    }

    void closeAudioSink(bool flush)
    {
        invalidateAudioClock();
        std::shared_ptr<AudioSink> sink;
        std::shared_ptr<AudioFrameConverter> converter;
        bool wasOpen = false;
        bool converterWasOpen = false;
        {
            std::lock_guard<std::mutex> lock(mutex_);
            sink = activeAudioSink_;
            converter = activeAudioFrameConverter_;
            wasOpen = audioSinkOpen_;
            converterWasOpen = audioFrameConverterOpen_;
            audioSinkOpen_ = false;
            audioSinkOpenAttempted_ = false;
            audioSinkHasClock_ = false;
            audioFrameConverterOpen_ = false;
        }
        if ((!sink || !wasOpen) && (!converter || !converterWasOpen)) {
            return;
        }
        std::lock_guard<std::mutex> sinkLock(audioSinkCallMutex_);
        if (sink && wasOpen && flush) {
            sink->flush();
        }
        if (converter && converterWasOpen) {
            if (flush) {
                converter->reset();
            }
            converter->close();
        }
        if (sink && wasOpen) {
            sink->close();
        }
    }

    void flushAudioSink()
    {
        invalidateAudioClock();
        std::shared_ptr<AudioSink> sink;
        std::shared_ptr<AudioFrameConverter> converter;
        bool converterOpen = false;
        {
            std::lock_guard<std::mutex> lock(mutex_);
            if (!audioSinkOpen_) {
                return;
            }
            sink = activeAudioSink_;
            converter = activeAudioFrameConverter_;
            converterOpen = audioFrameConverterOpen_;
        }
        bool reset = true;
        if (sink) {
            std::lock_guard<std::mutex> sinkLock(audioSinkCallMutex_);
            sink->flush();
            if (converter && converterOpen) {
                reset = converter->reset();
            }
        }
        if (!reset) {
            closeAudioSink(false);
            publishEvent({
                "audio.converter.reset",
                "The audio converter could not be reset",
                AVERROR_EXTERNAL,
            });
        }
    }

    void setAudioSinkPaused(bool paused)
    {
        if (paused) {
            invalidateAudioClock();
        }
        std::shared_ptr<AudioSink> sink;
        {
            std::lock_guard<std::mutex> lock(mutex_);
            if (!audioSinkOpen_) {
                return;
            }
            sink = activeAudioSink_;
        }
        if (sink) {
            std::lock_guard<std::mutex> sinkLock(audioSinkCallMutex_);
            sink->pause(paused);
        }
    }

    void deliverAudioToSink(
        const AudioFrame& frame,
        std::uint64_t generation)
    {
        std::shared_ptr<AudioSink> sink;
        std::shared_ptr<AudioFrameConverter> converter;
        std::uint64_t serial = 0;
        bool open = false;
        bool attempted = false;
        bool converterOpen = false;
        bool hasClock = false;
        {
            std::lock_guard<std::mutex> lock(mutex_);
            if (requestedState_ != State::Playing) {
                return;
            }
            sink = activeAudioSink_;
            converter = activeAudioFrameConverter_;
            serial = appliedAudioSinkSerial_;
            open = audioSinkOpen_;
            attempted = audioSinkOpenAttempted_;
            converterOpen = audioFrameConverterOpen_;
            hasClock = audioSinkHasClock_;
        }
        if (!sink) {
            return;
        }

        const auto decodedFormat = audioFormat(frame);
        if (!open && !attempted) {
            AudioSinkCapabilities capabilities;
            AudioSinkOpenResult result;
            AudioConverterOpenResult converterResult;
            bool conversionNeeded = false;
            bool converterOpened = false;
            {
                std::lock_guard<std::mutex> sinkLock(audioSinkCallMutex_);
                capabilities = sink->capabilities();
                result = sink->open(decodedFormat);
                conversionNeeded = result.success
                    && result.deviceFormat.isValid()
                    && !sameAudioFormat(
                        decodedFormat,
                        result.deviceFormat);
                if (conversionNeeded && converter) {
                    converterResult =
                        converter->open(decodedFormat, result.deviceFormat);
                    converterOpened = converterResult.success;
                }
            }

            bool formatSupported = result.success
                && result.deviceFormat.isValid()
                && (!conversionNeeded || converterOpened);
            bool stillActive = false;
            {
                std::lock_guard<std::mutex> lock(mutex_);
                stillActive = activeAudioSink_ == sink
                    && activeAudioFrameConverter_ == converter
                    && appliedAudioSinkSerial_ == serial
                    && audioSinkSerial_ == serial
                    && requestedState_ == State::Playing
                    && generation
                        == presentationGeneration_.load(
                            std::memory_order_acquire);
                if (stillActive) {
                    audioSinkOpenAttempted_ = true;
                    audioSinkOpen_ = formatSupported;
                    audioSinkHasClock_ =
                        formatSupported && capabilities.hasDeviceClock;
                    audioFrameConverterOpen_ =
                        formatSupported && converterOpened;
                }
            }

            if (!stillActive || (result.success && !formatSupported)) {
                std::lock_guard<std::mutex> sinkLock(audioSinkCallMutex_);
                if (converter && converterOpened) {
                    converter->close();
                }
                if (result.success) {
                    sink->close();
                }
            }
            if (!stillActive) {
                return;
            }
            if (!result.success) {
                publishEvent({
                    "audio.sink.open",
                    result.error.empty()
                        ? "The audio sink could not be opened"
                        : result.error,
                    AVERROR_EXTERNAL,
                });
                return;
            }
            if (!formatSupported) {
                if (!result.deviceFormat.isValid()) {
                    publishEvent({
                        "audio.sink.format",
                        "The audio sink returned an invalid device format",
                        AVERROR(EINVAL),
                    });
                } else if (!converter) {
                    publishEvent({
                        "audio.sink.format",
                        "The audio sink requires PCM conversion, but no "
                        "audio converter is connected",
                        AVERROR(ENOSYS),
                    });
                } else {
                    publishEvent({
                        "audio.converter.open",
                        converterResult.error.empty()
                            ? "The audio converter could not be opened"
                            : converterResult.error,
                        AVERROR_EXTERNAL,
                    });
                }
                return;
            }

            {
                std::lock_guard<std::mutex> sinkLock(audioSinkCallMutex_);
                sink->pause(false);
            }
            open = true;
            converterOpen = converterOpened;
            hasClock = capabilities.hasDeviceClock;
        }

        if (!open) {
            return;
        }

        AudioConversionResult conversion {
            true,
            audioBufferView(frame),
            {},
        };
        bool written = true;
        AudioSinkClock clockBefore;
        AudioSinkClock clockAfter;
        std::int64_t submittedUntil = 0;
        {
            std::lock_guard<std::mutex> sinkLock(audioSinkCallMutex_);
            if (hasClock) {
                clockBefore = sink->clock();
            }
            if (converter && converterOpen) {
                conversion = converter->convert(frame);
            }
            if (conversion.success && conversion.buffer.isValid()) {
                written = sink->write(conversion.buffer);
                if (written) {
                    submittedUntil = conversion.buffer.timestamp
                        + std::max<std::int64_t>(
                            0,
                            conversion.buffer.duration);
                }
            }
            if (hasClock) {
                clockAfter = sink->clock();
            }
        }
        if (hasClock) {
            cacheAudioClockSample(clockBefore, serial, 0, generation);
            cacheAudioClockSample(
                clockAfter,
                serial,
                written ? submittedUntil : 0,
                generation);
        }
        if (!conversion.success) {
            closeAudioSink(false);
            {
                std::lock_guard<std::mutex> lock(mutex_);
                if (activeAudioSink_ == sink
                    && appliedAudioSinkSerial_ == serial) {
                    audioSinkOpenAttempted_ = true;
                }
            }
            publishEvent({
                "audio.converter.convert",
                conversion.error.empty()
                    ? "The audio converter rejected a decoded frame"
                    : conversion.error,
                AVERROR_EXTERNAL,
            });
            return;
        }
        if (!conversion.buffer.isValid()) {
            return;
        }
        if (!written) {
            closeAudioSink(false);
            {
                std::lock_guard<std::mutex> lock(mutex_);
                if (activeAudioSink_ == sink
                    && appliedAudioSinkSerial_ == serial) {
                    audioSinkOpenAttempted_ = true;
                }
            }
            publishEvent({
                "audio.sink.write",
                "The audio sink rejected a decoded audio buffer",
                AVERROR_EXTERNAL,
            });
        }
    }

    bool drainAudioConverter()
    {
        std::shared_ptr<AudioSink> sink;
        std::shared_ptr<AudioFrameConverter> converter;
        std::uint64_t serial = 0;
        bool hasClock = false;
        {
            std::lock_guard<std::mutex> lock(mutex_);
            if (!audioSinkOpen_ || !audioFrameConverterOpen_) {
                return true;
            }
            sink = activeAudioSink_;
            converter = activeAudioFrameConverter_;
            serial = appliedAudioSinkSerial_;
            hasClock = audioSinkHasClock_;
        }
        if (!sink || !converter) {
            return true;
        }

        for (int iteration = 0; iteration < 32; ++iteration) {
            AudioConversionResult result;
            bool written = true;
            AudioSinkClock clockValue;
            std::int64_t submittedUntil = 0;
            {
                std::lock_guard<std::mutex> sinkLock(audioSinkCallMutex_);
                result = converter->drain();
                if (result.success && result.buffer.isValid()) {
                    written = sink->write(result.buffer);
                    if (written) {
                        submittedUntil = result.buffer.timestamp
                            + std::max<std::int64_t>(
                                0,
                                result.buffer.duration);
                    }
                }
                if (hasClock) {
                    clockValue = sink->clock();
                }
            }
            if (hasClock) {
                cacheAudioClockSample(
                    clockValue,
                    serial,
                    written ? submittedUntil : 0,
                    presentationGeneration_.load(
                        std::memory_order_acquire));
            }
            if (!result.success) {
                publishEvent({
                    "audio.converter.drain",
                    result.error.empty()
                        ? "The audio converter could not be drained"
                        : result.error,
                    AVERROR_EXTERNAL,
                });
                return false;
            }
            if (!result.buffer.isValid()) {
                return true;
            }
            if (!written) {
                publishEvent({
                    "audio.sink.write",
                    "The audio sink rejected a drained audio buffer",
                    AVERROR_EXTERNAL,
                });
                return false;
            }
        }
        publishEvent({
            "audio.converter.drain",
            "The audio converter did not finish draining",
            AVERROR_EXTERNAL,
        });
        return false;
    }

    bool drainAudioSink()
    {
        std::shared_ptr<AudioSink> sink;
        {
            std::lock_guard<std::mutex> lock(mutex_);
            if (!audioSinkOpen_) {
                return true;
            }
            sink = activeAudioSink_;
        }
        if (!sink) {
            return true;
        }

        bool drained = false;
        {
            std::lock_guard<std::mutex> sinkLock(audioSinkCallMutex_);
            drained = sink->drain();
        }
        if (drained) {
            completeCachedAudioClock();
        }
        if (!drained) {
            publishEvent({
                "audio.sink.drain",
                "The audio sink could not drain its queued buffers",
                AVERROR_EXTERNAL,
            });
        }
        return drained;
    }

    void refreshAudioClock(std::uint64_t generation)
    {
        std::shared_ptr<AudioSink> sink;
        std::uint64_t serial = 0;
        {
            std::lock_guard<std::mutex> lock(mutex_);
            if (!audioSinkOpen_ || !audioSinkHasClock_
                || generation
                    != presentationGeneration_.load(
                        std::memory_order_acquire)) {
                return;
            }
            sink = activeAudioSink_;
            serial = appliedAudioSinkSerial_;
        }
        if (!sink) {
            return;
        }

        AudioSinkClock value;
        {
            // A device write may legitimately wait for backend queue space
            // while holding this serialization lock. Presentation must keep
            // using the cached clock instead of stalling behind that write.
            std::unique_lock<std::mutex> sinkLock(
                audioSinkCallMutex_,
                std::try_to_lock);
            if (!sinkLock.owns_lock()) {
                return;
            }
            value = sink->clock();
        }
        cacheAudioClockSample(value, serial, 0, generation);
    }

    std::optional<std::int64_t> audioClockPosition() const
    {
        CachedAudioClock cached;
        {
            std::lock_guard<std::mutex> lock(audioClockMutex_);
            cached = cachedAudioClock_;
        }
        if (!cached.valid) {
            return std::nullopt;
        }

        {
            std::lock_guard<std::mutex> lock(mutex_);
            if (!audioSinkOpen_ || !audioSinkHasClock_
                || currentState_ != State::Playing
                || waitingForOutput_
                || cached.sinkSerial != appliedAudioSinkSerial_) {
                return std::nullopt;
            }
        }
        const auto elapsed = std::max<std::int64_t>(
            0,
            std::chrono::duration_cast<Milliseconds>(
                Clock::now() - cached.sampledAt)
                .count());
        auto position = cached.position + elapsed;
        if (cached.submittedUntil > 0) {
            position = std::min(position, cached.submittedUntil);
        }
        return std::max<std::int64_t>(0, position);
    }

    void publishAudioSinkEvent(
        const AudioSink* source,
        std::uint64_t serial,
        const AudioSinkEvent& event)
    {
        {
            std::lock_guard<std::mutex> lock(mutex_);
            if (activeAudioSink_.get() != source
                || appliedAudioSinkSerial_ != serial) {
                return;
            }
        }

        std::string code;
        switch (event.type) {
        case AudioSinkEventType::Underrun:
            code = "audio.sink.underrun";
            break;
        case AudioSinkEventType::DeviceLost:
            code = "audio.sink.device_lost";
            break;
        case AudioSinkEventType::Error:
            code = "audio.sink.error";
            break;
        }
        if (event.type == AudioSinkEventType::Underrun) {
            const auto generation =
                presentationGeneration_.load(std::memory_order_acquire);
            beginOutputWait(generation, position());
        }
        invalidateAudioClock();
        publishEvent({
            std::move(code),
            event.detail,
            event.type == AudioSinkEventType::Underrun ? 0 : AVERROR_EXTERNAL,
        });
    }

    void handlePlaybackEnd()
    {
        const auto generation =
            presentationGeneration_.load(std::memory_order_acquire);
        if (!waitForAudioQueueDrained(generation)) {
            return;
        }
        drainAudioConverter();
        drainAudioSink();
        if (!waitForPresentationDrained(generation)
            || generation
                != presentationGeneration_.load(std::memory_order_acquire)) {
            return;
        }

        int loopCount;
        int loopsCompleted;
        std::int64_t loopStart;
        {
            std::lock_guard<std::mutex> lock(mutex_);
            loopCount = loopCount_;
            loopsCompleted = loopsCompleted_;
            loopStart = rangeStart_;
        }
        if (loopCount < 0 || loopsCompleted < loopCount) {
            {
                std::lock_guard<std::mutex> lock(mutex_);
                ++loopsCompleted_;
            }
            const int error = seekMedia(loopStart, SeekFlag::KeyFrame);
            if (error >= 0) {
                return;
            }
            publishEvent({
                "loop.error",
                "Could not seek to the loop start: " + ffmpegError(error),
                error,
            });
        }
        stopPlayback(true);
    }

    void stopPlayback(bool naturalEnd, bool invalid = false)
    {
        resetPlaybackQueues();
        closeAudioSink(false);
        media_.reset();
        {
            std::lock_guard<std::mutex> lock(mutex_);
            hasOpenMedia_ = false;
            loadedSerial_ = 0;
            requestedState_ = State::Stopped;
            currentPosition_ =
                naturalEnd && mediaInfo_.duration > 0 ? mediaInfo_.duration : 0;
            resetClockLocked(currentPosition_);
        }
        publishState(State::Stopped);
        publishStatus(invalid
                ? MediaStatus::Invalid
                : naturalEnd ? MediaStatus::EndOfMedia
                             : MediaStatus::NoMedia);
    }

    void failOpen(int error, std::string detail)
    {
        publishEvent({
            "media.error",
            std::move(detail) + ": " + ffmpegError(error),
            error,
        });
        {
            std::lock_guard<std::mutex> lock(mutex_);
            hasOpenMedia_ = false;
            loadedSerial_ = 0;
            requestedState_ = State::Stopped;
        }
        publishState(State::Stopped);
        publishStatus(MediaStatus::Invalid);

        std::optional<PrepareRequest> prepare;
        {
            std::lock_guard<std::mutex> lock(mutex_);
            prepare = std::move(prepareRequest_);
            prepareRequest_.reset();
        }
        if (prepare && prepare->callback) {
            bool boost = false;
            prepare->callback(-1, &boost);
        }
    }

    bool wasCanceled(std::uint64_t serial) const
    {
        std::lock_guard<std::mutex> lock(mutex_);
        return quitting_.load(std::memory_order_acquire)
            || serial != mediaSerial_ || requestedState_ == State::Stopped;
    }

    void publishState(State value)
    {
        StateCallback callback;
        {
            std::lock_guard<std::mutex> lock(mutex_);
            if (currentState_ == value) {
                stateChanged_.notify_all();
                return;
            }
            currentState_ = value;
            callback = stateCallback_;
            stateChanged_.notify_all();
        }
        if (callback) {
            callback(value);
        }
    }

    void publishStatus(MediaStatus value)
    {
        StatusCallback callback;
        MediaStatus oldValue;
        {
            std::lock_guard<std::mutex> lock(mutex_);
            if (status_ == value) {
                return;
            }
            oldValue = status_;
            status_ = value;
            callback = statusCallback_;
        }
        if (callback) {
            callback(oldValue, value);
        }
    }

    void publishEvent(MediaEvent event)
    {
        EventCallback callback;
        {
            std::lock_guard<std::mutex> lock(mutex_);
            callback = eventCallback_;
        }
        if (callback) {
            callback(event);
        }
    }

    std::int64_t clockPositionLocked() const
    {
        if (currentState_ != State::Playing || waitingForOutput_) {
            return currentPosition_;
        }
        const auto elapsed = std::chrono::duration_cast<Milliseconds>(
                                 Clock::now() - clockWallBase_)
                                 .count();
        const auto value = clockMediaBase_
            + static_cast<std::int64_t>(
                               static_cast<double>(elapsed) * playbackRate_);
        if (mediaInfo_.duration > 0) {
            return std::clamp<std::int64_t>(value, 0, mediaInfo_.duration);
        }
        return std::max<std::int64_t>(0, value);
    }

    std::int64_t clampPosition(std::int64_t position) const
    {
        std::lock_guard<std::mutex> lock(mutex_);
        return clampPositionLocked(position);
    }

    std::int64_t clampPositionLocked(std::int64_t position) const
    {
        if (mediaInfo_.duration > 0) {
            return std::clamp<std::int64_t>(
                position,
                0,
                mediaInfo_.duration);
        }
        return std::max<std::int64_t>(0, position);
    }

    void resetClockLocked(std::int64_t position)
    {
        clockMediaBase_ = std::max<std::int64_t>(0, position);
        clockWallBase_ = Clock::now();
    }

    std::int64_t currentPosition() const
    {
        std::lock_guard<std::mutex> lock(mutex_);
        return currentPosition_;
    }

    mutable std::mutex mutex_;
    mutable std::mutex audioSinkCallMutex_;
    mutable std::mutex audioClockMutex_;
    mutable std::mutex audioQueueMutex_;
    mutable std::mutex presentationMutex_;
    std::shared_ptr<AudioSinkCallbackBridge> audioSinkCallbackBridge_ =
        std::make_shared<AudioSinkCallbackBridge>();
    std::condition_variable controlChanged_;
    std::condition_variable stateChanged_;
    std::condition_variable audioQueueChanged_;
    std::condition_variable audioQueueSpace_;
    std::condition_variable audioQueueDrained_;
    std::condition_variable presentationChanged_;
    std::condition_variable presentationDrained_;
    std::thread worker_;
    std::thread audioWorker_;
    std::thread presentationWorker_;
    std::atomic<bool> quitting_ { false };
    std::atomic<std::uint64_t> interruptEpoch_ { 1 };
    std::atomic<std::uint64_t> presentationGeneration_ { 1 };
    InterruptContext interrupt_;

    std::deque<QueuedAudioFrame> audioQueue_;
    std::int64_t queuedAudioDuration_ = 0;
    bool audioFrameInFlight_ = false;
    std::deque<PresentationItem> presentationQueue_;
    std::size_t queuedVideoFrames_ = 0;
    bool presentationInFlight_ = false;
    CachedAudioClock cachedAudioClock_;
    MediaContext media_;
    bool hasOpenMedia_ = false;
    bool reachedRangeEnd_ = false;
    std::string url_;
    std::uint64_t mediaSerial_ = 1;
    std::uint64_t loadedSerial_ = 0;
    std::uint64_t requestSerial_ = 0;
    std::optional<PrepareRequest> prepareRequest_;
    std::optional<SeekRequest> seekRequest_;

    State requestedState_ = State::Stopped;
    State currentState_ = State::Stopped;
    MediaStatus status_ = MediaStatus::NoMedia;
    MediaInfo mediaInfo_;
    std::int64_t currentPosition_ = 0;
    bool waitingForOutput_ = false;
    bool outputWaitPrimed_ = false;
    std::uint64_t outputWaitGeneration_ = 0;
    bool outputWaitRequiresDeviceClock_ = false;
    bool videoPrerollPending_ = false;
    std::uint64_t videoPrerollGeneration_ = 0;
    std::size_t videoPrerollFrameCount_ = 0;
    Clock::time_point videoPrerollDeadline_ = Clock::now();
    std::int64_t clockMediaBase_ = 0;
    Clock::time_point clockWallBase_ = Clock::now();
    float playbackRate_ = 1.0F;
    int loopCount_ = 0;
    int loopsCompleted_ = 0;
    std::int64_t rangeStart_ = 0;
    std::int64_t rangeEnd_ = MediaEnd;
    HardwareDecodeConfig hardwareDecodeConfig_;

    std::unordered_map<std::string, std::string> properties_;
    StateCallback stateCallback_;
    StatusCallback statusCallback_;
    EventCallback eventCallback_;
    VideoFrameCallback videoFrameCallback_;
    AudioFrameCallback audioFrameCallback_;
    std::shared_ptr<AudioSink> audioSink_;
    std::shared_ptr<AudioSink> activeAudioSink_;
    std::shared_ptr<AudioFrameConverter> audioFrameConverter_;
    std::shared_ptr<AudioFrameConverter> activeAudioFrameConverter_;
    std::uint64_t audioSinkSerial_ = 1;
    std::uint64_t appliedAudioSinkSerial_ = 0;
    bool audioSinkOpen_ = false;
    bool audioSinkOpenAttempted_ = false;
    bool audioSinkHasClock_ = false;
    bool audioFrameConverterOpen_ = false;
    RenderCallback renderCallback_;
    VideoRenderer videoRenderer_;
    std::unordered_map<void*, std::shared_ptr<VideoRenderAPI>>
        videoRenderAPIs_;
    VideoFrame currentVideoFrame_;
};

Player::Player()
    : impl_(std::make_unique<Impl>())
{
}

Player::~Player() = default;
Player::Player(Player&&) noexcept = default;
Player& Player::operator=(Player&&) noexcept = default;

void Player::setMedia(std::string url)
{
    impl_->setMedia(std::move(url));
}

std::string Player::url() const
{
    return impl_->url();
}

void Player::prepare(
    std::int64_t startPosition,
    PrepareCallback callback,
    SeekFlag flags)
{
    impl_->prepare(startPosition, std::move(callback), flags);
}

bool Player::seek(
    std::int64_t position,
    SeekFlag flags,
    SeekCallback callback)
{
    return impl_->seek(position, flags, std::move(callback));
}

void Player::setState(State state)
{
    impl_->setState(state);
}

State Player::state() const
{
    return impl_->state();
}

bool Player::waitFor(State state, long timeoutMs)
{
    return impl_->waitFor(state, timeoutMs);
}

MediaStatus Player::mediaStatus() const
{
    return impl_->mediaStatus();
}

MediaInfo Player::mediaInfo() const
{
    return impl_->mediaInfo();
}

std::int64_t Player::position() const
{
    return impl_->position();
}

Player& Player::onStateChanged(StateCallback callback)
{
    impl_->onStateChanged(std::move(callback));
    return *this;
}

Player& Player::onMediaStatus(StatusCallback callback)
{
    impl_->onMediaStatus(std::move(callback));
    return *this;
}

Player& Player::onEvent(EventCallback callback)
{
    impl_->onEvent(std::move(callback));
    return *this;
}

Player& Player::onVideoFrame(VideoFrameCallback callback)
{
    impl_->onVideoFrame(std::move(callback));
    return *this;
}

Player& Player::onAudioFrame(AudioFrameCallback callback)
{
    impl_->onAudioFrame(std::move(callback));
    return *this;
}

Player& Player::setAudioSink(std::shared_ptr<AudioSink> sink)
{
    impl_->setAudioSink(std::move(sink));
    return *this;
}

Player& Player::setAudioFrameConverter(
    std::shared_ptr<AudioFrameConverter> converter)
{
    impl_->setAudioFrameConverter(std::move(converter));
    return *this;
}

Player& Player::setHardwareDecodeConfig(HardwareDecodeConfig config)
{
    impl_->setHardwareDecodeConfig(config);
    return *this;
}

HardwareDecodeConfig Player::hardwareDecodeConfig() const
{
    return impl_->hardwareDecodeConfig();
}

Player& Player::setRenderCallback(RenderCallback callback)
{
    impl_->setRenderCallback(std::move(callback));
    return *this;
}

Player& Player::setVideoRenderer(VideoRenderer renderer)
{
    impl_->setVideoRenderer(std::move(renderer));
    return *this;
}

Player& Player::setVideoRenderAPI(
    std::shared_ptr<VideoRenderAPI> renderer,
    void* opaque)
{
    impl_->setVideoRenderAPI(std::move(renderer), opaque);
    return *this;
}

double Player::renderVideo(void* opaque)
{
    return impl_->renderVideo(opaque);
}

void Player::setPlaybackRate(float value)
{
    impl_->setPlaybackRate(value);
}

float Player::playbackRate() const
{
    return impl_->playbackRate();
}

void Player::setLoop(int count)
{
    impl_->setLoop(count);
}

void Player::setRange(std::int64_t start, std::int64_t end)
{
    impl_->setRange(start, end);
}

void Player::setProperty(std::string key, std::string value)
{
    impl_->setProperty(std::move(key), std::move(value));
}

std::string Player::property(
    const std::string& key,
    std::string defaultValue) const
{
    return impl_->property(key, std::move(defaultValue));
}

} // namespace qtav
