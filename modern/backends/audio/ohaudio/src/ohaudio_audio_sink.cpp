// SPDX-License-Identifier: LGPL-2.1-or-later

#include <qtav/ohaudio_audio_sink.h>

#include "../../common/interleaved_float_pcm_queue.h"

#include <ohaudio/native_audiorenderer.h>
#include <ohaudio/native_audiostreambuilder.h>

#include <algorithm>
#include <atomic>
#include <chrono>
#include <condition_variable>
#include <cstdint>
#include <cstring>
#include <limits>
#include <mutex>
#include <string>
#include <thread>
#include <time.h>
#include <utility>

namespace qtav {
namespace {

constexpr int kMinimumSampleRate = 8'000;
constexpr int kMaximumSampleRate = 384'000;
constexpr int kMinimumQueuedMilliseconds = 100;
constexpr int kMaximumQueuedMilliseconds = 5'000;
constexpr int kMinimumDrainTimeoutMilliseconds = 500;
constexpr int kMaximumDrainTimeoutMilliseconds = 30'000;
constexpr int kMaximumCallbackFrames = 16'384;
constexpr auto kWriteTimeout = std::chrono::milliseconds(2'000);

bool sameFormat(const AudioFormat& left, const AudioFormat& right)
{
    return left.sampleRate == right.sampleRate
        && left.channels == right.channels
        && left.sampleFormat == right.sampleFormat
        && left.channelLayout == right.channelLayout;
}

std::string defaultChannelLayout(int channels)
{
    if (channels == 1) {
        return "mono";
    }
    if (channels == 2) {
        return "stereo";
    }
    return {};
}

OH_AudioChannelLayout nativeChannelLayout(int channels)
{
    return channels == 1 ? CH_LAYOUT_MONO : CH_LAYOUT_STEREO;
}

std::int64_t millisecondsForFrames(
    std::uint64_t frames,
    int sampleRate) noexcept
{
    if (sampleRate <= 0) {
        return 0;
    }
    return static_cast<std::int64_t>(
        (frames * 1'000ULL
         + static_cast<std::uint64_t>(sampleRate / 2))
        / static_cast<std::uint64_t>(sampleRate));
}

std::string resultText(OH_AudioStream_Result result)
{
    switch (result) {
    case AUDIOSTREAM_SUCCESS:
        return "success";
    case AUDIOSTREAM_ERROR_INVALID_PARAM:
        return "invalid parameter";
    case AUDIOSTREAM_ERROR_ILLEGAL_STATE:
        return "illegal state";
    case AUDIOSTREAM_ERROR_SYSTEM:
        return "system error";
    case AUDIOSTREAM_ERROR_UNSUPPORTED_FORMAT:
        return "unsupported format";
    }
    return "unknown OHAudio error";
}

std::string routeReasonText(OH_AudioStream_DeviceChangeReason reason)
{
    switch (reason) {
    case REASON_NEW_DEVICE_AVAILABLE:
        return "new device available";
    case REASON_OLD_DEVICE_UNAVAILABLE:
        return "old device unavailable";
    case REASON_OVERRODE:
        return "route overridden";
    case REASON_SESSION_ACTIVATED:
        return "audio session activated";
    case REASON_STREAM_PRIORITY_CHANGED:
        return "stream priority changed";
    case REASON_UNKNOWN:
        break;
    }
    return "unknown route change";
}

OH_AudioStream_LatencyMode nativeLatencyMode(OHAudioLatencyMode mode)
{
    return mode == OHAudioLatencyMode::Fast
        ? AUDIOSTREAM_LATENCY_MODE_FAST
        : AUDIOSTREAM_LATENCY_MODE_NORMAL;
}

OHAudioLatencyMode publicLatencyMode(OH_AudioStream_LatencyMode mode)
{
    return mode == AUDIOSTREAM_LATENCY_MODE_FAST
        ? OHAudioLatencyMode::Fast
        : OHAudioLatencyMode::Normal;
}

} // namespace

class OHAudioAudioSink::Impl {
public:
    explicit Impl(OHAudioAudioSinkConfig value)
        : config_(std::move(value))
    {
        config_.preferredSampleRate = std::clamp(
            config_.preferredSampleRate,
            kMinimumSampleRate,
            kMaximumSampleRate);
        config_.maximumChannels =
            std::clamp(config_.maximumChannels, 1, 2);
        config_.maximumQueuedMilliseconds = std::clamp(
            config_.maximumQueuedMilliseconds,
            kMinimumQueuedMilliseconds,
            kMaximumQueuedMilliseconds);
        config_.callbackFrames = std::clamp(
            config_.callbackFrames,
            0,
            kMaximumCallbackFrames);
        config_.drainTimeoutMilliseconds = std::clamp(
            config_.drainTimeoutMilliseconds,
            kMinimumDrainTimeoutMilliseconds,
            kMaximumDrainTimeoutMilliseconds);
        worker_ = std::thread(&Impl::workerMain, this);
    }

    ~Impl()
    {
        close();
        quitting_.store(true, std::memory_order_release);
        workerChanged_.notify_all();
        if (worker_.joinable()) {
            worker_.join();
        }
    }

