// SPDX-License-Identifier: LGPL-2.1-or-later

#include <qtav/atempo_audio_time_stretcher.h>

#include <algorithm>
#include <array>
#include <cmath>
#include <cstdint>
#include <cstring>
#include <cstdlib>
#include <iomanip>
#include <limits>
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

constexpr double kMinimumTempoFactor = 0.5;
constexpr double kMaximumTempoFactor = 2.0;
constexpr int kMaximumTempoFilters = 32;
constexpr std::int64_t kTimestampDiscontinuityToleranceMilliseconds = 2;

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
        error = "Invalid channel layout for "
            + std::to_string(format.channels) + " channels";
        return false;
    }
    return true;
}

std::string tempoDescription(double playbackRate, std::string& error)
{
    if (!std::isfinite(playbackRate) || playbackRate <= 0.0) {
        error = "Playback rate must be finite and positive";
        return {};
    }

    std::vector<double> factors;
    double remaining = playbackRate;
    while (remaining > kMaximumTempoFactor + 0.0000001) {
        factors.push_back(kMaximumTempoFactor);
        remaining /= kMaximumTempoFactor;
        if (static_cast<int>(factors.size()) >= kMaximumTempoFilters) {
            error = "Playback rate requires too many atempo stages";
            return {};
        }
    }
    while (remaining < kMinimumTempoFactor - 0.0000001) {
        factors.push_back(kMinimumTempoFactor);
        remaining /= kMinimumTempoFactor;
        if (static_cast<int>(factors.size()) >= kMaximumTempoFilters) {
            error = "Playback rate requires too many atempo stages";
            return {};
        }
    }
    factors.push_back(std::clamp(
        remaining,
        kMinimumTempoFactor,
        kMaximumTempoFactor));

    std::ostringstream description;
    description << std::setprecision(17);
    for (std::size_t index = 0; index < factors.size(); ++index) {
        if (index != 0) {
            description << ',';
        }
        description << "atempo=tempo=" << factors[index];
    }
    return description.str();
}

} // namespace

class AtempoAudioTimeStretcher::Impl {
public:
    ~Impl()
    {
        avfilter_graph_free(&graph_);
    }

    AudioTimeStretchResult failure(std::string error) const
    {
        return { false, {}, std::move(error) };
    }

    void resetTimeline() noexcept
    {
        inputSamples_ = 0;
        outputSamples_ = 0;
        anchorTimestamp_ = 0;
        expectedInputTimestamp_ = 0;
        inputEndTimestamp_ = 0;
        lastOutputTimestamp_ = 0;
        hasTimeline_ = false;
        drainStarted_ = false;
        drained_ = false;
        output_.clear();
    }

