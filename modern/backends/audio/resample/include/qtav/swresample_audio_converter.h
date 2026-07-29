// SPDX-License-Identifier: LGPL-2.1-or-later
#pragma once

#include <memory>

#include <qtav/audio_converter.h>
#include <qtav/audio_resample_export.h>

namespace qtav {

class QTAV_AUDIO_RESAMPLE_EXPORT SwresampleAudioConverter final
    : public AudioFrameConverter {
public:
    SwresampleAudioConverter();
    ~SwresampleAudioConverter() override;

    SwresampleAudioConverter(SwresampleAudioConverter&&) noexcept;
    SwresampleAudioConverter& operator=(
        SwresampleAudioConverter&&) noexcept;
    SwresampleAudioConverter(const SwresampleAudioConverter&) = delete;
    SwresampleAudioConverter& operator=(
        const SwresampleAudioConverter&) = delete;

    AudioConverterOpenResult open(
        const AudioFormat& input,
        const AudioFormat& output) override;
    AudioConversionResult convert(const AudioFrame& frame) override;
    AudioConversionResult drain() override;
    bool reset() override;
    void close() noexcept override;

private:
    class Impl;
    std::unique_ptr<Impl> impl_;
};

} // namespace qtav
