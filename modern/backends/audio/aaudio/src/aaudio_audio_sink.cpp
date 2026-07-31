// SPDX-License-Identifier: LGPL-2.1-or-later

#include <qtav/aaudio_audio_sink.h>

#include "aaudio_pcm_queue.h"

#include <aaudio/AAudio.h>

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
#include <utility>

namespace qtav {
namespace {

constexpr int kMinimumQueuedMilliseconds = 100;
constexpr int kMaximumQueuedMilliseconds = 5'000;
constexpr int kMinimumDrainTimeoutMilliseconds = 500;
constexpr int kMaximumDrainTimeoutMilliseconds = 30'000;
constexpr auto kWriteTimeout = std::chrono::milliseconds(2'000);
constexpr auto kStateTimeout = std::chrono::milliseconds(500);
constexpr std::int64_t kStateWaitSliceNanoseconds = 20'000'000;

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

std::string resultText(aaudio_result_t result)
{
    const char* text = AAudio_convertResultToText(result);
    return text ? text : "unknown AAudio error";
}

} // namespace

AAudioDeviceId::AAudioDeviceId(std::int32_t value) noexcept
    : value_(std::max<std::int32_t>(0, value))
{
}

std::int32_t AAudioDeviceId::value() const noexcept
{
    return value_;
}

AAudioDeviceId::operator bool() const noexcept
{
    return value_ > 0;
}

class AAudioAudioSink::Impl {
public:
    explicit Impl(AAudioAudioSinkConfig value)
        : config_(std::move(value))
    {
        config_.maximumChannels =
            std::clamp(config_.maximumChannels, 1, 2);
        config_.maximumQueuedMilliseconds = std::clamp(
            config_.maximumQueuedMilliseconds,
            kMinimumQueuedMilliseconds,
            kMaximumQueuedMilliseconds);
        config_.bufferBursts =
            std::clamp(config_.bufferBursts, 1, 8);
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
        const int requestedChannels = std::clamp(
            decodedFormat.channels,
            1,
            config_.maximumChannels);
        AudioFormat requested {
            0,
            requestedChannels,
            SampleFormat::Float,
            defaultChannelLayout(requestedChannels),
        };
        std::string error;
        if (!createStreamLocked(requested, false, error)) {
            return { false, {}, std::move(error) };
        }
        return { true, deviceFormat_, {} };
    }

    void close() noexcept
    {
        std::lock_guard<std::mutex> lock(lifecycleMutex_);
        closeStreamLocked();
    }

    void pause(bool paused)
    {
        std::lock_guard<std::mutex> lock(lifecycleMutex_);
        if (!opened_ || !stream_ || paused_ == paused) {
            return;
        }
        if (paused) {
            paused_ = true;
            pauseStreamLocked();
            return;
        }
        paused_ = false;
        if (queue_.queuedFrames() > 0) {
            startStreamLocked();
        }
    }

    void flush()
    {
        std::lock_guard<std::mutex> lock(lifecycleMutex_);
        if (!opened_ || !stream_) {
            return;
        }
        pauseStreamLocked();
        const aaudio_stream_state_t state =
            AAudioStream_getState(stream_);
        if (state == AAUDIO_STREAM_STATE_OPEN
            || state == AAUDIO_STREAM_STATE_PAUSED
            || state == AAUDIO_STREAM_STATE_STOPPED
            || state == AAUDIO_STREAM_STATE_FLUSHED) {
            const aaudio_result_t result =
                AAudioStream_requestFlush(stream_);
            if (result == AAUDIO_OK) {
                waitForStateLocked(
                    AAUDIO_STREAM_STATE_FLUSHED,
                    kStateTimeout);
            }
        }
        resetTimelineAndQueueLocked(false);
    }

