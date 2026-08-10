// SPDX-License-Identifier: LGPL-2.1-or-later
#pragma once

#include <cstddef>
#include <cstdint>
#include <functional>
#include <string>
#include <vector>

#include <qtav/export.h>
#include <qtav/frame.h>

namespace qtav {

struct QTAV_CORE_EXPORT AudioFormat {
    int sampleRate = 0;
    int channels = 0;
    SampleFormat sampleFormat = SampleFormat::Unknown;
    std::string channelLayout;

    bool isValid() const noexcept
    {
        return sampleRate > 0 && channels > 0
            && sampleFormat != SampleFormat::Unknown;
    }
};

struct QTAV_CORE_EXPORT AudioSinkCapabilities {
    std::vector<SampleFormat> sampleFormats;
    int minimumSampleRate = 0;
    int maximumSampleRate = 0;
    int maximumChannels = 0;
    bool supportsPause = false;
    bool hasDeviceClock = false;
};

struct QTAV_CORE_EXPORT AudioSinkOpenResult {
    bool success = false;
    AudioFormat deviceFormat;
    std::string error;
};

enum class AudioSinkEventType {
    Underrun,
    DeviceLost,
    Error,
};

struct QTAV_CORE_EXPORT AudioSinkEvent {
    AudioSinkEventType type = AudioSinkEventType::Error;
    std::string detail;
};

struct QTAV_CORE_EXPORT AudioSinkClock {
    bool valid = false;
    // Device PCM position currently presented, anchored to the media
    // timestamp of the first accepted buffer after open/flush. Player maps
    // the physical elapsed portion back to media time when time stretching.
    std::int64_t positionMilliseconds = 0;
    // Informational queued/device latency; not included in the position.
    std::int64_t latencyMilliseconds = 0;
};

struct QTAV_CORE_EXPORT AudioBufferView {
    AudioFormat format;
    int samplesPerChannel = 0;
    std::vector<const std::uint8_t*> planes;
    std::vector<int> lineSizes;
    std::int64_t timestamp = 0;
    std::int64_t duration = 0;

    bool isValid() const noexcept
    {
        if (!format.isValid() || samplesPerChannel <= 0 || planes.empty()
            || planes.size() != lineSizes.size()) {
            return false;
        }
        for (std::size_t index = 0; index < planes.size(); ++index) {
            if (!planes[index] || lineSizes[index] <= 0) {
                return false;
            }
        }
        return true;
    }
};

QTAV_CORE_EXPORT AudioFormat audioFormat(const AudioFrame& frame);
QTAV_CORE_EXPORT AudioBufferView audioBufferView(const AudioFrame& frame);

class QTAV_CORE_EXPORT AudioSink {
public:
    using EventCallback = std::function<void(const AudioSinkEvent&)>;

    virtual ~AudioSink();

    virtual AudioSinkCapabilities capabilities() const = 0;
    virtual void setEventCallback(EventCallback callback) = 0;
    virtual AudioSinkOpenResult open(const AudioFormat& decodedFormat) = 0;
    virtual void close() noexcept = 0;
    virtual void pause(bool paused) = 0;
    virtual void flush() = 0;
    virtual bool write(const AudioBufferView& buffer) = 0;
    // Wait until all accepted buffers have been presented. Player calls this
    // after each completed playback segment, including loop boundaries, and
    // before close() at final natural end. The default is a no-op for sinks
    // that consume synchronously or do not queue.
    virtual bool drain();
    // Player samples this on its audio-output worker and publishes a cached
    // clock to position() callers.
    virtual AudioSinkClock clock() const noexcept = 0;
};

} // namespace qtav
