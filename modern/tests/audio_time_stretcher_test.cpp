// SPDX-License-Identifier: LGPL-2.1-or-later

#include <qtav/atempo_audio_time_stretcher.h>

#include <algorithm>
#include <cassert>
#include <cmath>
#include <cstdio>
#include <cstdint>
#include <cstdlib>
#include <cstring>
#include <vector>

#undef assert
#define assert(condition)                                                \
    do {                                                                 \
        if (!(condition)) {                                              \
            std::fprintf(                                                \
                stderr,                                                  \
                "require failed at %s:%d: %s\n",                        \
                __FILE__,                                                \
                __LINE__,                                                \
                #condition);                                             \
            std::abort();                                                \
        }                                                                \
    } while (false)

namespace {

constexpr int SampleRate = 48'000;
constexpr int ChannelCount = 2;
constexpr double Pi = 3.14159265358979323846;

const qtav::AudioFormat Format {
    SampleRate,
    ChannelCount,
    qtav::SampleFormat::Float,
    "stereo",
};

struct CapturedAudio {
    std::vector<float> samples;
    std::int64_t firstTimestamp = -1;
    std::int64_t lastTimestamp = -1;
};

void capture(
    const qtav::AudioTimeStretchResult& result,
    CapturedAudio& output)
{
    assert(result.success);
    for (const auto& buffer : result.buffers) {
        assert(buffer.isValid());
        assert(buffer.format.sampleRate == Format.sampleRate);
        assert(buffer.format.channels == Format.channels);
        assert(buffer.format.sampleFormat == Format.sampleFormat);
        assert(buffer.planes.size() == 1);
        assert(buffer.lineSizes.size() == 1);
        assert(buffer.lineSizes.front()
            == buffer.samplesPerChannel * ChannelCount
                * static_cast<int>(sizeof(float)));
        if (output.firstTimestamp < 0) {
            output.firstTimestamp = buffer.timestamp;
        } else {
            assert(buffer.timestamp == output.lastTimestamp);
        }
        output.lastTimestamp = buffer.timestamp + buffer.duration;
        const auto valueCount = static_cast<std::size_t>(
            buffer.samplesPerChannel * ChannelCount);
        const auto oldSize = output.samples.size();
        output.samples.resize(oldSize + valueCount);
        std::memcpy(
            output.samples.data() + oldSize,
            buffer.planes.front(),
            valueCount * sizeof(float));
    }
}

std::vector<float> sineWave(int sampleCount, double frequency)
{
    std::vector<float> samples(
        static_cast<std::size_t>(sampleCount * ChannelCount));
    for (int sample = 0; sample < sampleCount; ++sample) {
        const auto value = static_cast<float>(
            0.5 * std::sin(
                2.0 * Pi * frequency * static_cast<double>(sample)
                / static_cast<double>(SampleRate)));
        samples[static_cast<std::size_t>(sample * ChannelCount)] = value;
        samples[static_cast<std::size_t>(sample * ChannelCount + 1)] = value;
    }
    return samples;
}

double measuredFrequency(const CapturedAudio& audio)
{
    const auto frames = audio.samples.size() / ChannelCount;
    assert(frames > static_cast<std::size_t>(SampleRate / 2));
    const auto begin = frames / 10;
    const auto end = frames - begin;
    int crossings = 0;
    float previous = audio.samples[begin * ChannelCount];
    for (std::size_t sample = begin + 1; sample < end; ++sample) {
        const float current = audio.samples[sample * ChannelCount];
        if (previous <= 0.0F && current > 0.0F) {
            ++crossings;
        }
        previous = current;
    }
    const auto seconds = static_cast<double>(end - begin)
        / static_cast<double>(SampleRate);
    return static_cast<double>(crossings) / seconds;
}

CapturedAudio stretch(double rate, std::int64_t timestamp = 0)
{
    constexpr int inputSamples = SampleRate * 2;
    constexpr int chunkSamples = 480;
    auto input = sineWave(inputSamples, 440.0);
    qtav::AtempoAudioTimeStretcher stretcher;
    const auto opened = stretcher.open(Format, rate);
    assert(opened.success);

    CapturedAudio output;
    for (int offset = 0; offset < inputSamples; offset += chunkSamples) {
        const int count = std::min(chunkSamples, inputSamples - offset);
        qtav::AudioBufferView buffer {
            Format,
            count,
            {
                reinterpret_cast<const std::uint8_t*>(
                    input.data()
                    + static_cast<std::size_t>(offset * ChannelCount)),
            },
            {
                count * ChannelCount * static_cast<int>(sizeof(float)),
            },
            timestamp
                + static_cast<std::int64_t>(offset) * 1000 / SampleRate,
            static_cast<std::int64_t>(count) * 1000 / SampleRate,
        };
        capture(stretcher.process(buffer), output);
    }
    capture(stretcher.drain(), output);
    const auto repeatedDrain = stretcher.drain();
    assert(repeatedDrain.success);
    assert(repeatedDrain.buffers.empty());
    stretcher.close();
    return output;
}

void testPitchAndDuration(double rate)
{
    const auto output = stretch(rate);
    assert(output.firstTimestamp == 0);
    assert(output.lastTimestamp == 2'000);
    const auto outputFrames = output.samples.size() / ChannelCount;
    const auto expectedFrames = static_cast<double>(SampleRate * 2) / rate;
    assert(std::abs(
               static_cast<double>(outputFrames) - expectedFrames)
        < expectedFrames * 0.04);
    assert(std::abs(measuredFrequency(output) - 440.0) < 8.0);
}

void testResetAndDiscontinuity()
{
    qtav::AtempoAudioTimeStretcher stretcher;
    assert(stretcher.open(Format, 1.25).success);
    auto input = sineWave(SampleRate / 10, 440.0);
    qtav::AudioBufferView buffer {
        Format,
        SampleRate / 10,
        {
            reinterpret_cast<const std::uint8_t*>(input.data()),
        },
        { static_cast<int>(input.size() * sizeof(float)) },
        0,
        100,
    };
    CapturedAudio first;
    capture(stretcher.process(buffer), first);
    assert(stretcher.reset());
    buffer.timestamp = 2'000;
    CapturedAudio resetOutput;
    capture(stretcher.process(buffer), resetOutput);
    capture(stretcher.drain(), resetOutput);
    assert(resetOutput.firstTimestamp == 2'000);
    assert(resetOutput.lastTimestamp == 2'100);

    assert(stretcher.reset());
    buffer.timestamp = 3'000;
    CapturedAudio beforeDiscontinuity;
    capture(stretcher.process(buffer), beforeDiscontinuity);
    buffer.timestamp = 4'000;
    CapturedAudio discontinuous;
    capture(stretcher.process(buffer), discontinuous);
    capture(stretcher.drain(), discontinuous);
    assert(discontinuous.firstTimestamp == 4'000);
    assert(discontinuous.lastTimestamp == 4'100);
}

} // namespace

int main()
{
    testPitchAndDuration(0.75);
    testPitchAndDuration(1.5);
    testResetAndDiscontinuity();
    return 0;
}
