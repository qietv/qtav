// SPDX-License-Identifier: LGPL-2.1-or-later

#include <qtav/volume_audio_frame_processor.h>

#include <array>
#include <cmath>
#include <cstdint>
#include <cstring>
#include <iomanip>
#include <limits>
#include <locale>
#include <mutex>
#include <sstream>
#include <string>
#include <utility>
#include <vector>

extern "C" {
#include <libavfilter/avfilter.h>
#include <libavfilter/buffersink.h>
#include <libavfilter/buffersrc.h>
#include <libavutil/channel_layout.h>
#include <libavutil/error.h>
#include <libavutil/frame.h>
#include <libavutil/samplefmt.h>
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
    default:
        return AV_SAMPLE_FMT_NONE;
    }
}

bool sameFormat(const AudioFormat& left, const AudioFormat& right)
{
    return left.sampleRate == right.sampleRate
        && left.channels == right.channels
        && left.sampleFormat == right.sampleFormat
        && left.channelLayout == right.channelLayout;
}

class ChannelLayout {
public:
    ~ChannelLayout() { av_channel_layout_uninit(&value); }

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
        error = "Invalid channel layout for "
            + std::to_string(format.channels) + " channels";
        return false;
    }
    return true;
}

} // namespace

class VolumeAudioFrameProcessor::Impl {
public:
    ~Impl() { avfilter_graph_free(&graph_); }

    AudioProcessingResult failure(std::string error) const
    {
        return { false, {}, std::move(error) };
    }

    void clearGraph() noexcept
    {
        avfilter_graph_free(&graph_);
        source_ = nullptr;
        sink_ = nullptr;
        drained_ = false;
        output_.clear();
    }

    bool createGraph(std::string& error)
    {
        clearGraph();
        const auto avFormat = sampleFormat(format_.sampleFormat);
        const char* formatName = av_get_sample_fmt_name(avFormat);
        if (avFormat == AV_SAMPLE_FMT_NONE || !formatName) {
            error = "The reference volume processor requires interleaved PCM";
            return false;
        }

        ChannelLayout layout;
        if (!initializeLayout(layout, format_, error)) {
            return false;
        }
        std::array<char, 128> layoutName {};
        const int described = av_channel_layout_describe(
            &layout.value,
            layoutName.data(),
            layoutName.size());
        if (described < 0) {
            error = "Could not describe the channel layout: "
                + ffmpegError(described);
            return false;
        }

        graph_ = avfilter_graph_alloc();
        if (!graph_) {
            error = "Could not allocate the volume filter graph";
            return false;
        }
        const auto* sourceFilter = avfilter_get_by_name("abuffer");
        const auto* sinkFilter = avfilter_get_by_name("abuffersink");
        if (!sourceFilter || !sinkFilter) {
            error = "The FFmpeg audio buffer filters are unavailable";
            return false;
        }

        std::ostringstream sourceArguments;
        sourceArguments.imbue(std::locale::classic());
        sourceArguments
            << "time_base=1/" << format_.sampleRate
            << ":sample_rate=" << format_.sampleRate
            << ":sample_fmt=" << formatName
            << ":channel_layout=" << layoutName.data();
        int result = avfilter_graph_create_filter(
            &source_,
            sourceFilter,
            "qtav_audio_filter_input",
            sourceArguments.str().c_str(),
            nullptr,
            graph_);
        if (result < 0) {
            error = "Could not create the volume input: "
                + ffmpegError(result);
            return false;
        }
        result = avfilter_graph_create_filter(
            &sink_,
            sinkFilter,
            "qtav_audio_filter_output",
            nullptr,
            nullptr,
            graph_);
        if (result < 0) {
            error = "Could not create the volume output: "
                + ffmpegError(result);
            return false;
        }

        std::ostringstream description;
        description.imbue(std::locale::classic());
        description << std::setprecision(17)
                    << "volume=volume=" << gain_
                    << ",aformat=sample_fmts=" << formatName
                    << ":sample_rates=" << format_.sampleRate
                    << ":channel_layouts=" << layoutName.data();
        AVFilterInOut* inputs = avfilter_inout_alloc();
        AVFilterInOut* outputs = avfilter_inout_alloc();
        if (!inputs || !outputs) {
            avfilter_inout_free(&inputs);
            avfilter_inout_free(&outputs);
            error = "Could not allocate the volume graph endpoints";
            return false;
        }
        outputs->name = av_strdup("in");
        outputs->filter_ctx = source_;
        outputs->pad_idx = 0;
        inputs->name = av_strdup("out");
        inputs->filter_ctx = sink_;
        inputs->pad_idx = 0;
        if (!inputs->name || !outputs->name) {
            avfilter_inout_free(&inputs);
            avfilter_inout_free(&outputs);
            error = "Could not name the volume graph endpoints";
            return false;
        }
        result = avfilter_graph_parse_ptr(
            graph_,
            description.str().c_str(),
            &inputs,
            &outputs,
            nullptr);
        avfilter_inout_free(&inputs);
        avfilter_inout_free(&outputs);
        if (result < 0) {
            error = "Could not parse the volume graph: "
                + ffmpegError(result);
            return false;
        }
        result = avfilter_graph_config(graph_, nullptr);
        if (result < 0) {
            error = "Could not configure the volume graph: "
                + ffmpegError(result);
            return false;
        }
        if (av_buffersink_get_format(sink_) != avFormat
            || av_buffersink_get_sample_rate(sink_) != format_.sampleRate) {
            error = "The volume graph changed the negotiated PCM format";
            return false;
        }
        return true;
    }

