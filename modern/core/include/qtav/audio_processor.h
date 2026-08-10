// SPDX-License-Identifier: LGPL-2.1-or-later
#pragma once

#include <string>
#include <vector>

#include <qtav/audio_sink.h>
#include <qtav/export.h>

namespace qtav {

struct QTAV_CORE_EXPORT AudioProcessorOpenResult {
    bool success = false;
    std::string error;
};

struct QTAV_CORE_EXPORT AudioProcessingResult {
    bool success = false;
    // Storage referenced by buffers remains processor-owned and is valid
    // until the next process(), drain(), reset(), close(), or destruction.
    // A successful result may contain no buffers while input is buffered.
    // The processor must preserve the opened PCM format and media timeline;
    // it may repartition samples between output buffers, but the complete
    // segment must contain the same physical sample count as its input.
    std::vector<AudioBufferView> buffers;
    std::string error;
};

// Optional streaming PCM-effect stage. Player places this after format
// conversion and pitch-preserving time stretch, and before AudioSink. It is
// intended for format-preserving effects such as gain, equalization, channel
// balance, or analysis with passthrough output. It does not own a device and
// does not change playback rate.
class QTAV_CORE_EXPORT AudioFrameProcessor {
public:
    virtual ~AudioFrameProcessor();

    virtual AudioProcessorOpenResult open(const AudioFormat& format) = 0;
    virtual AudioProcessingResult process(
        const AudioBufferView& buffer) = 0;
    // Emits buffered output; repeat until success with no buffers.
    virtual AudioProcessingResult drain() = 0;
    // Discards buffered state while retaining the configured format.
    virtual bool reset() = 0;
    virtual void close() noexcept = 0;
};

} // namespace qtav
