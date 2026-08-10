// SPDX-License-Identifier: LGPL-2.1-or-later
#pragma once

#include <memory>

#include <qtav/audio_time_stretcher.h>
#include <qtav/audio_timestretch_export.h>

namespace qtav {

// Pitch-preserving streaming time stretch implemented by FFmpeg's atempo
// filter. The reference backend accepts interleaved U8/S16/S32/Float/Double
// PCM and preserves that format at its output.
class QTAV_AUDIO_TIMESTRETCH_EXPORT AtempoAudioTimeStretcher final
    : public AudioTimeStretcher {
public:
    AtempoAudioTimeStretcher();
    ~AtempoAudioTimeStretcher() override;

    AtempoAudioTimeStretcher(AtempoAudioTimeStretcher&&) noexcept;
    AtempoAudioTimeStretcher& operator=(
        AtempoAudioTimeStretcher&&) noexcept;
    AtempoAudioTimeStretcher(const AtempoAudioTimeStretcher&) = delete;
    AtempoAudioTimeStretcher& operator=(
        const AtempoAudioTimeStretcher&) = delete;

    AudioTimeStretchOpenResult open(
        const AudioFormat& format,
        double playbackRate) override;
    AudioTimeStretchResult process(
        const AudioBufferView& buffer) override;
    AudioTimeStretchResult drain() override;
    bool reset() override;
    void close() noexcept override;

private:
    class Impl;
    std::unique_ptr<Impl> impl_;
};

} // namespace qtav