    AudioProcessingResult collect(const AudioBufferView* input)
    {
        output_.clear();
        int totalSamples = 0;
        while (true) {
            AVFrame* frame = av_frame_alloc();
            if (!frame) {
                return failure("Could not allocate volume filter output");
            }
            const int result = av_buffersink_get_frame(sink_, frame);
            if (result == AVERROR(EAGAIN)) {
                av_frame_free(&frame);
                break;
            }
            if (result == AVERROR_EOF) {
                av_frame_free(&frame);
                drained_ = true;
                break;
            }
            if (result < 0) {
                av_frame_free(&frame);
                return failure(
                    "Could not read volume filter output: "
                    + ffmpegError(result));
            }
            if (frame->format != sampleFormat(format_.sampleFormat)
                || frame->sample_rate != format_.sampleRate
                || frame->ch_layout.nb_channels != format_.channels
                || frame->nb_samples <= 0 || !frame->extended_data
                || !frame->extended_data[0]) {
                av_frame_free(&frame);
                return failure(
                    "The volume graph returned an unexpected PCM format");
            }
            const int bytes = av_samples_get_buffer_size(
                nullptr,
                format_.channels,
                frame->nb_samples,
                sampleFormat(format_.sampleFormat),
                1);
            if (bytes <= 0
                || static_cast<std::size_t>(bytes)
                    > std::numeric_limits<std::size_t>::max()
                        - output_.size()
                || frame->nb_samples
                    > std::numeric_limits<int>::max() - totalSamples) {
                av_frame_free(&frame);
                return failure("The volume filter output is too large");
            }
            const auto oldSize = output_.size();
            output_.resize(oldSize + static_cast<std::size_t>(bytes));
            std::memcpy(
                output_.data() + oldSize,
                frame->extended_data[0],
                static_cast<std::size_t>(bytes));
            totalSamples += frame->nb_samples;
            av_frame_free(&frame);
        }

        AudioProcessingResult result { true, {}, {} };
        if (!input) {
            if (totalSamples != 0) {
                return failure(
                    "The reference volume processor buffered unmapped PCM");
            }
            return result;
        }
        if (totalSamples != input->samplesPerChannel) {
            return failure(
                "The reference volume processor changed the sample count");
        }
        result.buffers.push_back({
            format_,
            totalSamples,
            { output_.data() },
            { static_cast<int>(output_.size()) },
            input->timestamp,
            input->duration,
        });
        return result;
    }

    mutable std::mutex mutex_;
    AVFilterGraph* graph_ = nullptr;
    AVFilterContext* source_ = nullptr;
    AVFilterContext* sink_ = nullptr;
    AudioFormat format_;
    double gain_ = 1.0;
    std::vector<std::uint8_t> output_;
    bool drained_ = false;
};

VolumeAudioFrameProcessor::VolumeAudioFrameProcessor(double gain)
    : impl_(std::make_unique<Impl>())
{
    impl_->gain_ = gain;
}

VolumeAudioFrameProcessor::~VolumeAudioFrameProcessor() = default;
VolumeAudioFrameProcessor::VolumeAudioFrameProcessor(
    VolumeAudioFrameProcessor&&) noexcept = default;
VolumeAudioFrameProcessor& VolumeAudioFrameProcessor::operator=(
    VolumeAudioFrameProcessor&&) noexcept = default;

double VolumeAudioFrameProcessor::gain() const noexcept
{
    return impl_ ? impl_->gain_ : 0.0;
}

