// SPDX-License-Identifier: LGPL-2.1-or-later
#pragma once

#if !defined(__OHOS__)
#  error "qtav/ohaudio_audio_sink.h is OHOS-only"
#endif

#include <cstdint>
#include <memory>

#include <qtav/audio_ohaudio_export.h>
#include <qtav/audio_sink.h>

namespace qtav {

enum class OHAudioLatencyMode {
    Normal,
    Fast,
};

struct QTAV_AUDIO_OHAUDIO_EXPORT OHAudioAudioSinkConfig {
    // OHAudio output routes are normally 48 kHz. The negotiated format is
    // returned from open() and may be converted by QtAV::AudioResample.
    int preferredSampleRate = 48'000;
    // The initial backend intentionally negotiates mono/stereo Float32 PCM.
    int maximumChannels = 2;
    // Backend-owned PCM waiting for the native real-time write callback.
    int maximumQueuedMilliseconds = 500;
    // Zero lets OHAudio select the native callback frame size.
    int callbackFrames = 0;
    int drainTimeoutMilliseconds = 5'000;
    OHAudioLatencyMode latencyMode = OHAudioLatencyMode::Fast;
};

struct QTAV_AUDIO_OHAUDIO_EXPORT OHAudioStreamInfo {
    std::uint32_t streamId = 0;
    int callbackFrames = 0;
    std::int64_t framesWritten = 0;
    std::uint32_t nativeUnderflowCount = 0;
    std::uint64_t callbackCount = 0;
    std::uint64_t renderedPcmFrames = 0;
    std::uint64_t callbackUnderruns = 0;
    std::uint64_t starts = 0;
    std::uint64_t flushes = 0;
    std::uint64_t drains = 0;
    std::uint64_t routeChanges = 0;
    std::uint64_t streamRestarts = 0;
    std::uint64_t interrupts = 0;
    OHAudioLatencyMode latencyMode = OHAudioLatencyMode::Normal;
};

class QTAV_AUDIO_OHAUDIO_EXPORT OHAudioAudioSink final
    : public AudioSink {
public:
    explicit OHAudioAudioSink(OHAudioAudioSinkConfig config = {});
    ~OHAudioAudioSink() override;

    OHAudioAudioSink(OHAudioAudioSink&&) noexcept;
    OHAudioAudioSink& operator=(OHAudioAudioSink&&) noexcept;
    OHAudioAudioSink(const OHAudioAudioSink&) = delete;
    OHAudioAudioSink& operator=(const OHAudioAudioSink&) = delete;

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
    OHAudioStreamInfo streamInfo() const noexcept;

private:
    class Impl;
    std::unique_ptr<Impl> impl_;
};

} // namespace qtav
