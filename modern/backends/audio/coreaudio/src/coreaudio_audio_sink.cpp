// SPDX-License-Identifier: LGPL-2.1-or-later

#include <qtav/coreaudio_audio_sink.h>

#include <AudioToolbox/AudioQueue.h>

#include <algorithm>
#include <chrono>
#include <cmath>
#include <condition_variable>
#include <cstdint>
#include <cstring>
#include <deque>
#include <limits>
#include <mutex>
#include <string>
#include <unordered_map>
#include <utility>
#include <vector>

namespace qtav {
namespace {

constexpr int kMinimumQueueBuffers = 3;
constexpr int kMaximumQueueBuffers = 32;
constexpr int kInitialBufferMilliseconds = 250;

AudioObjectPropertyAddress propertyAddress(
    AudioObjectPropertySelector selector,
    AudioObjectPropertyScope scope = kAudioObjectPropertyScopeGlobal)
{
    return {
        selector,
        scope,
        kAudioObjectPropertyElementMain,
    };
}

template <typename T>
bool audioProperty(
    AudioObjectID object,
    const AudioObjectPropertyAddress& address,
    T& value) noexcept
{
    UInt32 size = sizeof(value);
    return AudioObjectGetPropertyData(
               object,
               &address,
               0,
               nullptr,
               &size,
               &value)
        == noErr
        && size == sizeof(value);
}

CoreAudioDevice systemDefaultOutputDevice() noexcept
{
    AudioDeviceID device = kAudioObjectUnknown;
    const auto address = propertyAddress(
        kAudioHardwarePropertyDefaultOutputDevice);
    if (!audioProperty(kAudioObjectSystemObject, address, device)) {
        return {};
    }
    return CoreAudioDevice(device);
}

int outputChannelCount(AudioDeviceID device) noexcept
{
    const auto address = propertyAddress(
        kAudioDevicePropertyStreamConfiguration,
        kAudioDevicePropertyScopeOutput);
    UInt32 size = 0;
    if (AudioObjectGetPropertyDataSize(
            device,
            &address,
            0,
            nullptr,
            &size)
            != noErr
        || size < sizeof(AudioBufferList)) {
        return 0;
    }

    std::vector<std::uint8_t> storage(size);
    auto* buffers =
        reinterpret_cast<AudioBufferList*>(storage.data());
    if (AudioObjectGetPropertyData(
            device,
            &address,
            0,
            nullptr,
            &size,
            buffers)
        != noErr) {
        return 0;
    }

    std::uint64_t channels = 0;
    for (UInt32 index = 0; index < buffers->mNumberBuffers; ++index) {
        channels += buffers->mBuffers[index].mNumberChannels;
    }
    return static_cast<int>(std::min<std::uint64_t>(
        channels,
        static_cast<std::uint64_t>(std::numeric_limits<int>::max())));
}

int nominalSampleRate(AudioDeviceID device) noexcept
{
    Float64 rate = 0.0;
    const auto address = propertyAddress(
        kAudioDevicePropertyNominalSampleRate);
    if (!audioProperty(device, address, rate) || !std::isfinite(rate)
        || rate < 1.0
        || rate > static_cast<Float64>(
            std::numeric_limits<int>::max())) {
        return 0;
    }
    return static_cast<int>(std::llround(rate));
}

std::int64_t hardwareLatencyFrames(AudioDeviceID device) noexcept
{
    UInt32 latency = 0;
    UInt32 safetyOffset = 0;
    UInt32 bufferFrames = 0;
    audioProperty(
        device,
        propertyAddress(
            kAudioDevicePropertyLatency,
            kAudioDevicePropertyScopeOutput),
        latency);
    audioProperty(
        device,
        propertyAddress(
            kAudioDevicePropertySafetyOffset,
            kAudioDevicePropertyScopeOutput),
        safetyOffset);
    audioProperty(
        device,
        propertyAddress(
            kAudioDevicePropertyBufferFrameSize),
        bufferFrames);
    return static_cast<std::int64_t>(latency)
        + static_cast<std::int64_t>(safetyOffset)
        + static_cast<std::int64_t>(bufferFrames);
}

CFStringRef copyDeviceUid(AudioDeviceID device) noexcept
{
    CFStringRef uid = nullptr;
    const auto address =
        propertyAddress(kAudioDevicePropertyDeviceUID);
    if (!audioProperty(device, address, uid)) {
        return nullptr;
    }
    return uid;
}

std::string statusText(OSStatus status)
{
    const auto value = static_cast<std::uint32_t>(status);
    char text[5] {
        static_cast<char>((value >> 24U) & 0xffU),
        static_cast<char>((value >> 16U) & 0xffU),
        static_cast<char>((value >> 8U) & 0xffU),
        static_cast<char>(value & 0xffU),
        '\0',
    };
    const bool printable = std::all_of(
        text,
        text + 4,
        [](char character) {
            const auto value =
                static_cast<unsigned char>(character);
            return value >= 32U && value <= 126U;
        });
    if (printable) {
        return "'" + std::string(text, 4) + "'";
    }
    return std::to_string(status);
}

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
    std::int64_t frames,
    int sampleRate) noexcept
{
    if (frames <= 0 || sampleRate <= 0) {
        return 0;
    }
    return (frames * 1'000 + sampleRate / 2) / sampleRate;
}

} // namespace

class CoreAudioAudioSink::Impl {
public:
    struct QueuedBuffer {
        int frames = 0;
    };

