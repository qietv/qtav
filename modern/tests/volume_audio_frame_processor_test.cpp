// SPDX-License-Identifier: LGPL-2.1-or-later

#include <qtav/volume_audio_frame_processor.h>

#include <cassert>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <vector>

namespace {

#define require(condition)                                               \
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

qtav::AudioBufferView view(std::vector<float>& samples)
{
    return {
        { 48'000, 2, qtav::SampleFormat::Float, "stereo" },
        static_cast<int>(samples.size() / 2),
        { reinterpret_cast<const std::uint8_t*>(samples.data()) },
        { static_cast<int>(samples.size() * sizeof(float)) },
        123,
        4,
    };
}

void requireSamples(
    const qtav::AudioProcessingResult& result,
    const std::vector<float>& expected)
{
    require(result.success);
    require(result.buffers.size() == 1);
    const auto& output = result.buffers.front();
    require(output.isValid());
    require(output.samplesPerChannel == 4);
    require(output.timestamp == 123);
    require(output.duration == 4);
    require(output.planes.size() == 1);
    require(output.lineSizes[0]
        == static_cast<int>(expected.size() * sizeof(float)));
    const auto* values = reinterpret_cast<const float*>(output.planes[0]);
    for (std::size_t index = 0; index < expected.size(); ++index) {
        require(std::abs(values[index] - expected[index]) < 0.00001F);
    }
}

} // namespace

int main()
{
    qtav::VolumeAudioFrameProcessor processor(0.5);
    require(std::abs(processor.gain() - 0.5) < 0.000001);
    const qtav::AudioFormat format {
        48'000,
        2,
        qtav::SampleFormat::Float,
        "stereo",
    };
    require(processor.open(format).success);

    std::vector<float> samples {
        -1.0F,
        -0.5F,
        0.0F,
        0.25F,
        0.5F,
        0.75F,
        1.0F,
        0.125F,
    };
    requireSamples(
        processor.process(view(samples)),
        { -0.5F, -0.25F, 0.0F, 0.125F, 0.25F, 0.375F, 0.5F,
          0.0625F });
    const auto drained = processor.drain();
    require(drained.success);
    require(drained.buffers.empty());

    require(processor.reset());
    requireSamples(
        processor.process(view(samples)),
        { -0.5F, -0.25F, 0.0F, 0.125F, 0.25F, 0.375F, 0.5F,
          0.0625F });
    processor.close();

    qtav::VolumeAudioFrameProcessor planar;
    require(!planar.open({
        48'000,
        2,
        qtav::SampleFormat::FloatPlanar,
        "stereo",
    }).success);
    qtav::VolumeAudioFrameProcessor invalid(-1.0);
    require(!invalid.open(format).success);
    return 0;
}
