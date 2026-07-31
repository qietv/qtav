// SPDX-License-Identifier: LGPL-2.1-or-later
#pragma once

#if !defined(__ANDROID__)
#  error "qtav/aaudio_audio_sink.h is available only on Android"
#endif

#include <cstdint>
#include <memory>

#include <qtav/audio_aaudio_export.h>
#include <qtav/audio_sink.h>

namespace qtav {

// AAudio device identifiers are process-local integer values. Zero follows
// the current default output route whenever the stream is opened or rebuilt.
class QTAV_AUDIO_AAUDIO_EXPORT AAudioDeviceId final {
public:
    AAudioDeviceId() noexcept = default;
    explicit AAudioDeviceId(std::int32_t value) noexcept;

    std::int32_t value() const noexcept;
    explicit operator bool() const noexcept;

private:
    std::int32_t value_ = 0;
};

struct QTAV_AUDIO_AAUDIO_EXPORT AAudioAudioSinkConfig {
    AAudioDeviceId device;
    // The initial backend intentionally negotiates mono/stereo Float32 PCM.
    int maximumChannels = 2;
    // Backend-owned PCM waiting for the real-time data callback.
    int maximumQueuedMilliseconds = 500;
    // Requested AAudio buffer size, measured in hardware bursts.
    int bufferBursts = 2;
    // Upper bound for waiting on accepted audio during drain().
    int drainTimeoutMilliseconds = 5'000;
};

struct QTAV_AUDIO_AAUDIO_EXPORT AAudioStreamInfo {
    AAudioDeviceId device;
    int bufferSizeInFrames = 0;
    int bufferCapacityInFrames = 0;
    int framesPerBurst = 0;
    int xRunCount = 0;
    std::uint64_t routeChanges = 0;
    std::uint64_t disconnectRestarts = 0;
};

class QTAV_AUDIO_AAUDIO_EXPORT AAudioAudioSink final
    : public AudioSink {
public:
    explicit AAudioAudioSink(AAudioAudioSinkConfig config = {});
    ~AAudioAudioSink() override;

    AAudioAudioSink(AAudioAudioSink&&) noexcept;
    AAudioAudioSink& operator=(AAudioAudioSink&&) noexcept;
    AAudioAudioSink(const AAudioAudioSink&) = delete;
    AAudioAudioSink& operator=(const AAudioAudioSink&) = delete;

    AudioSinkCapabilities capabilities() const override;
    void setEventCallback(EventCallback callback) override;
    AudioSinkOpenResult open(const AudioFormat& decodedFormat) override;
    void close() noexcept override;
    void pause(bool paused) override;
    void flush() override;
    bool write(const AudioBufferView& buffer) override;
    bool drain() override;
    AudioSinkClock clock() const noexcept override;

    AudioFormat deviceFormat() const;
    AAudioStreamInfo streamInfo() const noexcept;

private:
    class Impl;
    std::unique_ptr<Impl> impl_;
};

} // namespace qtav
