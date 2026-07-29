// SPDX-License-Identifier: LGPL-2.1-or-later

#include <qtav/swresample_audio_converter.h>

#include <algorithm>
#include <array>
#include <cstdint>
#include <limits>
#include <mutex>
#include <string>
#include <utility>
#include <vector>

extern "C" {
#include <libavutil/channel_layout.h>
#include <libavutil/error.h>
#include <libavutil/mathematics.h>
#include <libavutil/samplefmt.h>
#include <libswresample/swresample.h>
}

namespace qtav {
namespace {

std::string ffmpegError(int error)
{
    std::array<char, AV_ERROR_MAX_STRING_SIZE> text {};
    av_strerror(error, text.data(), text.size());
    return text.data();
}

AVSampleFormat sampleFormat(SampleFormat format) noexcept
{
    switch (format) {
    case SampleFormat::U8:
        return AV_SAMPLE_FMT_U8;
    case SampleFormat::S16:
        return AV_SAMPLE_FMT_S16;
    case SampleFormat::S32:
        return AV_SAMPLE_FMT_S32;
    case SampleFormat::Float:
        return AV_SAMPLE_FMT_FLT;
    case SampleFormat::Double:
        return AV_SAMPLE_FMT_DBL;
    case SampleFormat::U8Planar:
        return AV_SAMPLE_FMT_U8P;
    case SampleFormat::S16Planar:
        return AV_SAMPLE_FMT_S16P;
    case SampleFormat::S32Planar:
        return AV_SAMPLE_FMT_S32P;
    case SampleFormat::FloatPlanar:
        return AV_SAMPLE_FMT_FLTP;
    case SampleFormat::DoublePlanar:
        return AV_SAMPLE_FMT_DBLP;
    default:
        return AV_SAMPLE_FMT_NONE;
    }
}

bool sameInputFormat(
    const AudioFormat& expected,
    const AudioFrame& frame) noexcept
{
    return expected.sampleRate == frame.sampleRate()
        && expected.channels == frame.channels()
        && expected.sampleFormat == frame.format()
        && (expected.channelLayout.empty()
            || expected.channelLayout == frame.channelLayout());
}

class ChannelLayout {
public:
    ~ChannelLayout()
    {
        av_channel_layout_uninit(&value);
    }

    AVChannelLayout value {};
};

bool initializeLayout(
    ChannelLayout& layout,
    const AudioFormat& format,
    std::string& error)
{
    int result = -1;
    if (!format.channelLayout.empty()) {
        result = av_channel_layout_from_string(
            &layout.value,
            format.channelLayout.c_str());
    }
    if (result < 0) {
        av_channel_layout_uninit(&layout.value);
        av_channel_layout_default(&layout.value, format.channels);
    }
    if (!av_channel_layout_check(&layout.value)
        || layout.value.nb_channels != format.channels) {
        error = "Invalid channel layout for " + std::to_string(format.channels)
            + " channels";
        return false;
    }
    return true;
}

} // namespace

class SwresampleAudioConverter::Impl {
public:
    ~Impl()
    {
        swr_free(&context_);
    }

    AudioConversionResult failure(std::string error) const
    {
        return { false, {}, std::move(error) };
    }

    AudioConversionResult result(int samples)
    {
        AudioConversionResult converted;
        converted.success = true;
        if (samples <= 0) {
            return converted;
        }

        const AVSampleFormat outputSampleFormat =
            sampleFormat(output_.sampleFormat);
        int lineSize = 0;
        const int dataSize = av_samples_get_buffer_size(
            &lineSize,
            output_.channels,
            samples,
            outputSampleFormat,
            1);
        if (dataSize < 0) {
            return failure(
                "Could not describe converted audio: "
                + ffmpegError(dataSize));
        }
        buffer_.resize(static_cast<std::size_t>(dataSize));

        const auto startSample = outputSamples_;
        outputSamples_ += samples;
        const auto start = anchorTimestamp_
            + av_rescale_q(
                startSample,
                AVRational { 1, output_.sampleRate },
                AVRational { 1, 1000 });
        const auto end = anchorTimestamp_
            + av_rescale_q(
                outputSamples_,
                AVRational { 1, output_.sampleRate },
                AVRational { 1, 1000 });
        converted.buffer = {
            output_,
            samples,
            { buffer_.data() },
            { lineSize },
            start,
            std::max<std::int64_t>(0, end - start),
        };
        return converted;
    }