    bool createGraph(std::string& error)
    {
        avfilter_graph_free(&graph_);
        source_ = nullptr;
        sink_ = nullptr;

        const AVSampleFormat avFormat = sampleFormat(format_.sampleFormat);
        const char* formatName = av_get_sample_fmt_name(avFormat);
        if (avFormat == AV_SAMPLE_FMT_NONE || !formatName) {
            error = "The requested interleaved PCM format is not supported";
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
            error = "Could not allocate the audio filter graph";
            return false;
        }

        const auto* sourceFilter = avfilter_get_by_name("abuffer");
        const auto* sinkFilter = avfilter_get_by_name("abuffersink");
        if (!sourceFilter || !sinkFilter) {
            error = "The FFmpeg abuffer filters are unavailable";
            return false;
        }

        std::ostringstream sourceArguments;
        sourceArguments
            << "time_base=1/" << format_.sampleRate
            << ":sample_rate=" << format_.sampleRate
            << ":sample_fmt=" << formatName
            << ":channel_layout=" << layoutName.data();
        int result = avfilter_graph_create_filter(
            &source_,
            sourceFilter,
            "qtav_time_stretch_input",
            sourceArguments.str().c_str(),
            nullptr,
            graph_);
        if (result < 0) {
            error = "Could not create the time-stretch input: "
                + ffmpegError(result);
            return false;
        }
        result = avfilter_graph_create_filter(
            &sink_,
            sinkFilter,
            "qtav_time_stretch_output",
            nullptr,
            nullptr,
            graph_);
        if (result < 0) {
            error = "Could not create the time-stretch output: "
                + ffmpegError(result);
            return false;
        }

        std::string tempoError;
        const auto description = tempoDescription(playbackRate_, tempoError);
        if (description.empty()) {
            error = std::move(tempoError);
            return false;
        }

        AVFilterInOut* inputs = avfilter_inout_alloc();
        AVFilterInOut* outputs = avfilter_inout_alloc();
        if (!inputs || !outputs) {
            avfilter_inout_free(&inputs);
            avfilter_inout_free(&outputs);
            error = "Could not allocate the time-stretch graph endpoints";
            return false;
        }
        outputs->name = av_strdup("in");
        outputs->filter_ctx = source_;
        outputs->pad_idx = 0;
        outputs->next = nullptr;
        inputs->name = av_strdup("out");
        inputs->filter_ctx = sink_;
        inputs->pad_idx = 0;
        inputs->next = nullptr;
        if (!inputs->name || !outputs->name) {
            avfilter_inout_free(&inputs);
            avfilter_inout_free(&outputs);
            error = "Could not name the time-stretch graph endpoints";
            return false;
        }

        result = avfilter_graph_parse_ptr(
            graph_,
            description.c_str(),
            &inputs,
            &outputs,
            nullptr);
        avfilter_inout_free(&inputs);
        avfilter_inout_free(&outputs);
        if (result < 0) {
            error = "Could not parse the atempo graph: "
                + ffmpegError(result);
            return false;
        }
        result = avfilter_graph_config(graph_, nullptr);
        if (result < 0) {
            error = "Could not configure the atempo graph: "
                + ffmpegError(result);
            return false;
        }
        if (av_buffersink_get_format(sink_) != avFormat
            || av_buffersink_get_sample_rate(sink_)
                != format_.sampleRate) {
            error = "The atempo graph changed the negotiated PCM format";
            return false;
        }
        return true;
    }

    std::int64_t outputMediaTimestamp(std::int64_t samples) const noexcept
    {
        if (format_.sampleRate <= 0) {
            return anchorTimestamp_;
        }
        return anchorTimestamp_
            + static_cast<std::int64_t>(std::llround(
                static_cast<double>(samples) * 1000.0 * playbackRate_
                / static_cast<double>(format_.sampleRate)));
    }

    AudioTimeStretchResult collect(bool final)
    {
        output_.clear();
        int totalSamples = 0;
        while (true) {
            AVFrame* frame = av_frame_alloc();
            if (!frame) {
                return failure("Could not allocate filtered audio output");
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
                    "Could not read stretched audio: "
                    + ffmpegError(result));
            }
            if (frame->format != sampleFormat(format_.sampleFormat)
                || frame->sample_rate != format_.sampleRate
                || frame->ch_layout.nb_channels != format_.channels
                || frame->nb_samples <= 0 || !frame->extended_data
                || !frame->extended_data[0]) {
                av_frame_free(&frame);
                return failure(
                    "The atempo graph returned an unexpected PCM format");
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
                        - output_.size()) {
                av_frame_free(&frame);
                return failure("The stretched audio buffer is too large");
            }
            const auto oldSize = output_.size();
            output_.resize(oldSize + static_cast<std::size_t>(bytes));
            std::memcpy(
                output_.data() + oldSize,
                frame->extended_data[0],
                static_cast<std::size_t>(bytes));
            if (frame->nb_samples
                > std::numeric_limits<int>::max() - totalSamples) {
                av_frame_free(&frame);
                return failure("The stretched sample count is too large");
            }
            totalSamples += frame->nb_samples;
            av_frame_free(&frame);
        }

