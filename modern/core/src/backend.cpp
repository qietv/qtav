// SPDX-License-Identifier: LGPL-2.1-or-later

#include <qtav/audio_converter.h>
#include <qtav/audio_processor.h>
#include <qtav/audio_sink.h>
#include <qtav/audio_time_stretcher.h>
#include <qtav/hardware_frame.h>
#include <qtav/video_processor.h>
#include <qtav/video_render_api.h>

#include <cstddef>
#include <utility>

namespace qtav {

HardwareFrameMapping::~HardwareFrameMapping() = default;
HardwareFrameData::~HardwareFrameData() = default;
HardwareFrameInterop::~HardwareFrameInterop() = default;
VideoRenderAPI::~VideoRenderAPI() = default;

VideoRenderAttemptResult VideoRenderAPI::renderDetailed(
    const VideoFrame& frame)
{
    if (render(frame)) {
        return { VideoRenderAttemptStatus::Presented, 0, {} };
    }
    return {
        VideoRenderAttemptStatus::RetryAfterBackoff,
        1,
        "Legacy boolean renderer deferred the frame",
    };
}
AudioSink::~AudioSink() = default;

bool AudioSink::drain()
{
    return true;
}

AudioFrameConverter::~AudioFrameConverter() = default;
AudioTimeStretcher::~AudioTimeStretcher() = default;
AudioFrameProcessor::~AudioFrameProcessor() = default;
VideoFrameProcessor::~VideoFrameProcessor() = default;

AudioFormat audioFormat(const AudioFrame& frame)
{
    return {
        frame.sampleRate(),
        frame.channels(),
        frame.format(),
        frame.channelLayout(),
    };
}

AudioBufferView audioBufferView(const AudioFrame& frame)
{
    AudioBufferView result;
    if (!frame) {
        return result;
    }

    result.format = audioFormat(frame);
    result.samplesPerChannel = frame.samplesPerChannel();
    result.timestamp = frame.timestamp();
    result.duration = frame.duration();
    result.planes.reserve(static_cast<std::size_t>(frame.planeCount()));
    result.lineSizes.reserve(static_cast<std::size_t>(frame.planeCount()));
    for (int plane = 0; plane < frame.planeCount(); ++plane) {
        result.planes.push_back(frame.data(plane));
        result.lineSizes.push_back(frame.lineSize(plane));
    }
    return result;
}

VideoProcessorFormat videoProcessorFormat(const VideoFrame& frame) noexcept
{
    if (!frame) {
        return {};
    }
    const auto hardware = frame.hardwareFrame();
    return {
        frame.width(),
        frame.height(),
        frame.format(),
        frame.hasHardwareFrame(),
        hardware ? hardware.deviceType() : HardwareDeviceType::Unknown,
    };
}

HardwareFrame::HardwareFrame(std::shared_ptr<const HardwareFrameData> data)
    : data_(std::move(data))
{
}

HardwareFrame::operator bool() const noexcept
{
    return isValid();
}

bool HardwareFrame::isValid() const noexcept
{
    return data_ && data_->deviceType() != HardwareDeviceType::Unknown
        && data_->width() > 0 && data_->height() > 0;
}

HardwareDeviceType HardwareFrame::deviceType() const noexcept
{
    return data_ ? data_->deviceType() : HardwareDeviceType::Unknown;
}

int HardwareFrame::width() const noexcept
{
    return data_ ? data_->width() : 0;
}

int HardwareFrame::height() const noexcept
{
    return data_ ? data_->height() : 0;
}

PixelFormat HardwareFrame::softwareFormat() const noexcept
{
    return data_ ? data_->softwareFormat() : PixelFormat::Unknown;
}

NativeHandle HardwareFrame::nativeHandle(HardwareHandleType type) const noexcept
{
    return data_ ? data_->nativeHandle(type)
                 : NativeHandle { type, 0, 0 };
}

bool HardwareFrame::isMappable(HardwareMapMode mode) const noexcept
{
    return data_ && data_->isMappable(mode);
}

std::shared_ptr<HardwareFrameMapping> HardwareFrame::map(
    HardwareMapMode mode) const
{
    return data_ ? data_->map(mode) : nullptr;
}

} // namespace qtav
