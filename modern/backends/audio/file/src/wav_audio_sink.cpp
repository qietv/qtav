// SPDX-License-Identifier: LGPL-2.1-or-later

#include <qtav/wav_audio_sink.h>

#include <algorithm>
#include <array>
#include <cstdint>
#include <fstream>
#include <limits>
#include <mutex>
#include <utility>
#include <vector>

namespace qtav {
namespace {

int bytesPerSample(SampleFormat format) noexcept
{
    switch (format) {
    case SampleFormat::U8:
        return 1;
    case SampleFormat::S16:
        return 2;
    case SampleFormat::S32:
    case SampleFormat::Float:
        return 4;
    case SampleFormat::Double:
        return 8;
    default:
        return 0;
    }
}

bool isFloatingPoint(SampleFormat format) noexcept
{
    return format == SampleFormat::Float
        || format == SampleFormat::Double;
}

bool sameFormat(const AudioFormat& left, const AudioFormat& right)
{
    return left.sampleRate == right.sampleRate
        && left.channels == right.channels
        && left.sampleFormat == right.sampleFormat
        && left.channelLayout == right.channelLayout;
}

bool isLittleEndian() noexcept
{
    const std::uint16_t value = 1;
    return *reinterpret_cast<const std::uint8_t*>(&value) == 1;
}

void write16(
    std::array<std::uint8_t, 44>& header,
    std::size_t offset,
    std::uint16_t value)
{
    header[offset] = static_cast<std::uint8_t>(value & 0xffU);
    header[offset + 1] =
        static_cast<std::uint8_t>((value >> 8U) & 0xffU);
}

void write32(
    std::array<std::uint8_t, 44>& header,
    std::size_t offset,
    std::uint32_t value)
{
    for (std::size_t byte = 0; byte < 4; ++byte) {
        header[offset + byte] = static_cast<std::uint8_t>(
            (value >> static_cast<unsigned>(byte * 8U)) & 0xffU);
    }
}

} // namespace

class WavAudioSink::Impl {
public:
    explicit Impl(WavAudioSinkConfig value)
        : config_(std::move(value))
    {
    }

    ~Impl()
    {
        closeLocked();
    }

    bool writeHeaderLocked(std::uint32_t dataBytes)
    {
        const int sampleBytes = bytesPerSample(output_.sampleFormat);
        if (sampleBytes <= 0) {
            return false;
        }
        const auto blockAlign = static_cast<std::uint32_t>(
            output_.channels * sampleBytes);
        const auto byteRate = static_cast<std::uint64_t>(output_.sampleRate)
            * blockAlign;
        if (output_.channels > std::numeric_limits<std::uint16_t>::max()
            || blockAlign > std::numeric_limits<std::uint16_t>::max()
            || byteRate > std::numeric_limits<std::uint32_t>::max()) {
            return false;
        }

        std::array<std::uint8_t, 44> header {};
        std::copy_n("RIFF", 4, header.begin());
        write32(header, 4, 36U + dataBytes);
        std::copy_n("WAVE", 4, header.begin() + 8);
        std::copy_n("fmt ", 4, header.begin() + 12);
        write32(header, 16, 16);
        write16(header, 20, isFloatingPoint(output_.sampleFormat) ? 3 : 1);
        write16(
            header,
            22,
            static_cast<std::uint16_t>(output_.channels));
        write32(
            header,
            24,
            static_cast<std::uint32_t>(output_.sampleRate));
        write32(header, 28, static_cast<std::uint32_t>(byteRate));
        write16(header, 32, static_cast<std::uint16_t>(blockAlign));
        write16(
            header,
            34,
            static_cast<std::uint16_t>(sampleBytes * 8));
        std::copy_n("data", 4, header.begin() + 36);
        write32(header, 40, dataBytes);
        stream_.write(
            reinterpret_cast<const char*>(header.data()),
            static_cast<std::streamsize>(header.size()));
        return stream_.good();
    }

