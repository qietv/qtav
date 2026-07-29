// SPDX-License-Identifier: LGPL-2.1-or-later
#pragma once

#include <string>

#include <qtav/audio_sink.h>
#include <qtav/export.h>

namespace qtav {

struct QTAV_CORE_EXPORT AudioConverterOpenResult {
    bool success = false;
    std::string error;
};

struct QTAV_CORE_EXPORT AudioConversionResult {
    bool success = false;
    // Storage referenced by buffer remains converter-owned and is valid until
    // the next convert(), drain(), reset(), close(), or converter destruction.
    // A successful result may have an empty buffer while input is buffered.
    AudioBufferView buffer;
    std::string error;
};

class QTAV_CORE_EXPORT AudioFrameConverter {
public:
    virtual ~AudioFrameConverter();

    virtual AudioConverterOpenResult open(
        const AudioFormat& input,
        const AudioFormat& output) = 0;
    virtual AudioConversionResult convert(const AudioFrame& frame) = 0;
    // Emits currently buffered output; repeat until success with no buffer.
    virtual AudioConversionResult drain() = 0;
    // Discards buffered state while retaining the configured formats.
    virtual bool reset() = 0;
    virtual void close() noexcept = 0;
};

} // namespace qtav