    bool prepareBuffer(int samples, std::uint8_t*& output, std::string& error)
    {
        int lineSize = 0;
        const int dataSize = av_samples_get_buffer_size(
            &lineSize,
            output_.channels,
            samples,
            sampleFormat(output_.sampleFormat),
            1);
        if (dataSize < 0) {
            error =
                "Could not allocate converted audio: " + ffmpegError(dataSize);
            return false;
        }
        buffer_.resize(static_cast<std::size_t>(dataSize));
        output = buffer_.data();
        return true;
    }

    void resetTimeline() noexcept
    {
        hasTimeline_ = false;
        anchorTimestamp_ = 0;
        outputSamples_ = 0;
    }

    mutable std::mutex mutex_;
    SwrContext* context_ = nullptr;
    AudioFormat input_;
    AudioFormat output_;
    std::vector<std::uint8_t> buffer_;
    std::int64_t anchorTimestamp_ = 0;
    std::int64_t outputSamples_ = 0;
    bool hasTimeline_ = false;
};

SwresampleAudioConverter::SwresampleAudioConverter()
    : impl_(std::make_unique<Impl>())
{
}

SwresampleAudioConverter::~SwresampleAudioConverter() = default;
SwresampleAudioConverter::SwresampleAudioConverter(
    SwresampleAudioConverter&&) noexcept = default;
SwresampleAudioConverter& SwresampleAudioConverter::operator=(
    SwresampleAudioConverter&&) noexcept = default;

AudioConverterOpenResult SwresampleAudioConverter::open(
    const AudioFormat& input,
    const AudioFormat& output)
{
    if (!impl_) {
        return { false, "The converter has been moved from" };
    }
    if (!input.isValid() || !output.isValid()) {
        return { false, "The input and output audio formats must be valid" };
    }

    const AVSampleFormat inputSampleFormat =
        sampleFormat(input.sampleFormat);
    const AVSampleFormat outputSampleFormat =
        sampleFormat(output.sampleFormat);
    if (inputSampleFormat == AV_SAMPLE_FMT_NONE
        || outputSampleFormat == AV_SAMPLE_FMT_NONE) {
        return { false, "The requested sample format is not supported" };
    }
    if (av_sample_fmt_is_planar(outputSampleFormat)) {
        return {
            false,
            "The reference audio converter requires interleaved PCM output",
        };
    }

    SwrContext* context = nullptr;
    int result = 0;
    std::string layoutError;
    ChannelLayout inputLayout;
    ChannelLayout outputLayout;
    if (!initializeLayout(inputLayout, input, layoutError)
        || !initializeLayout(outputLayout, output, layoutError)) {
        return { false, std::move(layoutError) };
    }
    result = swr_alloc_set_opts2(
        &context,
        &outputLayout.value,
        outputSampleFormat,
        output.sampleRate,
        &inputLayout.value,
        inputSampleFormat,
        input.sampleRate,
        0,
        nullptr);
    if (result >= 0) {
        result = swr_init(context);
    }
    if (result < 0) {
        swr_free(&context);
        return {
            false,
            "Could not initialize libswresample: " + ffmpegError(result),
        };
    }

    std::lock_guard<std::mutex> lock(impl_->mutex_);
    swr_free(&impl_->context_);
    impl_->context_ = context;
    impl_->input_ = input;
    impl_->output_ = output;
    impl_->buffer_.clear();
    impl_->resetTimeline();
    return { true, {} };
}

AudioConversionResult SwresampleAudioConverter::convert(
    const AudioFrame& frame)
{
    if (!impl_) {
        return { false, {}, "The converter has been moved from" };
    }

    std::lock_guard<std::mutex> lock(impl_->mutex_);
    if (!impl_->context_) {
        return impl_->failure("The audio converter is not open");
    }
    if (!frame || !sameInputFormat(impl_->input_, frame)) {
        return impl_->failure(
            "The decoded frame does not match the configured input format");
    }

    const auto delay = swr_get_delay(
        impl_->context_,
        impl_->input_.sampleRate);
    const auto outputCapacity64 = av_rescale_rnd(
        delay + frame.samplesPerChannel(),
        impl_->output_.sampleRate,
        impl_->input_.sampleRate,
        AV_ROUND_UP);
    if (outputCapacity64 <= 0
        || outputCapacity64 > std::numeric_limits<int>::max()) {
        return impl_->failure("The converted audio size is invalid");
    }
    const int outputCapacity = static_cast<int>(outputCapacity64);

    std::uint8_t* outputData = nullptr;
    std::string bufferError;
    if (!impl_->prepareBuffer(
            outputCapacity,
            outputData,
            bufferError)) {
        return impl_->failure(std::move(bufferError));
    }

    std::vector<const std::uint8_t*> inputData;
    inputData.reserve(static_cast<std::size_t>(frame.planeCount()));
    for (int plane = 0; plane < frame.planeCount(); ++plane) {
        inputData.push_back(frame.data(plane));
    }
    std::array<std::uint8_t*, 1> outputPlanes { outputData };
    const int converted = swr_convert(
        impl_->context_,
        outputPlanes.data(),
        outputCapacity,
        inputData.data(),
        frame.samplesPerChannel());
    if (converted < 0) {
        return impl_->failure(
            "Could not convert decoded audio: " + ffmpegError(converted));
    }
    if (!impl_->hasTimeline_) {
        impl_->anchorTimestamp_ = frame.timestamp();
        impl_->hasTimeline_ = true;
    }
    return impl_->result(converted);
}

AudioConversionResult SwresampleAudioConverter::drain()
{
    if (!impl_) {
        return { false, {}, "The converter has been moved from" };
    }

    std::lock_guard<std::mutex> lock(impl_->mutex_);
    if (!impl_->context_) {
        return impl_->failure("The audio converter is not open");
    }
    const auto delay = swr_get_delay(
        impl_->context_,
        impl_->input_.sampleRate);
    if (delay <= 0) {
        return { true, {}, {} };
    }
    const auto outputCapacity64 = av_rescale_rnd(
        delay,
        impl_->output_.sampleRate,
        impl_->input_.sampleRate,
        AV_ROUND_UP);
    if (outputCapacity64 <= 0
        || outputCapacity64 > std::numeric_limits<int>::max()) {
        return impl_->failure("The buffered audio size is invalid");
    }
    const int outputCapacity = static_cast<int>(outputCapacity64);

    std::uint8_t* outputData = nullptr;
    std::string bufferError;
    if (!impl_->prepareBuffer(
            outputCapacity,
            outputData,
            bufferError)) {
        return impl_->failure(std::move(bufferError));
    }
    std::array<std::uint8_t*, 1> outputPlanes { outputData };
    const int converted = swr_convert(
        impl_->context_,
        outputPlanes.data(),
        outputCapacity,
        nullptr,
        0);
    if (converted < 0) {
        return impl_->failure(
            "Could not drain converted audio: " + ffmpegError(converted));
    }
    return impl_->result(converted);
}

bool SwresampleAudioConverter::reset()
{
    if (!impl_) {
        return false;
    }
    std::lock_guard<std::mutex> lock(impl_->mutex_);
    if (!impl_->context_) {
        return false;
    }
    swr_close(impl_->context_);
    const int result = swr_init(impl_->context_);
    impl_->buffer_.clear();
    impl_->resetTimeline();
    return result >= 0;
}

void SwresampleAudioConverter::close() noexcept
{
    if (!impl_) {
        return;
    }
    std::lock_guard<std::mutex> lock(impl_->mutex_);
    swr_free(&impl_->context_);
    impl_->input_ = {};
    impl_->output_ = {};
    impl_->buffer_.clear();
    impl_->resetTimeline();
}

} // namespace qtav
