// SPDX-License-Identifier: LGPL-2.1-or-later
#pragma once

#if !defined(_WIN32)
#  error "qtav/wasapi_audio_sink.h is available only on Windows"
#endif

#include <memory>
#include <string>

#include <qtav/audio_sink.h>
#include <qtav/audio_wasapi_export.h>

namespace qtav {

// WASAPI endpoint identifiers are stable strings owned by the application,
// unlike apartment-bound IMMDevice pointers. An empty identifier follows the
// current default multimedia output endpoint each time the sink is opened.
class QTAV_AUDIO_WASAPI_EXPORT WasapiEndpointId final {
public:
    WasapiEndpointId() = default;
    explicit WasapiEndpointId(std::wstring value);

    const std::wstring& value() const noexcept;
    explicit operator bool() const noexcept;

private:
    std::wstring value_;
};

struct QTAV_AUDIO_WASAPI_EXPORT WasapiAudioSinkConfig {
    WasapiEndpointId endpoint;
    // The first implementation intentionally negotiates mono/stereo PCM.
    int maximumChannels = 2;
    // Requested shared-mode engine buffer duration.
    int bufferMilliseconds = 100;
    // Backend-owned PCM queued in addition to the engine buffer.
    int maximumQueuedMilliseconds = 500;
};

class QTAV_AUDIO_WASAPI_EXPORT WasapiAudioSink final : public AudioSink {
public:
    explicit WasapiAudioSink(WasapiAudioSinkConfig config = {});
    ~WasapiAudioSink() override;

    WasapiAudioSink(WasapiAudioSink&&) noexcept;
    WasapiAudioSink& operator=(WasapiAudioSink&&) noexcept;
    WasapiAudioSink(const WasapiAudioSink&) = delete;
    WasapiAudioSink& operator=(const WasapiAudioSink&) = delete;

    AudioSinkCapabilities capabilities() const override;
    void setEventCallback(EventCallback callback) override;
    AudioSinkOpenResult open(const AudioFormat& decodedFormat) override;
    void close() noexcept override;
    void pause(bool paused) override;
    void flush() override;
    bool write(const AudioBufferView& buffer) override;
    bool drain() override;
    AudioSinkClock clock() const noexcept override;

    WasapiEndpointId endpoint() const;
    AudioFormat deviceFormat() const;

private:
    class Impl;
    std::unique_ptr<Impl> impl_;
};

} // namespace qtav