    AudioSinkOpenResult open(const AudioFormat& decodedFormat)
    {
        close();
        if (!decodedFormat.isValid()) {
            return {
                false,
                {},
                "The decoded audio format is invalid",
            };
        }

        std::lock_guard<std::mutex> lock(lifecycleMutex_);
        const int channels = std::clamp(
            decodedFormat.channels,
            1,
            config_.maximumChannels);
        const AudioFormat requested {
            config_.preferredSampleRate,
            channels,
            SampleFormat::Float,
            defaultChannelLayout(channels),
        };
        std::string error;
        if (!createRendererLocked(
                requested,
                false,
                config_.latencyMode,
                error)) {
            if (config_.latencyMode != OHAudioLatencyMode::Fast
                || !createRendererLocked(
                    requested,
                    false,
                    OHAudioLatencyMode::Normal,
                    error)) {
                return { false, {}, std::move(error) };
            }
        }
        return { true, deviceFormat_, {} };
    }

    void close() noexcept
    {
        std::lock_guard<std::mutex> lock(lifecycleMutex_);
        releaseRendererLocked(true);
    }

    void pause(bool paused)
    {
        std::lock_guard<std::mutex> lock(lifecycleMutex_);
        if (!opened_ || !renderer_ || paused_ == paused) {
            return;
        }
        paused_ = paused;
        if (paused) {
            pauseRendererLocked();
        } else if (!interrupted_ && queue_.queuedFrames() > 0) {
            startRendererLocked();
        }
    }

    void flush()
    {
        std::lock_guard<std::mutex> lock(lifecycleMutex_);
        if (!opened_ || !renderer_) {
            return;
        }
        pauseRendererLocked();
        if (OH_AudioRenderer_Flush(renderer_) == AUDIOSTREAM_SUCCESS) {
            flushes_.fetch_add(1, std::memory_order_acq_rel);
        }
        resetTimelineAndQueueLocked();
    }

    bool write(const AudioBufferView& buffer)
    {
        if (!buffer.isValid() || buffer.planes.size() != 1
            || buffer.lineSizes.size() != 1) {
            return false;
        }
        const int frames = buffer.samplesPerChannel;
        const auto deadline = std::chrono::steady_clock::now()
            + kWriteTimeout;
        for (;;) {
            {
                std::lock_guard<std::mutex> lock(lifecycleMutex_);
                if (!opened_ || !renderer_ || paused_ || interrupted_
                    || !sameFormat(buffer.format, deviceFormat_)
                    || frames <= 0
                    || frames > queue_.capacityFrames()) {
                    return false;
                }
                const auto bytes =
                    static_cast<std::uint64_t>(frames)
                    * static_cast<std::uint64_t>(
                        deviceFormat_.channels)
                    * sizeof(float);
                if (bytes == 0
                    || bytes
                        > static_cast<std::uint64_t>(
                            std::numeric_limits<std::size_t>::max())
                    || bytes
                        > static_cast<std::uint64_t>(
                            buffer.lineSizes.front())) {
                    return false;
                }
                if (queue_.canPush(frames)) {
                    const auto* samples =
                        reinterpret_cast<const float*>(
                            buffer.planes.front());
                    if (!queue_.push(
                            samples,
                            frames,
                            buffer.timestamp)) {
                        return false;
                    }
                    expectingData_.store(
                        true,
                        std::memory_order_release);
                    if (!startRendererLocked()) {
                        queue_.clear();
                        expectingData_.store(
                            false,
                            std::memory_order_release);
                        return false;
                    }
                    return true;
                }
            }
            if (std::chrono::steady_clock::now() >= deadline) {
                return false;
            }
            std::this_thread::sleep_for(std::chrono::milliseconds(1));
        }
    }

    bool drain()
    {
        const auto deadline = std::chrono::steady_clock::now()
            + std::chrono::milliseconds(
                config_.drainTimeoutMilliseconds);
        draining_.store(true, std::memory_order_release);
        for (;;) {
            {
                std::lock_guard<std::mutex> lock(lifecycleMutex_);
                if (!opened_ || !renderer_ || paused_ || interrupted_) {
                    draining_.store(false, std::memory_order_release);
                    return false;
                }
                if (queue_.queuedFrames() == 0) {
                    const std::int64_t targetFrame =
                        lastAudioEndFrame_.load(
                            std::memory_order_acquire);
                    if (targetFrame <= 0) {
                        finishDrainLocked();
                        return true;
                    }
                    std::int64_t framePosition = 0;
                    std::int64_t timestamp = 0;
                    const OH_AudioStream_Result result =
                        OH_AudioRenderer_GetAudioTimestampInfo(
                            renderer_,
                            &framePosition,
                            &timestamp);
                    if (result == AUDIOSTREAM_SUCCESS
                        && timestamp > 0
                        && framePosition >= targetFrame) {
                        finishDrainLocked();
                        return true;
                    }
                    if (result != AUDIOSTREAM_SUCCESS
                        && result != AUDIOSTREAM_ERROR_ILLEGAL_STATE) {
                        draining_.store(
                            false,
                            std::memory_order_release);
                        return false;
                    }
                }
            }
            if (std::chrono::steady_clock::now() >= deadline) {
                draining_.store(false, std::memory_order_release);
                return false;
            }
            std::this_thread::sleep_for(std::chrono::milliseconds(2));
        }
    }