        AudioTimeStretchResult result { true, {}, {} };
        if (totalSamples <= 0) {
            return result;
        }
        const auto start = hasTimeline_
            ? lastOutputTimestamp_
            : static_cast<std::int64_t>(0);
        outputSamples_ += totalSamples;
        auto end = outputMediaTimestamp(outputSamples_);
        if (hasTimeline_) {
            end = std::min(end, inputEndTimestamp_);
            if (final && drained_) {
                end = inputEndTimestamp_;
            }
        }
        end = std::max(start, end);
        lastOutputTimestamp_ = end;
        result.buffers.push_back({
            format_,
            totalSamples,
            { output_.data() },
            { static_cast<int>(output_.size()) },
            start,
            end - start,
        });
        return result;
    }

    mutable std::mutex mutex_;
    AVFilterGraph* graph_ = nullptr;
    AVFilterContext* source_ = nullptr;
    AVFilterContext* sink_ = nullptr;
    AudioFormat format_;
    double playbackRate_ = 1.0;
    std::vector<std::uint8_t> output_;
    std::int64_t inputSamples_ = 0;
    std::int64_t outputSamples_ = 0;
    std::int64_t anchorTimestamp_ = 0;
    std::int64_t expectedInputTimestamp_ = 0;
    std::int64_t inputEndTimestamp_ = 0;
    std::int64_t lastOutputTimestamp_ = 0;
    bool hasTimeline_ = false;
    bool drainStarted_ = false;
    bool drained_ = false;
};

AtempoAudioTimeStretcher::AtempoAudioTimeStretcher()
    : impl_(std::make_unique<Impl>())
{
}

AtempoAudioTimeStretcher::~AtempoAudioTimeStretcher() = default;
AtempoAudioTimeStretcher::AtempoAudioTimeStretcher(
    AtempoAudioTimeStretcher&&) noexcept = default;
AtempoAudioTimeStretcher& AtempoAudioTimeStretcher::operator=(
    AtempoAudioTimeStretcher&&) noexcept = default;

AudioTimeStretchOpenResult AtempoAudioTimeStretcher::open(
    const AudioFormat& format,
    double playbackRate)
{
    if (!impl_) {
        return { false, "The time stretcher has been moved from" };
    }
    if (!format.isValid()) {
        return { false, "The PCM format must be valid" };
    }
    if (sampleFormat(format.sampleFormat) == AV_SAMPLE_FMT_NONE) {
        return {
            false,
            "The reference time stretcher requires interleaved PCM",
        };
    }
    std::string tempoError;
    if (tempoDescription(playbackRate, tempoError).empty()) {
        return { false, std::move(tempoError) };
    }

    std::lock_guard<std::mutex> lock(impl_->mutex_);
    impl_->format_ = format;
    impl_->playbackRate_ = playbackRate;
    impl_->resetTimeline();
    std::string error;
    if (!impl_->createGraph(error)) {
        avfilter_graph_free(&impl_->graph_);
        impl_->source_ = nullptr;
        impl_->sink_ = nullptr;
        impl_->format_ = {};
        return { false, std::move(error) };
    }
    return { true, {} };
}

