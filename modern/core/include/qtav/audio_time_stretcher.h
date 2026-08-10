// SPDX-License-Identifier: LGPL-2.1-or-later
#pragma once

#include <string>
#include <vector>

#include <qtav/audio_sink.h>
#include <qtav/export.h>

namespace qtav {

struct QTAV_CORE_EXPORT AudioTimeStretchOpenResult {
    bool success = false;
    std::string error;
};

struct QTAV_CORE_EXPORT AudioTimeStretchResult {
    bool success = false;
    // Storage referenced by buffers remains processor-owned and is valid
    // until the next process(), drain(), reset(), close(), or destruction.
    // A successful result may contain no buffers while input is buffered.
    // Every buffer keeps media-timeline timestamps and duration even though
    // its physical PCM sample count changes with playbackRate.
    std::vector<AudioBufferView> buffers;
    std::string error;
};

// Optional streaming pitch-preserving time-stretch stage. Player places this
// after AudioFrameConverter and before AudioSink, so it must preserve the
// configured PCM format and must not assume ownership of a platform device.
class QTAV_CORE_EXPORT AudioTimeStretcher {
public:
    virtual ~AudioTimeStretcher();

    virtual AudioTimeStretchOpenResult open(
        const AudioFormat& format,
        double playbackRate) = 0;
    virtual AudioTimeStretchResult process(
        const AudioBufferView& buffer) = 0;
    // Emits currently buffered output; repeat until success with no buffers.
    virtual AudioTimeStretchResult drain() = 0;
    // Discards buffered state while retaining the format and playback rate.
    virtual bool reset() = 0;
    virtual void close() noexcept = 0;
};

} // namespace qtav