    bool write(const AudioBufferView& buffer)
    {
        if (!buffer.isValid() || buffer.planes.size() != 1
            || buffer.lineSizes.size() != 1) {
            return false;
        }
        const auto frames = buffer.samplesPerChannel;
        const auto deadline = std::chrono::steady_clock::now()
            + kWriteTimeout;
        for (;;) {
            {
                std::lock_guard<std::mutex> lock(lifecycleMutex_);
                if (!opened_ || !stream_ || paused_
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
                    if (!startStreamLocked()) {
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
            std::this_thread::sleep_for(
                std::chrono::milliseconds(1));
        }
    }

    bool drain()
    {
        const auto deadline = std::chrono::steady_clock::now()
            + std::chrono::milliseconds(
                config_.drainTimeoutMilliseconds);
        draining_.store(true, std::memory_order_release);
        std::int64_t targetFrame = -1;
        for (;;) {
            {
                std::lock_guard<std::mutex> lock(lifecycleMutex_);
                if (!opened_ || !stream_ || paused_) {
                    draining_.store(false, std::memory_order_release);
                    return false;
                }
                if (queue_.queuedFrames() == 0) {
                    targetFrame = lastAudioEndFrame_.load(
                        std::memory_order_acquire);
                    if (targetFrame <= 0) {
                        finishDrainLocked();
                        return true;
                    }
                    std::int64_t position = 0;
                    std::int64_t time = 0;
                    const aaudio_result_t timestampResult =
                        AAudioStream_getTimestamp(
                            stream_,
                            CLOCK_MONOTONIC,
                            &position,
                            &time);
                    if (timestampResult == AAUDIO_OK
                        && position >= targetFrame) {
                        finishDrainLocked();
                        return true;
                    }
                    if (timestampResult
                            == AAUDIO_ERROR_DISCONNECTED
                        || timestampResult
                            == AAUDIO_ERROR_INVALID_HANDLE) {
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
            std::this_thread::sleep_for(
                std::chrono::milliseconds(2));
        }
    }

    AudioSinkClock clock() const noexcept
    {
        std::lock_guard<std::mutex> lock(lifecycleMutex_);
        if (!opened_ || !stream_
            || !anchorValid_.load(std::memory_order_acquire)) {
            return {};
        }
        const std::int64_t anchorFrame =
            anchorStreamFrame_.load(std::memory_order_relaxed);
        const std::int64_t anchorMediaNanoseconds =
            anchorMediaNanoseconds_.load(
                std::memory_order_relaxed);
        std::int64_t framePosition = 0;
        std::int64_t timeNanoseconds = 0;
        const aaudio_result_t result =
            AAudioStream_getTimestamp(
                stream_,
                CLOCK_MONOTONIC,
                &framePosition,
                &timeNanoseconds);
        if (result != AAUDIO_OK || framePosition < anchorFrame
            || deviceFormat_.sampleRate <= 0) {
            return {};
        }
        const auto elapsedFrames =
            static_cast<std::uint64_t>(
                framePosition - anchorFrame);
        const std::int64_t positionNanoseconds =
            anchorMediaNanoseconds
            + static_cast<std::int64_t>(
                elapsedFrames * 1'000'000'000ULL
                / static_cast<std::uint64_t>(
                    deviceFormat_.sampleRate));
        const std::int64_t callbackFrame =
            callbackFramePosition_.load(
                std::memory_order_acquire);
        const auto pipelineFrames =
            callbackFrame > framePosition
            ? static_cast<std::uint64_t>(
                callbackFrame - framePosition)
            : 0;
        const auto latencyFrames =
            pipelineFrames + queue_.queuedFrames();
        return {
            true,
            std::max<std::int64_t>(
                0,
                positionNanoseconds / 1'000'000LL),
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

    AAudioStreamInfo streamInfo() const noexcept
    {
        std::lock_guard<std::mutex> lock(lifecycleMutex_);
        AAudioStreamInfo result;
        result.device = AAudioDeviceId(activeDeviceId_);
        result.routeChanges =
            routeChanges_.load(std::memory_order_acquire);
        result.disconnectRestarts =
            disconnectRestarts_.load(
                std::memory_order_acquire);
        if (!stream_) {
            return result;
        }
        result.bufferSizeInFrames =
            AAudioStream_getBufferSizeInFrames(stream_);
        result.bufferCapacityInFrames =
            AAudioStream_getBufferCapacityInFrames(stream_);
        result.framesPerBurst =
            AAudioStream_getFramesPerBurst(stream_);
        result.xRunCount =
            AAudioStream_getXRunCount(stream_);
        return result;
    }

    void setEventCallback(EventCallback callback)
    {
        std::lock_guard<std::mutex> lock(callbackMutex_);
        callback_ = std::move(callback);
    }

private:
    static aaudio_data_callback_result_t dataCallback(
        AAudioStream* stream,
        void* userData,
        void* audioData,
        std::int32_t frames)
    {
        return static_cast<Impl*>(userData)->onAudioReady(
            stream,
            audioData,
            frames);
    }

    static void errorCallback(
        AAudioStream* stream,
        void* userData,
        aaudio_result_t error)
    {
        auto* impl = static_cast<Impl*>(userData);
        if (impl->activeStream_.load(std::memory_order_acquire)
            != stream) {
            return;
        }
        impl->errorStream_.store(
            stream,
            std::memory_order_release);
        impl->pendingError_.store(
            error,
            std::memory_order_release);
        impl->workerChanged_.notify_one();
    }

    aaudio_data_callback_result_t onAudioReady(
        AAudioStream* stream,
        void* audioData,
        std::int32_t frames) noexcept
    {
        const int channels =
            callbackChannels_.load(std::memory_order_relaxed);
        if (!audioData || frames <= 0 || channels <= 0) {
            return AAUDIO_CALLBACK_RESULT_STOP;
        }
        auto* output = static_cast<float*>(audioData);
        if (activeStream_.load(std::memory_order_acquire)
            != stream) {
            std::memset(
                output,
                0,
                static_cast<std::size_t>(frames)
                    * static_cast<std::size_t>(channels)
                    * sizeof(float));
            return AAUDIO_CALLBACK_RESULT_STOP;
        }

        const std::int64_t callbackStart =
            callbackFramePosition_.load(
                std::memory_order_relaxed);
        const detail::AAudioPcmQueue::PopResult popped =
            queue_.pop(output, frames);
        if (popped.frames > 0) {
            if (!anchorValid_.load(std::memory_order_acquire)) {
                anchorStreamFrame_.store(
                    callbackStart,
                    std::memory_order_relaxed);
                anchorMediaNanoseconds_.store(
                    popped.firstTimestampNanoseconds,
                    std::memory_order_relaxed);
                anchorValid_.store(
                    true,
                    std::memory_order_release);
            }
            lastAudioEndFrame_.store(
                callbackStart + popped.frames,
                std::memory_order_release);
            underrunReported_.store(
                false,
                std::memory_order_release);
        }
        if (popped.frames < frames) {
            std::memset(
                output
                    + static_cast<std::size_t>(popped.frames)
                        * static_cast<std::size_t>(channels),
                0,
                static_cast<std::size_t>(
                    frames - popped.frames)
                    * static_cast<std::size_t>(channels)
                    * sizeof(float));
            if (expectingData_.load(std::memory_order_acquire)
                && !draining_.load(std::memory_order_acquire)) {
                anchorValid_.store(
                    false,
                    std::memory_order_release);
                if (!underrunReported_.exchange(
                        true,
                        std::memory_order_acq_rel)) {
                    pendingUnderrun_.store(
                        true,
                        std::memory_order_release);
                }
            }
        }
        callbackFramePosition_.store(
            callbackStart + frames,
            std::memory_order_release);
        return AAUDIO_CALLBACK_RESULT_CONTINUE;
    }

    bool createStreamLocked(
        const AudioFormat& requested,
        bool exactFormat,
        std::string& error)
    {
        AAudioStreamBuilder* builder = nullptr;
        aaudio_result_t result =
            AAudio_createStreamBuilder(&builder);
        if (result != AAUDIO_OK || !builder) {
            error = "Could not create the AAudio stream builder: "
                + resultText(result);
            return false;
        }
        AAudioStreamBuilder_setDirection(
            builder,
            AAUDIO_DIRECTION_OUTPUT);
        AAudioStreamBuilder_setFormat(
            builder,
            AAUDIO_FORMAT_PCM_FLOAT);
        if (exactFormat && requested.sampleRate > 0) {
            AAudioStreamBuilder_setSampleRate(
                builder,
                requested.sampleRate);
        }
        AAudioStreamBuilder_setChannelCount(
            builder,
            requested.channels);
        AAudioStreamBuilder_setSharingMode(
            builder,
            AAUDIO_SHARING_MODE_SHARED);
        AAudioStreamBuilder_setPerformanceMode(
            builder,
            AAUDIO_PERFORMANCE_MODE_LOW_LATENCY);
        if (config_.device) {
            AAudioStreamBuilder_setDeviceId(
                builder,
                config_.device.value());
        }
        AAudioStreamBuilder_setDataCallback(
            builder,
            &Impl::dataCallback,
            this);
        AAudioStreamBuilder_setErrorCallback(
            builder,
            &Impl::errorCallback,
            this);

        AAudioStream* stream = nullptr;
        result = AAudioStreamBuilder_openStream(
            builder,
            &stream);
        AAudioStreamBuilder_delete(builder);
        if (result != AAUDIO_OK || !stream) {
            error = "Could not open the AAudio output stream: "
                + resultText(result);
            return false;
        }

        const int sampleRate =
            AAudioStream_getSampleRate(stream);
        const int channels =
            AAudioStream_getChannelCount(stream);
        const aaudio_format_t format =
            AAudioStream_getFormat(stream);
        if (sampleRate <= 0 || channels <= 0
            || channels > config_.maximumChannels
            || format != AAUDIO_FORMAT_PCM_FLOAT
            || (exactFormat
                && (sampleRate != requested.sampleRate
                    || channels != requested.channels))) {
            AAudioStream_close(stream);
            error =
                "AAudio did not provide the requested Float32 PCM format";
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
            AAudioStream_close(stream);
            error = "Could not allocate the bounded AAudio PCM queue";
            return false;
        }

        const int burst =
            AAudioStream_getFramesPerBurst(stream);
        const int capacity =
            AAudioStream_getBufferCapacityInFrames(stream);
        if (burst > 0 && capacity > 0) {
            const int requestedBuffer =
                std::min(
                    capacity,
                    burst * config_.bufferBursts);
            if (requestedBuffer > 0) {
                AAudioStream_setBufferSizeInFrames(
                    stream,
                    requestedBuffer);
            }
        }

        stream_ = stream;
        deviceFormat_ = {
            sampleRate,
            channels,
            SampleFormat::Float,
            defaultChannelLayout(channels),
        };
        activeDeviceId_ =
            AAudioStream_getDeviceId(stream);
        callbackChannels_.store(
            channels,
            std::memory_order_release);
        resetTimelineAndQueueLocked(true);
        pendingError_.store(AAUDIO_OK, std::memory_order_release);
        errorStream_.store(nullptr, std::memory_order_release);
        activeStream_.store(stream, std::memory_order_release);
        opened_ = true;
        started_ = false;
        paused_ = false;
        return true;
    }

    void closeStreamLocked() noexcept
    {
        opened_ = false;
        activeStream_.store(nullptr, std::memory_order_release);
        if (stream_) {
            if (started_) {
                AAudioStream_requestStop(stream_);
            }
            AAudioStream_close(stream_);
            stream_ = nullptr;
        }
        started_ = false;
        paused_ = false;
        activeDeviceId_ = 0;
        deviceFormat_ = {};
        callbackChannels_.store(0, std::memory_order_release);
        resetTimelineAndQueueLocked(true);
        pendingError_.store(AAUDIO_OK, std::memory_order_release);
        errorStream_.store(nullptr, std::memory_order_release);
    }

    bool startStreamLocked()
    {
        if (!stream_ || !opened_ || paused_) {
            return false;
        }
        if (started_) {
            return true;
        }
        const aaudio_result_t result =
            AAudioStream_requestStart(stream_);
        if (result != AAUDIO_OK) {
            return false;
        }
        started_ = true;
        return true;
    }

    void pauseStreamLocked()
    {
        if (!stream_ || !started_) {
            return;
        }
        const aaudio_result_t result =
            AAudioStream_requestPause(stream_);
        if (result == AAUDIO_OK) {
            waitForStateLocked(
                AAUDIO_STREAM_STATE_PAUSED,
                kStateTimeout);
        }
        started_ = false;
    }

    bool waitForStateLocked(
        aaudio_stream_state_t desired,
        std::chrono::milliseconds timeout)
    {
        if (!stream_) {
            return false;
        }
        const auto deadline =
            std::chrono::steady_clock::now() + timeout;
        aaudio_stream_state_t current =
            AAudioStream_getState(stream_);
        while (current != desired
               && current != AAUDIO_STREAM_STATE_DISCONNECTED
               && current != AAUDIO_STREAM_STATE_CLOSED
               && std::chrono::steady_clock::now() < deadline) {
            aaudio_stream_state_t next = current;
            const aaudio_result_t result =
                AAudioStream_waitForStateChange(
                    stream_,
                    current,
                    &next,
                    kStateWaitSliceNanoseconds);
            if (result != AAUDIO_OK
                && result != AAUDIO_ERROR_TIMEOUT) {
                return false;
            }
            current = next;
        }
        return current == desired;
    }

    void resetTimelineAndQueueLocked(
        bool resetFramePosition) noexcept
    {
        queue_.clear();
        expectingData_.store(false, std::memory_order_release);
        draining_.store(false, std::memory_order_release);
        anchorValid_.store(false, std::memory_order_release);
        anchorStreamFrame_.store(0, std::memory_order_relaxed);
        anchorMediaNanoseconds_.store(
            0,
            std::memory_order_relaxed);
        if (resetFramePosition) {
            callbackFramePosition_.store(
                0,
                std::memory_order_release);
        } else if (stream_) {
            const std::int64_t written =
                AAudioStream_getFramesWritten(stream_);
            if (written >= 0) {
                callbackFramePosition_.store(
                    written,
                    std::memory_order_release);
            }
        }
        lastAudioEndFrame_.store(0, std::memory_order_release);
        underrunReported_.store(false, std::memory_order_release);
        pendingUnderrun_.store(false, std::memory_order_release);
    }

    void finishDrainLocked() noexcept
    {
        pauseStreamLocked();
        expectingData_.store(false, std::memory_order_release);
        draining_.store(false, std::memory_order_release);
        anchorValid_.store(false, std::memory_order_release);
        underrunReported_.store(false, std::memory_order_release);
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

    void handleStreamError(
        AAudioStream* failedStream,
        aaudio_result_t error)
    {
        std::string detail;
        bool recovered = false;
        {
            std::lock_guard<std::mutex> lock(lifecycleMutex_);
            if (!opened_ || stream_ != failedStream) {
                return;
            }
            const AudioFormat restartFormat = deviceFormat_;
            const bool wasPaused = paused_;
            opened_ = false;
            activeStream_.store(
                nullptr,
                std::memory_order_release);
            AAudioStream_close(stream_);
            stream_ = nullptr;
            started_ = false;
            resetTimelineAndQueueLocked(true);

            std::string reopenError;
            if (error == AAUDIO_ERROR_DISCONNECTED
                && createStreamLocked(
                    restartFormat,
                    true,
                    reopenError)) {
                paused_ = wasPaused;
                disconnectRestarts_.fetch_add(
                    1,
                    std::memory_order_acq_rel);
                detail =
                    "AAudio route disconnected; output stream rebuilt";
                recovered = true;
            } else {
                opened_ = false;
                deviceFormat_ = {};
                activeDeviceId_ = 0;
                detail = "AAudio output failed: "
                    + resultText(error);
                if (!reopenError.empty()) {
                    detail += "; restart failed: " + reopenError;
                }
            }
        }
        publishEvent({
            recovered
                ? AudioSinkEventType::Underrun
                : (error == AAUDIO_ERROR_DISCONNECTED
                    ? AudioSinkEventType::DeviceLost
                    : AudioSinkEventType::Error),
            std::move(detail),
        });
    }

    void workerMain()
    {
        int routePoll = 0;
        while (!quitting_.load(std::memory_order_acquire)) {
            {
                std::unique_lock<std::mutex> lock(workerMutex_);
                workerChanged_.wait_for(
                    lock,
                    std::chrono::milliseconds(10),
                    [this] {
                        return quitting_.load(
                                   std::memory_order_acquire)
                            || pendingError_.load(
                                   std::memory_order_acquire)
                                != AAUDIO_OK;
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
                    "AAudio real-time callback underrun",
                });
            }
            const aaudio_result_t error =
                pendingError_.exchange(
                    AAUDIO_OK,
                    std::memory_order_acq_rel);
            AAudioStream* failedStream =
                errorStream_.exchange(
                    nullptr,
                    std::memory_order_acq_rel);
            if (error != AAUDIO_OK && failedStream) {
                handleStreamError(failedStream, error);
            }

            if (++routePoll >= 10) {
                routePoll = 0;
                std::lock_guard<std::mutex> lock(
                    lifecycleMutex_);
                if (opened_ && stream_) {
                    const int currentDevice =
                        AAudioStream_getDeviceId(stream_);
                    if (currentDevice > 0
                        && activeDeviceId_ > 0
                        && currentDevice != activeDeviceId_) {
                        activeDeviceId_ = currentDevice;
                        routeChanges_.fetch_add(
                            1,
                            std::memory_order_acq_rel);
                    }
                }
            }
        }
    }

    AAudioAudioSinkConfig config_;
    mutable std::mutex lifecycleMutex_;
    mutable std::mutex callbackMutex_;
    EventCallback callback_;
    AAudioStream* stream_ = nullptr;
    AudioFormat deviceFormat_;
    int activeDeviceId_ = 0;
    bool opened_ = false;
    bool started_ = false;
    bool paused_ = false;

    detail::AAudioPcmQueue queue_;
    std::atomic<AAudioStream*> activeStream_ { nullptr };
    std::atomic<int> callbackChannels_ { 0 };
    std::atomic<bool> expectingData_ { false };
    std::atomic<bool> draining_ { false };
    std::atomic<bool> anchorValid_ { false };
    std::atomic<std::int64_t> anchorStreamFrame_ { 0 };
    std::atomic<std::int64_t> anchorMediaNanoseconds_ { 0 };
    std::atomic<std::int64_t> callbackFramePosition_ { 0 };
    std::atomic<std::int64_t> lastAudioEndFrame_ { 0 };
    std::atomic<bool> underrunReported_ { false };
    std::atomic<bool> pendingUnderrun_ { false };

    std::thread worker_;
    std::mutex workerMutex_;
    std::condition_variable workerChanged_;
    std::atomic<bool> quitting_ { false };
    std::atomic<AAudioStream*> errorStream_ { nullptr };
    std::atomic<aaudio_result_t> pendingError_ { AAUDIO_OK };
    std::atomic<std::uint64_t> routeChanges_ { 0 };
    std::atomic<std::uint64_t> disconnectRestarts_ { 0 };
};

AAudioAudioSink::AAudioAudioSink(AAudioAudioSinkConfig config)
    : impl_(std::make_unique<Impl>(std::move(config)))
{
}

AAudioAudioSink::~AAudioAudioSink() = default;

AAudioAudioSink::AAudioAudioSink(AAudioAudioSink&&) noexcept =
    default;
AAudioAudioSink& AAudioAudioSink::operator=(
    AAudioAudioSink&&) noexcept = default;

AudioSinkCapabilities AAudioAudioSink::capabilities() const
{
    if (!impl_) {
        return {};
    }
    return {
        { SampleFormat::Float },
        8'000,
        384'000,
        impl_->maximumChannels(),
        true,
        true,
    };
}

void AAudioAudioSink::setEventCallback(EventCallback callback)
{
    if (impl_) {
        impl_->setEventCallback(std::move(callback));
    }
}

AudioSinkOpenResult AAudioAudioSink::open(
    const AudioFormat& decodedFormat)
{
    if (!impl_) {
        return {
            false,
            {},
            "The AAudio audio sink has been moved from",
        };
    }
    return impl_->open(decodedFormat);
}

void AAudioAudioSink::close() noexcept
{
    if (impl_) {
        impl_->close();
    }
}

void AAudioAudioSink::pause(bool paused)
{
    if (impl_) {
        impl_->pause(paused);
    }
}

void AAudioAudioSink::flush()
{
    if (impl_) {
        impl_->flush();
    }
}

bool AAudioAudioSink::write(const AudioBufferView& buffer)
{
    return impl_ && impl_->write(buffer);
}

bool AAudioAudioSink::drain()
{
    return impl_ && impl_->drain();
}

AudioSinkClock AAudioAudioSink::clock() const noexcept
{
    return impl_ ? impl_->clock() : AudioSinkClock {};
}

AudioFormat AAudioAudioSink::deviceFormat() const
{
    return impl_ ? impl_->deviceFormat() : AudioFormat {};
}

AAudioStreamInfo AAudioAudioSink::streamInfo() const noexcept
{
    return impl_ ? impl_->streamInfo() : AAudioStreamInfo {};
}

} // namespace qtav