AudioTimeStretchResult AtempoAudioTimeStretcher::process(
    const AudioBufferView& buffer)
{
    if (!impl_) {
        return { false, {}, "The time stretcher has been moved from" };
    }

    std::lock_guard<std::mutex> lock(impl_->mutex_);
    if (!impl_->graph_ || !impl_->source_ || !impl_->sink_) {
        return impl_->failure("The audio time stretcher is not open");
    }
    if (impl_->drainStarted_) {
        return impl_->failure(
            "The audio time stretcher must be reset after drain");
    }
    if (!buffer.isValid() || !sameFormat(buffer.format, impl_->format_)
        || buffer.planes.size() != 1 || buffer.lineSizes.size() != 1) {
        return impl_->failure(
            "The PCM buffer does not match the configured interleaved format");
    }

    const AVSampleFormat avFormat = sampleFormat(impl_->format_.sampleFormat);
    const int dataSize = av_samples_get_buffer_size(
        nullptr,
        impl_->format_.channels,
        buffer.samplesPerChannel,
        avFormat,
        1);
    if (dataSize <= 0 || dataSize > buffer.lineSizes.front()) {
        return impl_->failure("The PCM input buffer is smaller than declared");
    }

    if (impl_->hasTimeline_
        && std::llabs(
               buffer.timestamp - impl_->expectedInputTimestamp_)
            > kTimestampDiscontinuityToleranceMilliseconds) {
        std::string error;
        impl_->resetTimeline();
        if (!impl_->createGraph(error)) {
            return impl_->failure(
                "Could not reset at an audio discontinuity: "
                + error);
        }
    }
    if (!impl_->hasTimeline_) {
        impl_->anchorTimestamp_ = buffer.timestamp;
        impl_->lastOutputTimestamp_ = buffer.timestamp;
        impl_->hasTimeline_ = true;
    }

    AVFrame* frame = av_frame_alloc();
    if (!frame) {
        return impl_->failure("Could not allocate time-stretch input");
    }
    frame->format = avFormat;
    frame->sample_rate = impl_->format_.sampleRate;
    frame->nb_samples = buffer.samplesPerChannel;
    frame->pts = impl_->inputSamples_;
    ChannelLayout layout;
    std::string layoutError;
    if (!initializeLayout(layout, impl_->format_, layoutError)
        || av_channel_layout_copy(&frame->ch_layout, &layout.value) < 0) {
        av_frame_free(&frame);
        return impl_->failure(
            layoutError.empty()
                ? "Could not copy the time-stretch channel layout"
                : layoutError);
    }
    int result = av_frame_get_buffer(frame, 0);
    if (result >= 0) {
        std::memcpy(
            frame->data[0],
            buffer.planes.front(),
            static_cast<std::size_t>(dataSize));
        result = av_buffersrc_add_frame_flags(
            impl_->source_,
            frame,
            AV_BUFFERSRC_FLAG_KEEP_REF);
    }
    av_frame_free(&frame);
    if (result < 0) {
        return impl_->failure(
            "Could not submit PCM to atempo: " + ffmpegError(result));
    }

    impl_->inputSamples_ += buffer.samplesPerChannel;
    const auto fallbackDuration = static_cast<std::int64_t>(std::llround(
        static_cast<double>(buffer.samplesPerChannel) * 1000.0
        / static_cast<double>(impl_->format_.sampleRate)));
    const auto duration = buffer.duration > 0
        ? buffer.duration
        : std::max<std::int64_t>(0, fallbackDuration);
    impl_->expectedInputTimestamp_ = buffer.timestamp + duration;
    impl_->inputEndTimestamp_ = impl_->expectedInputTimestamp_;
    return impl_->collect(false);
}

AudioTimeStretchResult AtempoAudioTimeStretcher::drain()
{
    if (!impl_) {
        return { false, {}, "The time stretcher has been moved from" };
    }
    std::lock_guard<std::mutex> lock(impl_->mutex_);
    if (!impl_->graph_ || !impl_->source_ || !impl_->sink_) {
        return impl_->failure("The audio time stretcher is not open");
    }
    if (impl_->drained_) {
        return { true, {}, {} };
    }
    if (!impl_->drainStarted_) {
        const int result = av_buffersrc_add_frame_flags(
            impl_->source_,
            nullptr,
            0);
        if (result < 0) {
            return impl_->failure(
                "Could not drain atempo input: " + ffmpegError(result));
        }
        impl_->drainStarted_ = true;
    }
    return impl_->collect(true);
}

bool AtempoAudioTimeStretcher::reset()
{
    if (!impl_) {
        return false;
    }
    std::lock_guard<std::mutex> lock(impl_->mutex_);
    if (!impl_->graph_) {
        return false;
    }
    impl_->resetTimeline();
    std::string error;
    return impl_->createGraph(error);
}

void AtempoAudioTimeStretcher::close() noexcept
{
    if (!impl_) {
        return;
    }
    std::lock_guard<std::mutex> lock(impl_->mutex_);
    avfilter_graph_free(&impl_->graph_);
    impl_->source_ = nullptr;
    impl_->sink_ = nullptr;
    impl_->format_ = {};
    impl_->playbackRate_ = 1.0;
    impl_->resetTimeline();
}

} // namespace qtav
