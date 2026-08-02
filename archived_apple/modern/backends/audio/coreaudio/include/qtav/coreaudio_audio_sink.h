// SPDX-License-Identifier: LGPL-2.1-or-later
#pragma once

#if !defined(__APPLE__)
#  error "qtav/coreaudio_audio_sink.h is available only on macOS"
#endif

#include <TargetConditionals.h>
#if !TARGET_OS_OSX
#  error "qtav/coreaudio_audio_sink.h is available only on macOS"
#endif

#include <CoreAudio/AudioHardware.h>

#include <cstdint>
#include <memory>

#include <qtav/audio_coreaudio_export.h>
#include <qtav/audio_sink.h>

namespace qtav {

// AudioDeviceID is a non-owning process-local CoreAudio object identifier.
// CoreAudioDevice makes its native role explicit without leaking it into the
// generic AudioSink contract.
class QTAV_AUDIO_COREAUDIO_EXPORT CoreAudioDevice final {
public:
    CoreAudioDevice() noexcept = default;
    explicit CoreAudioDevice(AudioDeviceID value) noexcept;

    AudioDeviceID get() const noexcept;
    explicit operator bool() const noexcept;

private:
    AudioDeviceID value_ = kAudioObjectUnknown;
};

struct QTAV_AUDIO_COREAUDIO_EXPORT CoreAudioAudioSinkConfig {
    // An empty value follows the current default output device.
    CoreAudioDevice device;
    // The initial backend is intentionally limited to mono/stereo PCM.
    int maximumChannels = 2;
    // Each accepted decoded/converter buffer occupies one native queue slot.
    int queueBufferCount = 8;
};

class QTAV_AUDIO_COREAUDIO_EXPORT CoreAudioAudioSink final
    : public AudioSink {
public:
    explicit CoreAudioAudioSink(CoreAudioAudioSinkConfig config = {});
    ~CoreAudioAudioSink() override;

    CoreAudioAudioSink(CoreAudioAudioSink&&) noexcept;
    CoreAudioAudioSink& operator=(CoreAudioAudioSink&&) noexcept;
    CoreAudioAudioSink(const CoreAudioAudioSink&) = delete;
    CoreAudioAudioSink& operator=(const CoreAudioAudioSink&) = delete;

    AudioSinkCapabilities capabilities() const override;
    void setEventCallback(EventCallback callback) override;
    AudioSinkOpenResult open(const AudioFormat& decodedFormat) override;
    void close() noexcept override;
    void pause(bool paused) override;
    void flush() override;
    bool write(const AudioBufferView& buffer) override;
    bool drain() override;
    AudioSinkClock clock() const noexcept override;

    CoreAudioDevice device() const noexcept;
    AudioFormat deviceFormat() const;
    static CoreAudioDevice defaultOutputDevice() noexcept;

private:
    class Impl;
    std::unique_ptr<Impl> impl_;
};

} // namespace qtav