AudioProcessorOpenResult VolumeAudioFrameProcessor::open(
    const AudioFormat& format)
{
    if (!impl_) {
        return { false, "The volume processor has been moved from" };
    }
    if (!format.isValid()) {
        return { false, "The PCM format must be valid" };
    }
    if (!std::isfinite(impl_->gain_) || impl_->gain_ < 0.0) {
        return { false, "Volume gain must be finite and non-negative" };
    }
    if (sampleFormat(format.sampleFormat) == AV_SAMPLE_FMT_NONE) {
        return {
            false,
            "The reference volume processor requires interleaved PCM",
        };
    }

    std::lock_guard<std::mutex> lock(impl_->mutex_);
    impl_->format_ = format;
    std::string error;
    if (!impl_->createGraph(error)) {
        impl_->clearGraph();
        impl_->format_ = {};
        return { false, std::move(error) };
    }
    return { true, {} };
}

AudioProcessingResult VolumeAudioFrameProcessor::process(
    const AudioBufferView& buffer)
{
    if (!impl_) {
        return { false, {}, "The volume processor has been moved from" };
    }
    std::lock_guard<std::mutex> lock(impl_->mutex_);
    if (!impl_->graph_ || !impl_->source_ || !impl_->sink_) {
        return impl_->failure("The volume processor is not open");
    }
    if (impl_->drained_) {
        return impl_->failure("The volume processor has already been drained");
    }
    if (!buffer.isValid() || !sameFormat(buffer.format, impl_->format_)
        || buffer.planes.size() != 1) {
        return impl_->failure(
            "The volume processor received an incompatible PCM buffer");
    }
    const int bytes = av_samples_get_buffer_size(
        nullptr,
        impl_->format_.channels,
        buffer.samplesPerChannel,
        sampleFormat(impl_->format_.sampleFormat),
        1);
    if (bytes <= 0 || buffer.lineSizes[0] < bytes) {
        return impl_->failure("The PCM input buffer is too small");
    }

    AVFrame* frame = av_frame_alloc();
    if (!frame) {
        return impl_->failure("Could not allocate volume filter input");
    }
    frame->format = sampleFormat(impl_->format_.sampleFormat);
    frame->sample_rate = impl_->format_.sampleRate;
    frame->nb_samples = buffer.samplesPerChannel;
    ChannelLayout layout;
    std::string layoutError;
    if (!initializeLayout(layout, impl_->format_, layoutError)
        || av_channel_layout_copy(&frame->ch_layout, &layout.value) < 0) {
        av_frame_free(&frame);
        return impl_->failure(
            layoutError.empty()
                ? "Could not copy the PCM channel layout"
                : std::move(layoutError));
    }
    int result = av_frame_get_buffer(frame, 1);
    if (result < 0) {
        av_frame_free(&frame);
        return impl_->failure(
            "Could not allocate volume filter input data: "
            + ffmpegError(result));
    }
    std::memcpy(frame->extended_data[0], buffer.planes[0], bytes);
    result = av_buffersrc_add_frame_flags(
        impl_->source_,
        frame,
        AV_BUFFERSRC_FLAG_KEEP_REF);
    av_frame_free(&frame);
    if (result < 0) {
        return impl_->failure(
            "Could not submit PCM to the volume graph: "
            + ffmpegError(result));
    }
    return impl_->collect(&buffer);
}

AudioProcessingResult VolumeAudioFrameProcessor::drain()
{
    if (!impl_) {
        return { false, {}, "The volume processor has been moved from" };
    }
    std::lock_guard<std::mutex> lock(impl_->mutex_);
    if (!impl_->graph_ || !impl_->source_ || !impl_->sink_) {
        return impl_->failure("The volume processor is not open");
    }
    if (!impl_->drained_) {
        const int result = av_buffersrc_add_frame_flags(
            impl_->source_,
            nullptr,
            0);
        if (result < 0 && result != AVERROR_EOF) {
            return impl_->failure(
                "Could not drain the volume graph: "
                + ffmpegError(result));
        }
    }
    return impl_->collect(nullptr);
}

bool VolumeAudioFrameProcessor::reset()
{
    if (!impl_) {
        return false;
    }
    std::lock_guard<std::mutex> lock(impl_->mutex_);
    if (!impl_->format_.isValid()) {
        return false;
    }
    std::string error;
    return impl_->createGraph(error);
}

void VolumeAudioFrameProcessor::close() noexcept
{
    if (!impl_) {
        return;
    }
    std::lock_guard<std::mutex> lock(impl_->mutex_);
    impl_->clearGraph();
    impl_->format_ = {};
}

} // namespace qtav