    explicit Impl(CoreAudioAudioSinkConfig value)
        : config_(std::move(value))
    {
        config_.maximumChannels =
            std::clamp(config_.maximumChannels, 1, 2);
        config_.queueBufferCount = std::clamp(
            config_.queueBufferCount,
            kMinimumQueueBuffers,
            kMaximumQueueBuffers);
    }

    ~Impl()
    {
        shutdown();
    }

    static void outputCallback(
        void* userData,
        AudioQueueRef,
        AudioQueueBufferRef buffer)
    {
        auto* self = static_cast<Impl*>(userData);
        {
            std::lock_guard<std::mutex> lock(self->mutex_);
            const auto queued = self->queued_.find(buffer);
            if (queued != self->queued_.end()) {
                self->queuedFrames_ = std::max<std::int64_t>(
                    0,
                    self->queuedFrames_ - queued->second.frames);
                self->queued_.erase(queued);
            }
            if (self->open_) {
                self->available_.push_back(buffer);
            }
        }
        self->changed_.notify_all();
    }

    void shutdown() noexcept
    {
        AudioQueueRef queue = nullptr;
        {
            std::lock_guard<std::mutex> lock(mutex_);
            queue = queue_;
            queue_ = nullptr;
            open_ = false;
            started_ = false;
            paused_ = false;
            draining_ = false;
        }
        changed_.notify_all();

        if (queue) {
            AudioQueueStop(queue, true);
            AudioQueueDispose(queue, true);
        }

        {
            std::lock_guard<std::mutex> lock(mutex_);
            buffers_.clear();
            available_.clear();
            queued_.clear();
            queuedFrames_ = 0;
            hasTimelineAnchor_ = false;
            lastEnqueuedTimestamp_ = 0;
            device_ = {};
            deviceFormat_ = {};
            hardwareLatencyFrames_ = 0;
        }
        changed_.notify_all();
    }

