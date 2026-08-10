// SPDX-License-Identifier: LGPL-2.1-or-later
#pragma once

#include <memory>

#include <qtav/audio_filter_export.h>
#include <qtav/audio_processor.h>

namespace qtav {

// Narrow FFmpeg-backed reference implementation of AudioFrameProcessor. It
// applies a constant linear gain while preserving the negotiated interleaved
// PCM format, physical sample count, and media timeline.
class QTAV_AUDIO_FILTER_EXPORT VolumeAudioFrameProcessor final
    : public AudioFrameProcessor {
public:
    explicit VolumeAudioFrameProcessor(double gain = 1.0);
    ~VolumeAudioFrameProcessor() override;

    VolumeAudioFrameProcessor(VolumeAudioFrameProcessor&&) noexcept;
    VolumeAudioFrameProcessor& operator=(
        VolumeAudioFrameProcessor&&) noexcept;
    VolumeAudioFrameProcessor(const VolumeAudioFrameProcessor&) = delete;
    VolumeAudioFrameProcessor& operator=(
        const VolumeAudioFrameProcessor&) = delete;

    double gain() const noexcept;

    AudioProcessorOpenResult open(const AudioFormat& format) override;
    AudioProcessingResult process(
        const AudioBufferView& buffer) override;
    AudioProcessingResult drain() override;
    bool reset() override;
    void close() noexcept override;

private:
    class Impl;
    std::unique_ptr<Impl> impl_;
};

} // namespace qtav