    AudioSinkClock clock() const noexcept
    {
        std::lock_guard<std::mutex> lock(lifecycleMutex_);
        if (!opened_ || !renderer_ || !started_
            || !anchorValid_.load(std::memory_order_acquire)) {
            return {};
        }
        const std::int64_t anchorFrame =
            anchorStreamFrame_.load(std::memory_order_relaxed);
        const std::int64_t anchorMediaNanoseconds =
            anchorMediaNanoseconds_.load(std::memory_order_relaxed);
        std::int64_t framePosition = 0;
        std::int64_t timestampNanoseconds = 0;
        if (OH_AudioRenderer_GetAudioTimestampInfo(
                renderer_,
                &framePosition,
                &timestampNanoseconds)
                != AUDIOSTREAM_SUCCESS
            || timestampNanoseconds <= 0
            || framePosition < anchorFrame
            || deviceFormat_.sampleRate <= 0) {
            return {};
        }

        const std::int64_t callbackFrame =
            callbackFramePosition_.load(std::memory_order_acquire);
        std::int64_t estimatedFramePosition = framePosition;
        timespec monotonicNow {};
        if (clock_gettime(CLOCK_MONOTONIC, &monotonicNow) == 0) {
            const std::int64_t nowNanoseconds =
                static_cast<std::int64_t>(monotonicNow.tv_sec)
                    * 1'000'000'000LL
                + monotonicNow.tv_nsec;
            const std::int64_t timestampAge = std::clamp<std::int64_t>(
                nowNanoseconds - timestampNanoseconds,
                0,
                1'000'000'000LL);
            const auto elapsedSinceTimestamp =
                static_cast<std::int64_t>(
                    static_cast<std::uint64_t>(timestampAge)
                    * static_cast<std::uint64_t>(
                        deviceFormat_.sampleRate)
                    / 1'000'000'000ULL);
            estimatedFramePosition = std::min(
                callbackFrame,
                framePosition + elapsedSinceTimestamp);
        }
        const auto elapsedFrames = static_cast<std::uint64_t>(
            std::max<std::int64_t>(
                0,
                estimatedFramePosition - anchorFrame));
        const std::int64_t positionNanoseconds =
            anchorMediaNanoseconds
            + static_cast<std::int64_t>(
                elapsedFrames * 1'000'000'000ULL
                / static_cast<std::uint64_t>(
                    deviceFormat_.sampleRate));
        const auto pipelineFrames = callbackFrame > estimatedFramePosition
            ? static_cast<std::uint64_t>(
                callbackFrame - estimatedFramePosition)
            : 0;
        const auto latencyFrames =
            pipelineFrames + queue_.queuedFrames();
        std::int64_t positionMilliseconds = std::max<std::int64_t>(
            0,
            positionNanoseconds / 1'000'000LL);
        auto previous = lastClockPositionMilliseconds_.load(
            std::memory_order_relaxed);
        while (previous < positionMilliseconds
               && !lastClockPositionMilliseconds_.compare_exchange_weak(
                   previous,
                   positionMilliseconds,
                   std::memory_order_relaxed)) {
        }
        positionMilliseconds = std::max(
            positionMilliseconds,
            previous);
        return {
            true,
            positionMilliseconds,
            millisecondsForFrames(
                latencyFrames,
                deviceFormat_.sampleRate),
        };
    }

    AudioFormat deviceFormat() const
    {
        std::lock_guard<std::mutex> lock(lifecycleMutex_);
        return deviceFormat_;
    }

    int maximumChannels() const noexcept
    {
        return config_.maximumChannels;
    }

    OHAudioStreamInfo streamInfo() const noexcept
    {
        std::lock_guard<std::mutex> lock(lifecycleMutex_);
        OHAudioStreamInfo result;
        result.callbackCount =
            callbackCount_.load(std::memory_order_acquire);
        result.renderedPcmFrames =
            renderedPcmFrames_.load(std::memory_order_acquire);
        result.callbackUnderruns =
            callbackUnderruns_.load(std::memory_order_acquire);
        result.starts = starts_.load(std::memory_order_acquire);
        result.flushes = flushes_.load(std::memory_order_acquire);
        result.drains = drains_.load(std::memory_order_acquire);
        result.routeChanges =
            routeChanges_.load(std::memory_order_acquire);
        result.streamRestarts =
            streamRestarts_.load(std::memory_order_acquire);
        result.interrupts = interrupts_.load(std::memory_order_acquire);
        result.latencyMode = activeLatencyMode_;
        if (!renderer_) {
            return result;
        }
        OH_AudioRenderer_GetStreamId(renderer_, &result.streamId);
        OH_AudioRenderer_GetFrameSizeInCallback(
            renderer_,
            &result.callbackFrames);
        OH_AudioRenderer_GetFramesWritten(
            renderer_,
            &result.framesWritten);
        OH_AudioRenderer_GetUnderflowCount(
            renderer_,
            &result.nativeUnderflowCount);
        return result;
    }

    void setEventCallback(EventCallback callback)
    {
        std::lock_guard<std::mutex> lock(callbackMutex_);
        callback_ = std::move(callback);
    }

private:
    static OH_AudioData_Callback_Result writeDataCallback(
        OH_AudioRenderer* renderer,
        void* userData,
        void* audioData,
        std::int32_t audioDataSize)
    {
        return static_cast<Impl*>(userData)->onWriteData(
            renderer,
            audioData,
            audioDataSize);
    }

    static void outputDeviceChangeCallback(
        OH_AudioRenderer* renderer,
        void* userData,
        OH_AudioStream_DeviceChangeReason reason)
    {
        static_cast<Impl*>(userData)->onOutputDeviceChange(
            renderer,
            reason);
    }

    static void interruptCallback(
        OH_AudioRenderer* renderer,
        void* userData,
        OH_AudioInterrupt_ForceType type,
        OH_AudioInterrupt_Hint hint)
    {
        static_cast<Impl*>(userData)->onInterrupt(
            renderer,
            type,
            hint);
    }

    static void errorCallback(
        OH_AudioRenderer* renderer,
        void* userData,
        OH_AudioStream_Result error)
    {
        static_cast<Impl*>(userData)->onError(renderer, error);
    }

    OH_AudioData_Callback_Result onWriteData(
        OH_AudioRenderer* renderer,
        void* audioData,
        std::int32_t audioDataSize) noexcept
    {
        const int channels =
            callbackChannels_.load(std::memory_order_relaxed);
        const int frameBytes = channels > 0
            ? channels * static_cast<int>(sizeof(float))
            : 0;
        if (!audioData || audioDataSize <= 0 || frameBytes <= 0
            || audioDataSize % frameBytes != 0) {
            if (audioData && audioDataSize > 0) {
                std::memset(
                    audioData,
                    0,
                    static_cast<std::size_t>(audioDataSize));
            }
            return AUDIO_DATA_CALLBACK_RESULT_INVALID;
        }
        auto* output = static_cast<float*>(audioData);
        const int frames = audioDataSize / frameBytes;
        if (activeRenderer_.load(std::memory_order_acquire)
            != renderer) {
            std::memset(
                output,
                0,
                static_cast<std::size_t>(audioDataSize));
            return AUDIO_DATA_CALLBACK_RESULT_VALID;
        }

        const std::int64_t callbackStart =
            callbackFramePosition_.load(std::memory_order_relaxed);
        const detail::InterleavedFloatPcmQueue::PopResult popped =
            queue_.pop(output, frames);
        callbackCount_.fetch_add(1, std::memory_order_relaxed);
        if (popped.frames > 0) {
            if (!anchorValid_.load(std::memory_order_acquire)) {
                anchorStreamFrame_.store(
                    callbackStart,
                    std::memory_order_relaxed);
                anchorMediaNanoseconds_.store(
                    popped.firstTimestampNanoseconds,
                    std::memory_order_relaxed);
                anchorValid_.store(true, std::memory_order_release);
            }
            lastAudioEndFrame_.store(
                callbackStart + popped.frames,
                std::memory_order_release);
            renderedPcmFrames_.fetch_add(
                static_cast<std::uint64_t>(popped.frames),
                std::memory_order_relaxed);
            underrunReported_.store(false, std::memory_order_release);
        }
        if (popped.frames < frames) {
            std::memset(
                output
                    + static_cast<std::size_t>(popped.frames)
                        * static_cast<std::size_t>(channels),
                0,
                static_cast<std::size_t>(frames - popped.frames)
                    * static_cast<std::size_t>(channels)
                    * sizeof(float));
            if (expectingData_.load(std::memory_order_acquire)
                && !draining_.load(std::memory_order_acquire)) {
                anchorValid_.store(false, std::memory_order_release);
                if (!underrunReported_.exchange(
                        true,
                        std::memory_order_acq_rel)) {
                    callbackUnderruns_.fetch_add(
                        1,
                        std::memory_order_relaxed);
                    pendingUnderrun_.store(
                        true,
                        std::memory_order_release);
                    workerChanged_.notify_one();
                }
            }
        }
        callbackFramePosition_.store(
            callbackStart + frames,
            std::memory_order_release);
        return AUDIO_DATA_CALLBACK_RESULT_VALID;
    }

    void onOutputDeviceChange(
        OH_AudioRenderer* renderer,
        OH_AudioStream_DeviceChangeReason reason) noexcept
    {
        if (activeRenderer_.load(std::memory_order_acquire)
            != renderer) {
            return;
        }
        routeChanges_.fetch_add(1, std::memory_order_relaxed);
        if (reason == REASON_SESSION_ACTIVATED
            || reason == REASON_STREAM_PRIORITY_CHANGED) {
            return;
        }
        anchorValid_.store(false, std::memory_order_release);
        routeRenderer_.store(renderer, std::memory_order_release);
        pendingRouteReason_.store(
            static_cast<int>(reason),
            std::memory_order_release);
        pendingRouteChange_.store(true, std::memory_order_release);
        workerChanged_.notify_one();
    }

    void onInterrupt(
        OH_AudioRenderer* renderer,
        OH_AudioInterrupt_ForceType type,
        OH_AudioInterrupt_Hint hint) noexcept
    {
        if (activeRenderer_.load(std::memory_order_acquire)
            != renderer) {
            return;
        }
        interruptRenderer_.store(renderer, std::memory_order_release);
        pendingInterruptType_.store(
            static_cast<int>(type),
            std::memory_order_release);
        pendingInterruptHint_.store(
            static_cast<int>(hint),
            std::memory_order_release);
        pendingInterrupt_.store(true, std::memory_order_release);
        workerChanged_.notify_one();
    }

    void onError(
        OH_AudioRenderer* renderer,
        OH_AudioStream_Result error) noexcept
    {
        if (activeRenderer_.load(std::memory_order_acquire)
            != renderer) {
            return;
        }
        errorRenderer_.store(renderer, std::memory_order_release);
        pendingErrorCode_.store(
            static_cast<int>(error),
            std::memory_order_release);
        pendingError_.store(true, std::memory_order_release);
        workerChanged_.notify_one();
    }

    bool createRendererLocked(
        const AudioFormat& requested,
        bool exactFormat,
        OHAudioLatencyMode latencyMode,
        std::string& error)
    {
        OH_AudioStreamBuilder* builder = nullptr;
        OH_AudioStream_Result result = OH_AudioStreamBuilder_Create(
            &builder,
            AUDIOSTREAM_TYPE_RENDERER);
        if (result != AUDIOSTREAM_SUCCESS || !builder) {
            error = "Could not create the OHAudio stream builder: "
                + resultText(result);
            return false;
        }

        auto configure = [&](OH_AudioStream_Result stepResult,
                             const char* step) {
            if (stepResult == AUDIOSTREAM_SUCCESS) {
                return true;
            }
            error = std::string("Could not configure OHAudio ") + step
                + ": " + resultText(stepResult);
            return false;
        };
        bool configured = configure(
            OH_AudioStreamBuilder_SetSamplingRate(
                builder,
                requested.sampleRate),
            "sample rate");
        configured = configured && configure(
            OH_AudioStreamBuilder_SetChannelCount(
                builder,
                requested.channels),
            "channel count");
        configured = configured && configure(
            OH_AudioStreamBuilder_SetChannelLayout(
                builder,
                nativeChannelLayout(requested.channels)),
            "channel layout");
        configured = configured && configure(
            OH_AudioStreamBuilder_SetSampleFormat(
                builder,
                AUDIOSTREAM_SAMPLE_F32LE),
            "Float32 sample format");
        configured = configured && configure(
            OH_AudioStreamBuilder_SetEncodingType(
                builder,
                AUDIOSTREAM_ENCODING_TYPE_RAW),
            "PCM encoding");
        configured = configured && configure(
            OH_AudioStreamBuilder_SetLatencyMode(
                builder,
                nativeLatencyMode(latencyMode)),
            "latency mode");
        configured = configured && configure(
            OH_AudioStreamBuilder_SetRendererInfo(
                builder,
                AUDIOSTREAM_USAGE_MOVIE),
            "movie usage");
        if (configured && config_.callbackFrames > 0) {
            configured = configure(
                OH_AudioStreamBuilder_SetFrameSizeInCallback(
                    builder,
                    config_.callbackFrames),
                "callback frame size");
        }
        configured = configured && configure(
            OH_AudioStreamBuilder_SetRendererWriteDataCallback(
                builder,
                &Impl::writeDataCallback,
                this),
            "write callback");
        configured = configured && configure(
            OH_AudioStreamBuilder_SetRendererOutputDeviceChangeCallback(
                builder,
                &Impl::outputDeviceChangeCallback,
                this),
            "route-change callback");
        configured = configured && configure(
            OH_AudioStreamBuilder_SetRendererInterruptCallback(
                builder,
                &Impl::interruptCallback,
                this),
            "interrupt callback");
        configured = configured && configure(
            OH_AudioStreamBuilder_SetRendererErrorCallback(
                builder,
                &Impl::errorCallback,
                this),
            "error callback");
        if (!configured) {
            OH_AudioStreamBuilder_Destroy(builder);
            return false;
        }

        OH_AudioRenderer* renderer = nullptr;
        result = OH_AudioStreamBuilder_GenerateRenderer(
            builder,
            &renderer);
        OH_AudioStreamBuilder_Destroy(builder);
        if (result != AUDIOSTREAM_SUCCESS || !renderer) {
            error = "Could not open the OHAudio renderer: "
                + resultText(result);
            return false;
        }

        std::int32_t sampleRate = 0;
        std::int32_t channels = 0;
        OH_AudioStream_SampleFormat format = AUDIOSTREAM_SAMPLE_U8;
        OH_AudioStream_LatencyMode actualLatency =
            AUDIOSTREAM_LATENCY_MODE_NORMAL;
        const bool queried =
            OH_AudioRenderer_GetSamplingRate(renderer, &sampleRate)
                    == AUDIOSTREAM_SUCCESS
            && OH_AudioRenderer_GetChannelCount(renderer, &channels)
                    == AUDIOSTREAM_SUCCESS
            && OH_AudioRenderer_GetSampleFormat(renderer, &format)
                    == AUDIOSTREAM_SUCCESS
            && OH_AudioRenderer_GetLatencyMode(
                   renderer,
                   &actualLatency)
                    == AUDIOSTREAM_SUCCESS;
        if (!queried || sampleRate <= 0 || channels <= 0
            || channels > config_.maximumChannels
            || format != AUDIOSTREAM_SAMPLE_F32LE
            || (exactFormat
                && (sampleRate != requested.sampleRate
                    || channels != requested.channels))) {
            OH_AudioRenderer_Release(renderer);
            error =
                "OHAudio did not provide the requested Float32 PCM format";
            return false;
        }

        const auto queueFrames64 =
            static_cast<std::uint64_t>(sampleRate)
            * static_cast<std::uint64_t>(
                config_.maximumQueuedMilliseconds)
            / 1'000ULL;
        if (queueFrames64 == 0
            || queueFrames64
                > static_cast<std::uint64_t>(
                    std::numeric_limits<int>::max())
            || !queue_.configure(
                static_cast<int>(queueFrames64),
                channels,
                sampleRate)) {
            OH_AudioRenderer_Release(renderer);
            error = "Could not allocate the bounded OHAudio PCM queue";
            return false;
        }

        renderer_ = renderer;
        deviceFormat_ = {
            sampleRate,
            channels,
            SampleFormat::Float,
            defaultChannelLayout(channels),
        };
        activeLatencyMode_ = publicLatencyMode(actualLatency);
        callbackChannels_.store(channels, std::memory_order_release);
        resetTimelineAndQueueLocked();
        clearPendingCallbacksLocked();
        activeRenderer_.store(renderer, std::memory_order_release);
        opened_ = true;
        started_ = false;
        paused_ = false;
        interrupted_ = false;
        return true;
    }

    void releaseRendererLocked(bool clearFormat) noexcept
    {
        opened_ = false;
        activeRenderer_.store(nullptr, std::memory_order_release);
        if (renderer_) {
            if (started_) {
                OH_AudioRenderer_Stop(renderer_);
            }
            OH_AudioRenderer_Release(renderer_);
            renderer_ = nullptr;
        }
        started_ = false;
        paused_ = false;
        interrupted_ = false;
        callbackChannels_.store(0, std::memory_order_release);
        resetTimelineAndQueueLocked();
        clearPendingCallbacksLocked();
        if (clearFormat) {
            deviceFormat_ = {};
            activeLatencyMode_ = OHAudioLatencyMode::Normal;
        }
    }

    bool startRendererLocked()
    {
        if (!renderer_ || !opened_ || paused_ || interrupted_) {
            return false;
        }
        if (started_) {
            return true;
        }
        if (OH_AudioRenderer_Start(renderer_) != AUDIOSTREAM_SUCCESS) {
            return false;
        }
        started_ = true;
        starts_.fetch_add(1, std::memory_order_acq_rel);
        return true;
    }

    void pauseRendererLocked()
    {
        if (!renderer_ || !started_) {
            return;
        }
        OH_AudioStream_State state = AUDIOSTREAM_STATE_INVALID;
        if (OH_AudioRenderer_GetCurrentState(renderer_, &state)
                == AUDIOSTREAM_SUCCESS
            && state == AUDIOSTREAM_STATE_RUNNING) {
            OH_AudioRenderer_Pause(renderer_);
        }
        started_ = false;
    }

    void resetTimelineAndQueueLocked() noexcept
    {
        queue_.clear();
        expectingData_.store(false, std::memory_order_release);
        draining_.store(false, std::memory_order_release);
        anchorValid_.store(false, std::memory_order_release);
        anchorStreamFrame_.store(0, std::memory_order_relaxed);
        anchorMediaNanoseconds_.store(0, std::memory_order_relaxed);
        callbackFramePosition_.store(0, std::memory_order_release);
        lastAudioEndFrame_.store(0, std::memory_order_release);
        lastClockPositionMilliseconds_.store(0, std::memory_order_release);
        underrunReported_.store(false, std::memory_order_release);
        pendingUnderrun_.store(false, std::memory_order_release);
    }

    void clearPendingCallbacksLocked() noexcept
    {
        pendingRouteChange_.store(false, std::memory_order_release);
        routeRenderer_.store(nullptr, std::memory_order_release);
        pendingInterrupt_.store(false, std::memory_order_release);
        interruptRenderer_.store(nullptr, std::memory_order_release);
        pendingError_.store(false, std::memory_order_release);
        errorRenderer_.store(nullptr, std::memory_order_release);
    }

    void finishDrainLocked() noexcept
    {
        pauseRendererLocked();
        expectingData_.store(false, std::memory_order_release);
        draining_.store(false, std::memory_order_release);
        anchorValid_.store(false, std::memory_order_release);
        underrunReported_.store(false, std::memory_order_release);
        drains_.fetch_add(1, std::memory_order_acq_rel);
    }

    void publishEvent(AudioSinkEvent event)
    {
        EventCallback callback;
        {
            std::lock_guard<std::mutex> lock(callbackMutex_);
            callback = callback_;
        }
        if (callback) {
            callback(event);
        }
    }

    void recoverRenderer(
        OH_AudioRenderer* failedRenderer,
        std::string cause)
    {
        std::string detail;
        bool recovered = false;
        {
            std::lock_guard<std::mutex> lock(lifecycleMutex_);
            if (!opened_ || renderer_ != failedRenderer) {
                return;
            }
            const AudioFormat restartFormat = deviceFormat_;
            const OHAudioLatencyMode restartLatency = activeLatencyMode_;
            const bool wasPaused = paused_;
            const bool wasInterrupted = interrupted_;
            releaseRendererLocked(false);

            std::string reopenError;
            bool reopened = createRendererLocked(
                    restartFormat,
                    true,
                    restartLatency,
                    reopenError);
            if (!reopened
                && restartLatency == OHAudioLatencyMode::Fast) {
                reopened = createRendererLocked(
                    restartFormat,
                    true,
                    OHAudioLatencyMode::Normal,
                    reopenError);
            }
            if (reopened) {
                paused_ = wasPaused;
                interrupted_ = wasInterrupted;
                streamRestarts_.fetch_add(1, std::memory_order_acq_rel);
                detail = std::move(cause)
                    + "; OHAudio output stream rebuilt";
                recovered = true;
            } else {
                opened_ = false;
                deviceFormat_ = {};
                detail = std::move(cause) + "; restart failed: "
                    + reopenError;
            }
        }
        publishEvent({
            recovered
                ? AudioSinkEventType::Underrun
                : AudioSinkEventType::DeviceLost,
            std::move(detail),
        });
    }

    void handleInterrupt(
        OH_AudioRenderer* renderer,
        OH_AudioInterrupt_ForceType type,
        OH_AudioInterrupt_Hint hint)
    {
        bool notifyUnderrun = false;
        {
            std::lock_guard<std::mutex> lock(lifecycleMutex_);
            if (!opened_ || renderer_ != renderer) {
                return;
            }
            interrupts_.fetch_add(1, std::memory_order_acq_rel);
            if (type != AUDIOSTREAM_INTERRUPT_FORCE) {
                return;
            }
            if (hint == AUDIOSTREAM_INTERRUPT_HINT_PAUSE
                || hint == AUDIOSTREAM_INTERRUPT_HINT_STOP) {
                interrupted_ = true;
                pauseRendererLocked();
                anchorValid_.store(false, std::memory_order_release);
                notifyUnderrun = true;
            } else if (hint == AUDIOSTREAM_INTERRUPT_HINT_RESUME) {
                interrupted_ = false;
                if (!paused_ && queue_.queuedFrames() > 0) {
                    startRendererLocked();
                }
            }
        }
        if (notifyUnderrun) {
            publishEvent({
                AudioSinkEventType::Underrun,
                "OHAudio output was interrupted by the system",
            });
        }
    }

    void workerMain()
    {
        while (!quitting_.load(std::memory_order_acquire)) {
            {
                std::unique_lock<std::mutex> lock(workerMutex_);
                workerChanged_.wait_for(
                    lock,
                    std::chrono::milliseconds(10),
                    [this] {
                        return quitting_.load(std::memory_order_acquire)
                            || pendingUnderrun_.load(
                                std::memory_order_acquire)
                            || pendingRouteChange_.load(
                                std::memory_order_acquire)
                            || pendingInterrupt_.load(
                                std::memory_order_acquire)
                            || pendingError_.load(
                                std::memory_order_acquire);
                    });
            }
            if (quitting_.load(std::memory_order_acquire)) {
                break;
            }
            if (pendingUnderrun_.exchange(
                    false,
                    std::memory_order_acq_rel)) {
                publishEvent({
                    AudioSinkEventType::Underrun,
                    "OHAudio real-time callback underrun",
                });
            }
            if (pendingRouteChange_.exchange(
                    false,
                    std::memory_order_acq_rel)) {
                OH_AudioRenderer* renderer = routeRenderer_.exchange(
                    nullptr,
                    std::memory_order_acq_rel);
                const auto reason =
                    static_cast<OH_AudioStream_DeviceChangeReason>(
                        pendingRouteReason_.load(
                            std::memory_order_acquire));
                if (renderer) {
                    recoverRenderer(
                        renderer,
                        "OHAudio route changed ("
                            + routeReasonText(reason) + ')');
                }
            }
            if (pendingInterrupt_.exchange(
                    false,
                    std::memory_order_acq_rel)) {
                OH_AudioRenderer* renderer =
                    interruptRenderer_.exchange(
                        nullptr,
                        std::memory_order_acq_rel);
                const auto type = static_cast<OH_AudioInterrupt_ForceType>(
                    pendingInterruptType_.load(
                        std::memory_order_acquire));
                const auto hint = static_cast<OH_AudioInterrupt_Hint>(
                    pendingInterruptHint_.load(
                        std::memory_order_acquire));
                if (renderer) {
                    handleInterrupt(renderer, type, hint);
                }
            }
            if (pendingError_.exchange(
                    false,
                    std::memory_order_acq_rel)) {
                OH_AudioRenderer* renderer = errorRenderer_.exchange(
                    nullptr,
                    std::memory_order_acq_rel);
                const auto error = static_cast<OH_AudioStream_Result>(
                    pendingErrorCode_.load(std::memory_order_acquire));
                if (renderer) {
                    recoverRenderer(
                        renderer,
                        "OHAudio output failed: " + resultText(error));
                }
            }
        }
    }

    OHAudioAudioSinkConfig config_;
    mutable std::mutex lifecycleMutex_;
    mutable std::mutex callbackMutex_;
    EventCallback callback_;
    OH_AudioRenderer* renderer_ = nullptr;
    AudioFormat deviceFormat_;
    OHAudioLatencyMode activeLatencyMode_ = OHAudioLatencyMode::Normal;
    bool opened_ = false;
    bool started_ = false;
    bool paused_ = false;
    bool interrupted_ = false;

    detail::InterleavedFloatPcmQueue queue_;
    std::atomic<OH_AudioRenderer*> activeRenderer_ { nullptr };
    std::atomic<int> callbackChannels_ { 0 };
    std::atomic<bool> expectingData_ { false };
    std::atomic<bool> draining_ { false };
    std::atomic<bool> anchorValid_ { false };
    std::atomic<std::int64_t> anchorStreamFrame_ { 0 };
    std::atomic<std::int64_t> anchorMediaNanoseconds_ { 0 };
    std::atomic<std::int64_t> callbackFramePosition_ { 0 };
    std::atomic<std::int64_t> lastAudioEndFrame_ { 0 };
    mutable std::atomic<std::int64_t>
        lastClockPositionMilliseconds_ { 0 };
    std::atomic<bool> underrunReported_ { false };
    std::atomic<bool> pendingUnderrun_ { false };

    std::thread worker_;
    std::mutex workerMutex_;
    std::condition_variable workerChanged_;
    std::atomic<bool> quitting_ { false };
    std::atomic<OH_AudioRenderer*> routeRenderer_ { nullptr };
    std::atomic<bool> pendingRouteChange_ { false };
    std::atomic<int> pendingRouteReason_ { REASON_UNKNOWN };
    std::atomic<OH_AudioRenderer*> interruptRenderer_ { nullptr };
    std::atomic<bool> pendingInterrupt_ { false };
    std::atomic<int> pendingInterruptType_ { AUDIOSTREAM_INTERRUPT_SHARE };
    std::atomic<int> pendingInterruptHint_ {
        AUDIOSTREAM_INTERRUPT_HINT_NONE
    };
    std::atomic<OH_AudioRenderer*> errorRenderer_ { nullptr };
    std::atomic<bool> pendingError_ { false };
    std::atomic<int> pendingErrorCode_ { AUDIOSTREAM_SUCCESS };

    std::atomic<std::uint64_t> callbackCount_ { 0 };
    std::atomic<std::uint64_t> renderedPcmFrames_ { 0 };
    std::atomic<std::uint64_t> callbackUnderruns_ { 0 };
    std::atomic<std::uint64_t> starts_ { 0 };
    std::atomic<std::uint64_t> flushes_ { 0 };
    std::atomic<std::uint64_t> drains_ { 0 };
    std::atomic<std::uint64_t> routeChanges_ { 0 };
    std::atomic<std::uint64_t> streamRestarts_ { 0 };
    std::atomic<std::uint64_t> interrupts_ { 0 };
};

OHAudioAudioSink::OHAudioAudioSink(OHAudioAudioSinkConfig config)
    : impl_(std::make_unique<Impl>(std::move(config)))
{
}

OHAudioAudioSink::~OHAudioAudioSink() = default;

OHAudioAudioSink::OHAudioAudioSink(OHAudioAudioSink&&) noexcept = default;
OHAudioAudioSink& OHAudioAudioSink::operator=(
    OHAudioAudioSink&&) noexcept = default;

AudioSinkCapabilities OHAudioAudioSink::capabilities() const
{
    if (!impl_) {
        return {};
    }
    return {
        { SampleFormat::Float },
        kMinimumSampleRate,
        kMaximumSampleRate,
        impl_->maximumChannels(),
        true,
        true,
    };
}

void OHAudioAudioSink::setEventCallback(EventCallback callback)
{
    if (impl_) {
        impl_->setEventCallback(std::move(callback));
    }
}

AudioSinkOpenResult OHAudioAudioSink::open(
    const AudioFormat& decodedFormat)
{
    if (!impl_) {
        return {
            false,
            {},
            "The OHAudio audio sink has been moved from",
        };
    }
    return impl_->open(decodedFormat);
}

void OHAudioAudioSink::close() noexcept
{
    if (impl_) {
        impl_->close();
    }
}

void OHAudioAudioSink::pause(bool paused)
{
    if (impl_) {
        impl_->pause(paused);
    }
}

void OHAudioAudioSink::flush()
{
    if (impl_) {
        impl_->flush();
    }
}

bool OHAudioAudioSink::write(const AudioBufferView& buffer)
{
    return impl_ && impl_->write(buffer);
}

bool OHAudioAudioSink::drain()
{
    return impl_ && impl_->drain();
}

AudioSinkClock OHAudioAudioSink::clock() const noexcept
{
    return impl_ ? impl_->clock() : AudioSinkClock {};
}

AudioFormat OHAudioAudioSink::deviceFormat() const
{
    return impl_ ? impl_->deviceFormat() : AudioFormat {};
}

OHAudioStreamInfo OHAudioAudioSink::streamInfo() const noexcept
{
    return impl_ ? impl_->streamInfo() : OHAudioStreamInfo {};
}

} // namespace qtav
