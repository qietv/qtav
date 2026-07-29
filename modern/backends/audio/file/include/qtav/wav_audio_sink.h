// SPDX-License-Identifier: LGPL-2.1-or-later
#pragma once

#include <cstdint>
#include <memory>
#include <string>

#include <qtav/audio_file_export.h>
#include <qtav/audio_sink.h>

namespace qtav {

struct QTAV_AUDIO_FILE_EXPORT WavAudioSinkConfig {
    std::string path;
    // Zero keeps the decoded value.
    int sampleRate = 0;
    // Zero keeps the decoded value.
    int channels = 0;
    // Must be an interleaved U8, S16, S32, Float, or Double format.
    SampleFormat sampleFormat = SampleFormat::S16;
    // Empty keeps the decoded layout when the channel count is unchanged.
    std::string channelLayout;
};

class QTAV_AUDIO_FILE_EXPORT WavAudioSink final : public AudioSink {
public:
    explicit WavAudioSink(WavAudioSinkConfig config);
    ~WavAudioSink() override;

    WavAudioSink(WavAudioSink&&) noexcept;
    WavAudioSink& operator=(WavAudioSink&&) noexcept;
    WavAudioSink(const WavAudioSink&) = delete;
    WavAudioSink& operator=(const WavAudioSink&) = delete;

    AudioSinkCapabilities capabilities() const override;
    void setEventCallback(EventCallback callback) override;
    AudioSinkOpenResult open(const AudioFormat& decodedFormat) override;
    void close() noexcept override;
    void pause(bool paused) override;
    void flush() override;
    bool write(const AudioBufferView& buffer) override;
    bool drain() override;
    AudioSinkClock clock() const noexcept override;

    std::string path() const;
    AudioFormat outputFormat() const;
    std::uint64_t bytesWritten() const noexcept;

private:
    class Impl;
    std::unique_ptr<Impl> impl_;
};

} // namespace qtav