    bool finalizeLocked()
    {
        if (!open_) {
            return true;
        }
        stream_.flush();
        const bool dataWritten = stream_.good();
        stream_.clear();
        stream_.seekp(0, std::ios::beg);
        const bool headerWritten = writeHeaderLocked(
            static_cast<std::uint32_t>(bytesWritten_));
        stream_.flush();
        return dataWritten && headerWritten && stream_.good();
    }

    void closeLocked() noexcept
    {
        if (!open_) {
            return;
        }
        finalizeLocked();
        stream_.close();
        open_ = false;
        paused_ = false;
    }

    mutable std::mutex mutex_;
    WavAudioSinkConfig config_;
    EventCallback callback_;
    AudioFormat output_;
    std::ofstream stream_;
    std::vector<std::uint8_t> byteSwapBuffer_;
    std::uint64_t bytesWritten_ = 0;
    bool open_ = false;
    bool paused_ = false;
};

WavAudioSink::WavAudioSink(WavAudioSinkConfig config)
    : impl_(std::make_unique<Impl>(std::move(config)))
{
}

WavAudioSink::~WavAudioSink() = default;
WavAudioSink::WavAudioSink(WavAudioSink&&) noexcept = default;
WavAudioSink& WavAudioSink::operator=(WavAudioSink&&) noexcept = default;

AudioSinkCapabilities WavAudioSink::capabilities() const
{
    if (!impl_) {
        return {};
    }
    std::lock_guard<std::mutex> lock(impl_->mutex_);
    return {
        {
            SampleFormat::U8,
            SampleFormat::S16,
            SampleFormat::S32,
            SampleFormat::Float,
            SampleFormat::Double,
        },
        impl_->config_.sampleRate > 0 ? impl_->config_.sampleRate : 1,
        impl_->config_.sampleRate > 0 ? impl_->config_.sampleRate : 384'000,
        impl_->config_.channels > 0 ? impl_->config_.channels : 64,
        true,
        false,
    };
}

void WavAudioSink::setEventCallback(EventCallback callback)
{
    if (!impl_) {
        return;
    }
    std::lock_guard<std::mutex> lock(impl_->mutex_);
    impl_->callback_ = std::move(callback);
}

AudioSinkOpenResult WavAudioSink::open(
    const AudioFormat& decodedFormat)
{
    if (!impl_) {
        return { false, {}, "The WAV audio sink has been moved from" };
    }
    if (!decodedFormat.isValid()) {
        return { false, {}, "The decoded audio format is invalid" };
    }

    std::lock_guard<std::mutex> lock(impl_->mutex_);
    impl_->closeLocked();
    if (impl_->config_.path.empty()) {
        return { false, {}, "The WAV output path is empty" };
    }

    AudioFormat output {
        impl_->config_.sampleRate > 0
            ? impl_->config_.sampleRate
            : decodedFormat.sampleRate,
        impl_->config_.channels > 0
            ? impl_->config_.channels
            : decodedFormat.channels,
        impl_->config_.sampleFormat,
        impl_->config_.channelLayout,
    };
    if (output.channelLayout.empty()
        && output.channels == decodedFormat.channels) {
        output.channelLayout = decodedFormat.channelLayout;
    }
    if (!output.isValid() || bytesPerSample(output.sampleFormat) <= 0) {
        return {
            false,
            {},
            "WAV output requires interleaved U8, S16, S32, Float, or "
            "Double PCM",
        };
    }

    const auto sampleBytes = static_cast<std::uint64_t>(
        bytesPerSample(output.sampleFormat));
    const auto blockAlign =
        static_cast<std::uint64_t>(output.channels) * sampleBytes;
    const auto byteRate =
        static_cast<std::uint64_t>(output.sampleRate) * blockAlign;
    if (output.channels > std::numeric_limits<std::uint16_t>::max()
        || blockAlign > std::numeric_limits<std::uint16_t>::max()
        || byteRate > std::numeric_limits<std::uint32_t>::max()) {
        return { false, {}, "The WAV output format exceeds RIFF limits" };
    }

    impl_->stream_.open(
        impl_->config_.path,
        std::ios::binary | std::ios::out | std::ios::trunc);
    if (!impl_->stream_.is_open()) {
        return {
            false,
            {},
            "Could not open WAV output file '" + impl_->config_.path + "'",
        };
    }
    impl_->output_ = output;
    impl_->bytesWritten_ = 0;
    impl_->paused_ = false;
    impl_->open_ = true;
    if (!impl_->writeHeaderLocked(0)) {
        impl_->closeLocked();
        return { false, {}, "Could not write the WAV header" };
    }
    return { true, std::move(output), {} };
}

void WavAudioSink::close() noexcept
{
    if (!impl_) {
        return;
    }
    std::lock_guard<std::mutex> lock(impl_->mutex_);
    impl_->closeLocked();
}

void WavAudioSink::pause(bool paused)
{
    if (!impl_) {
        return;
    }
    std::lock_guard<std::mutex> lock(impl_->mutex_);
    if (impl_->open_) {
        impl_->paused_ = paused;
    }
}

void WavAudioSink::flush()
{
    if (!impl_) {
        return;
    }
    std::lock_guard<std::mutex> lock(impl_->mutex_);
    if (impl_->open_) {
        impl_->stream_.flush();
    }
}

bool WavAudioSink::write(const AudioBufferView& buffer)
{
    if (!impl_) {
        return false;
    }
    std::lock_guard<std::mutex> lock(impl_->mutex_);
    if (!impl_->open_ || impl_->paused_ || !buffer.isValid()
        || !sameFormat(buffer.format, impl_->output_)
        || buffer.planes.size() != 1) {
        return false;
    }

    const auto sampleBytes =
        static_cast<std::uint64_t>(bytesPerSample(buffer.format.sampleFormat));
    const auto dataBytes = static_cast<std::uint64_t>(
                               buffer.samplesPerChannel)
        * static_cast<std::uint64_t>(buffer.format.channels) * sampleBytes;
    if (dataBytes > static_cast<std::uint64_t>(
                        std::numeric_limits<std::streamsize>::max())
        || dataBytes
            > static_cast<std::uint64_t>(buffer.lineSizes.front())
        || impl_->bytesWritten_ + dataBytes
            > std::numeric_limits<std::uint32_t>::max() - 36ULL) {
        return false;
    }

    const auto* data = buffer.planes.front();
    if (!isLittleEndian() && sampleBytes > 1) {
        impl_->byteSwapBuffer_.resize(
            static_cast<std::size_t>(dataBytes));
        for (std::uint64_t offset = 0; offset < dataBytes;
             offset += sampleBytes) {
            std::reverse_copy(
                data + offset,
                data + offset + sampleBytes,
                impl_->byteSwapBuffer_.data() + offset);
        }
        data = impl_->byteSwapBuffer_.data();
    }
    impl_->stream_.write(
        reinterpret_cast<const char*>(data),
        static_cast<std::streamsize>(dataBytes));
    if (!impl_->stream_.good()) {
        return false;
    }
    impl_->bytesWritten_ += dataBytes;
    return true;
}

bool WavAudioSink::drain()
{
    if (!impl_) {
        return false;
    }
    std::lock_guard<std::mutex> lock(impl_->mutex_);
    if (!impl_->open_) {
        return false;
    }
    impl_->stream_.flush();
    return impl_->stream_.good();
}

AudioSinkClock WavAudioSink::clock() const noexcept
{
    return {};
}

std::string WavAudioSink::path() const
{
    if (!impl_) {
        return {};
    }
    std::lock_guard<std::mutex> lock(impl_->mutex_);
    return impl_->config_.path;
}

AudioFormat WavAudioSink::outputFormat() const
{
    if (!impl_) {
        return {};
    }
    std::lock_guard<std::mutex> lock(impl_->mutex_);
    return impl_->output_;
}

std::uint64_t WavAudioSink::bytesWritten() const noexcept
{
    if (!impl_) {
        return 0;
    }
    std::lock_guard<std::mutex> lock(impl_->mutex_);
    return impl_->bytesWritten_;
}

} // namespace qtav