    mutable std::mutex mutex_;
    std::condition_variable changed_;
    CoreAudioAudioSinkConfig config_;
    EventCallback callback_;
    AudioQueueRef queue_ = nullptr;
    std::vector<AudioQueueBufferRef> buffers_;
    std::deque<AudioQueueBufferRef> available_;
    std::unordered_map<AudioQueueBufferRef, QueuedBuffer> queued_;
    CoreAudioDevice device_;
    AudioFormat deviceFormat_;
    std::int64_t hardwareLatencyFrames_ = 0;
    std::int64_t queuedFrames_ = 0;
    std::int64_t timelineAnchorTimestamp_ = 0;
    Float64 timelineAnchorSample_ = 0.0;
    std::int64_t lastEnqueuedTimestamp_ = 0;
    bool open_ = false;
    bool started_ = false;
    bool paused_ = false;
    bool draining_ = false;
    bool hasTimelineAnchor_ = false;
};

CoreAudioDevice::CoreAudioDevice(AudioDeviceID value) noexcept
    : value_(value)
{
}

AudioDeviceID CoreAudioDevice::get() const noexcept
{
    return value_;
}

CoreAudioDevice::operator bool() const noexcept
{
    return value_ != kAudioObjectUnknown;
}

CoreAudioAudioSink::CoreAudioAudioSink(
    CoreAudioAudioSinkConfig config)
    : impl_(std::make_unique<Impl>(std::move(config)))
{
}

CoreAudioAudioSink::~CoreAudioAudioSink() = default;
CoreAudioAudioSink::CoreAudioAudioSink(
    CoreAudioAudioSink&&) noexcept = default;
CoreAudioAudioSink& CoreAudioAudioSink::operator=(
    CoreAudioAudioSink&&) noexcept = default;

AudioSinkCapabilities CoreAudioAudioSink::capabilities() const
{
    if (!impl_) {
        return {};
    }
    std::lock_guard<std::mutex> lock(impl_->mutex_);
    const auto configuredDevice = impl_->config_.device
        ? impl_->config_.device
        : systemDefaultOutputDevice();
    const int rate = configuredDevice
        ? nominalSampleRate(configuredDevice.get())
        : 0;
    const int channels = configuredDevice
        ? outputChannelCount(configuredDevice.get())
        : 0;
    return {
        { SampleFormat::Float },
        rate > 0 ? rate : 1,
        rate > 0 ? rate : 384'000,
        channels > 0
            ? std::min(channels, impl_->config_.maximumChannels)
            : impl_->config_.maximumChannels,
        true,
        true,
    };
}

void CoreAudioAudioSink::setEventCallback(EventCallback callback)
{
    if (!impl_) {
        return;
    }
    std::lock_guard<std::mutex> lock(impl_->mutex_);
    impl_->callback_ = std::move(callback);
}

AudioSinkOpenResult CoreAudioAudioSink::open(
    const AudioFormat& decodedFormat)
{
    if (!impl_) {
        return {
            false,
            {},
            "The CoreAudio audio sink has been moved from",
        };
    }
    if (!decodedFormat.isValid()) {
        return { false, {}, "The decoded audio format is invalid" };
    }

    impl_->shutdown();

    CoreAudioDevice configuredDevice;
    int maximumChannels = 0;
    int bufferCount = 0;
    {
        std::lock_guard<std::mutex> lock(impl_->mutex_);
        configuredDevice = impl_->config_.device;
        maximumChannels = impl_->config_.maximumChannels;
        bufferCount = impl_->config_.queueBufferCount;
    }
    const auto device = configuredDevice
        ? configuredDevice
        : systemDefaultOutputDevice();
    if (!device) {
        return {
            false,
            {},
            "No CoreAudio output device is available",
        };
    }

    const int sampleRate = nominalSampleRate(device.get());
    const int deviceChannels = outputChannelCount(device.get());
    if (sampleRate <= 0 || deviceChannels <= 0) {
        return {
            false,
            {},
            "The CoreAudio output device has no usable PCM format",
        };
    }

    const int channels = std::min(
        decodedFormat.channels,
        std::min(deviceChannels, maximumChannels));
    AudioFormat output {
        sampleRate,
        channels,
        SampleFormat::Float,
        defaultChannelLayout(channels),
    };
    if (!output.isValid()) {
        return {
            false,
            {},
            "The negotiated CoreAudio PCM format is invalid",
        };
    }

    AudioStreamBasicDescription description {};
    description.mSampleRate = output.sampleRate;
    description.mFormatID = kAudioFormatLinearPCM;
    description.mFormatFlags =
        kAudioFormatFlagIsFloat | kAudioFormatFlagIsPacked
        | kAudioFormatFlagsNativeEndian;
    description.mBytesPerPacket =
        static_cast<UInt32>(output.channels * sizeof(float));
    description.mFramesPerPacket = 1;
    description.mBytesPerFrame = description.mBytesPerPacket;
    description.mChannelsPerFrame =
        static_cast<UInt32>(output.channels);
    description.mBitsPerChannel = 32;

    AudioQueueRef queue = nullptr;
    OSStatus status = AudioQueueNewOutput(
        &description,
        &Impl::outputCallback,
        impl_.get(),
        nullptr,
        nullptr,
        0,
        &queue);
    if (status != noErr || !queue) {
        return {
            false,
            {},
            "Could not create a CoreAudio output queue: "
                + statusText(status),
        };
    }

    if (configuredDevice) {
        CFStringRef uid = copyDeviceUid(device.get());
        if (!uid) {
            AudioQueueDispose(queue, true);
            return {
                false,
                {},
                "Could not query the selected CoreAudio device UID",
            };
        }
        status = AudioQueueSetProperty(
            queue,
            kAudioQueueProperty_CurrentDevice,
            &uid,
            sizeof(uid));
        CFRelease(uid);
        if (status != noErr) {
            AudioQueueDispose(queue, true);
            return {
                false,
                {},
                "Could not select the CoreAudio output device: "
                    + statusText(status),
            };
        }
    }

    const auto bytesPerSecond =
        static_cast<std::uint64_t>(output.sampleRate)
        * static_cast<std::uint64_t>(description.mBytesPerFrame);
    const auto desiredBytes = std::max<std::uint64_t>(
        16'384,
        bytesPerSecond * kInitialBufferMilliseconds / 1'000);
    const auto initialBufferBytes = static_cast<UInt32>(
        std::min<std::uint64_t>(
            desiredBytes,
            std::numeric_limits<UInt32>::max()));

    std::vector<AudioQueueBufferRef> buffers;
    buffers.reserve(static_cast<std::size_t>(bufferCount));
    for (int index = 0; index < bufferCount; ++index) {
        AudioQueueBufferRef buffer = nullptr;
        status = AudioQueueAllocateBuffer(
            queue,
            initialBufferBytes,
            &buffer);
        if (status != noErr || !buffer) {
            AudioQueueDispose(queue, true);
            return {
                false,
                {},
                "Could not allocate CoreAudio queue buffers: "
                    + statusText(status),
            };
        }
        buffers.push_back(buffer);
    }

    {
        std::lock_guard<std::mutex> lock(impl_->mutex_);
        impl_->queue_ = queue;
        impl_->buffers_ = buffers;
        impl_->available_.assign(buffers.begin(), buffers.end());
        impl_->device_ = device;
        impl_->deviceFormat_ = output;
        impl_->hardwareLatencyFrames_ =
            hardwareLatencyFrames(device.get());
        impl_->queuedFrames_ = 0;
        impl_->lastEnqueuedTimestamp_ = 0;
        impl_->hasTimelineAnchor_ = false;
        impl_->open_ = true;
        impl_->started_ = false;
        impl_->paused_ = false;
        impl_->draining_ = false;
    }
    return { true, std::move(output), {} };
}

void CoreAudioAudioSink::close() noexcept
{
    if (impl_) {
        impl_->shutdown();
    }
}

void CoreAudioAudioSink::pause(bool paused)
{
    if (!impl_) {
        return;
    }

    EventCallback callback;
    AudioSinkEvent event;
    {
        std::lock_guard<std::mutex> lock(impl_->mutex_);
        if (!impl_->open_ || impl_->paused_ == paused) {
            return;
        }
        OSStatus status = noErr;
        if (paused && impl_->started_) {
            status = AudioQueuePause(impl_->queue_);
        } else if (!paused && impl_->started_) {
            status = AudioQueueStart(impl_->queue_, nullptr);
        }
        if (status != noErr) {
            callback = impl_->callback_;
            event = {
                AudioSinkEventType::Error,
                "Could not change CoreAudio pause state: "
                    + statusText(status),
            };
        } else {
            impl_->paused_ = paused;
        }
    }
    if (callback) {
        callback(event);
    }
}

void CoreAudioAudioSink::flush()
{
    if (!impl_) {
        return;
    }

    AudioQueueRef queue = nullptr;
    {
        std::lock_guard<std::mutex> lock(impl_->mutex_);
        if (!impl_->open_) {
            return;
        }
        queue = impl_->queue_;
    }

    const OSStatus status = AudioQueueReset(queue);
    EventCallback callback;
    AudioSinkEvent event;
    {
        std::lock_guard<std::mutex> lock(impl_->mutex_);
        if (impl_->queue_ != queue || !impl_->open_) {
            return;
        }
        impl_->available_.assign(
            impl_->buffers_.begin(),
            impl_->buffers_.end());
        impl_->queued_.clear();
        impl_->queuedFrames_ = 0;
        impl_->started_ = false;
        impl_->draining_ = false;
        impl_->hasTimelineAnchor_ = false;
        impl_->lastEnqueuedTimestamp_ = 0;
        if (status != noErr) {
            callback = impl_->callback_;
            event = {
                AudioSinkEventType::Error,
                "Could not reset the CoreAudio queue: "
                    + statusText(status),
            };
        }
    }
    impl_->changed_.notify_all();
    if (callback) {
        callback(event);
    }
}

bool CoreAudioAudioSink::write(const AudioBufferView& buffer)
{
    if (!impl_ || !buffer.isValid() || buffer.planes.size() != 1) {
        return false;
    }

    AudioQueueBufferRef nativeBuffer = nullptr;
    AudioQueueRef queue = nullptr;
    std::size_t dataBytes = 0;
    {
        std::unique_lock<std::mutex> lock(impl_->mutex_);
        if (!impl_->open_ || impl_->paused_
            || !sameFormat(buffer.format, impl_->deviceFormat_)) {
            return false;
        }

        const auto bytes = static_cast<std::uint64_t>(
                               buffer.samplesPerChannel)
            * static_cast<std::uint64_t>(buffer.format.channels)
            * sizeof(float);
        if (bytes == 0
            || bytes
                > static_cast<std::uint64_t>(
                    std::numeric_limits<UInt32>::max())
            || bytes
                > static_cast<std::uint64_t>(
                    buffer.lineSizes.front())) {
            return false;
        }
        dataBytes = static_cast<std::size_t>(bytes);

        const bool available = impl_->changed_.wait_for(
            lock,
            std::chrono::seconds(2),
            [this] {
                return !impl_->open_ || impl_->paused_
                    || !impl_->available_.empty();
            });
        if (!available || !impl_->open_ || impl_->paused_
            || impl_->available_.empty()) {
            return false;
        }
        nativeBuffer = impl_->available_.front();
        impl_->available_.pop_front();
        queue = impl_->queue_;

        if (nativeBuffer->mAudioDataBytesCapacity < dataBytes) {
            const OSStatus freeStatus =
                AudioQueueFreeBuffer(queue, nativeBuffer);
            if (freeStatus != noErr) {
                impl_->available_.push_front(nativeBuffer);
                return false;
            }
            const auto found = std::find(
                impl_->buffers_.begin(),
                impl_->buffers_.end(),
                nativeBuffer);
            AudioQueueBufferRef replacement = nullptr;
            const OSStatus allocateStatus = AudioQueueAllocateBuffer(
                queue,
                static_cast<UInt32>(dataBytes),
                &replacement);
            if (allocateStatus != noErr || !replacement) {
                if (found != impl_->buffers_.end()) {
                    impl_->buffers_.erase(found);
                }
                return false;
            }
            if (found != impl_->buffers_.end()) {
                *found = replacement;
            } else {
                impl_->buffers_.push_back(replacement);
            }
            nativeBuffer = replacement;
        }

        std::memcpy(
            nativeBuffer->mAudioData,
            buffer.planes.front(),
            dataBytes);
        nativeBuffer->mAudioDataByteSize =
            static_cast<UInt32>(dataBytes);

        AudioTimeStamp queueTime {};
        if (!impl_->hasTimelineAnchor_) {
            const OSStatus timeStatus = AudioQueueGetCurrentTime(
                queue,
                nullptr,
                &queueTime,
                nullptr);
            impl_->timelineAnchorTimestamp_ = buffer.timestamp;
            impl_->timelineAnchorSample_ =
                timeStatus == noErr
                    && (queueTime.mFlags
                        & kAudioTimeStampSampleTimeValid)
                ? queueTime.mSampleTime
                : 0.0;
            impl_->hasTimelineAnchor_ = true;
        }

        const auto endTimestamp = buffer.timestamp
            + millisecondsForFrames(
                buffer.samplesPerChannel,
                buffer.format.sampleRate);
        impl_->queued_.emplace(
            nativeBuffer,
            Impl::QueuedBuffer {
                buffer.samplesPerChannel,
            });
        impl_->queuedFrames_ += buffer.samplesPerChannel;
        impl_->lastEnqueuedTimestamp_ = std::max(
            impl_->lastEnqueuedTimestamp_,
            endTimestamp);

        const OSStatus enqueueStatus =
            AudioQueueEnqueueBuffer(queue, nativeBuffer, 0, nullptr);
        if (enqueueStatus != noErr) {
            impl_->queuedFrames_ -= buffer.samplesPerChannel;
            impl_->queued_.erase(nativeBuffer);
            impl_->available_.push_front(nativeBuffer);
            return false;
        }

        if (!impl_->started_) {
            const OSStatus startStatus =
                AudioQueueStart(queue, nullptr);
            if (startStatus != noErr) {
                return false;
            }
            impl_->started_ = true;
        }
    }
    return true;
}

bool CoreAudioAudioSink::drain()
{
    if (!impl_) {
        return false;
    }

    AudioQueueRef queue = nullptr;
    std::chrono::milliseconds timeout {};
    {
        std::lock_guard<std::mutex> lock(impl_->mutex_);
        if (!impl_->open_) {
            return false;
        }
        queue = impl_->queue_;
        impl_->draining_ = true;
        const auto queuedMilliseconds = millisecondsForFrames(
            impl_->queuedFrames_,
            impl_->deviceFormat_.sampleRate);
        timeout = std::chrono::milliseconds(
            std::clamp<std::int64_t>(
                queuedMilliseconds + 5'000,
                5'000,
                60'000));
    }

    AudioQueueFlush(queue);
    bool drained = false;
    {
        std::unique_lock<std::mutex> lock(impl_->mutex_);
        drained = impl_->changed_.wait_for(
            lock,
            timeout,
            [this, queue] {
                return impl_->queue_ != queue || !impl_->open_
                    || impl_->queued_.empty();
            });
        if (impl_->queue_ != queue || !impl_->open_) {
            return false;
        }
    }

    if (drained) {
        AudioQueueStop(queue, true);
    }
    {
        std::lock_guard<std::mutex> lock(impl_->mutex_);
        if (impl_->queue_ == queue) {
            impl_->started_ = false;
            impl_->draining_ = false;
        }
    }
    return drained;
}

AudioSinkClock CoreAudioAudioSink::clock() const noexcept
{
    if (!impl_) {
        return {};
    }

    std::lock_guard<std::mutex> lock(impl_->mutex_);
    const auto latency = millisecondsForFrames(
        impl_->hardwareLatencyFrames_ + impl_->queuedFrames_,
        impl_->deviceFormat_.sampleRate);
    if (!impl_->open_ || !impl_->started_
        || !impl_->hasTimelineAnchor_) {
        return { false, 0, latency };
    }

    AudioTimeStamp time {};
    const OSStatus status = AudioQueueGetCurrentTime(
        impl_->queue_,
        nullptr,
        &time,
        nullptr);
    if (status != noErr
        || !(time.mFlags & kAudioTimeStampSampleTimeValid)
        || !std::isfinite(time.mSampleTime)) {
        return { false, 0, latency };
    }

    const auto frames = static_cast<std::int64_t>(std::max<Float64>(
        0.0,
        time.mSampleTime - impl_->timelineAnchorSample_));
    const auto mapped = impl_->timelineAnchorTimestamp_
        + millisecondsForFrames(
            frames,
            impl_->deviceFormat_.sampleRate);
    const auto position = std::clamp(
        mapped,
        impl_->timelineAnchorTimestamp_,
        std::max(
            impl_->timelineAnchorTimestamp_,
            impl_->lastEnqueuedTimestamp_));
    const auto queuedLatency = std::max<std::int64_t>(
        0,
        impl_->lastEnqueuedTimestamp_ - position);
    const auto hardwareLatency = millisecondsForFrames(
        impl_->hardwareLatencyFrames_,
        impl_->deviceFormat_.sampleRate);
    return {
        true,
        std::max<std::int64_t>(0, position),
        hardwareLatency + queuedLatency,
    };
}

CoreAudioDevice CoreAudioAudioSink::device() const noexcept
{
    if (!impl_) {
        return {};
    }
    std::lock_guard<std::mutex> lock(impl_->mutex_);
    return impl_->device_;
}

AudioFormat CoreAudioAudioSink::deviceFormat() const
{
    if (!impl_) {
        return {};
    }
    std::lock_guard<std::mutex> lock(impl_->mutex_);
    return impl_->deviceFormat_;
}

CoreAudioDevice CoreAudioAudioSink::defaultOutputDevice() noexcept
{
    return systemDefaultOutputDevice();
}

} // namespace qtav
