// SPDX-License-Identifier: LGPL-2.1-or-later

#include <qtav/player.h>
#include <qtav/swresample_audio_converter.h>
#include <qtav/wav_audio_sink.h>

#include <atomic>
#include <cassert>
#include <chrono>
#include <condition_variable>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <fstream>
#include <iterator>
#include <memory>
#include <mutex>
#include <string>
#include <vector>

namespace {

std::uint16_t read16(
    const std::vector<std::uint8_t>& data,
    std::size_t offset)
{
    return static_cast<std::uint16_t>(data[offset])
        | static_cast<std::uint16_t>(data[offset + 1] << 8U);
}

std::uint32_t read32(
    const std::vector<std::uint8_t>& data,
    std::size_t offset)
{
    std::uint32_t result = 0;
    for (std::size_t byte = 0; byte < 4; ++byte) {
        result |= static_cast<std::uint32_t>(data[offset + byte])
            << static_cast<unsigned>(byte * 8U);
    }
    return result;
}

std::vector<std::uint8_t> readFile(const std::string& path)
{
    std::ifstream input(path, std::ios::binary);
    assert(input);
    return {
        std::istreambuf_iterator<char>(input),
        std::istreambuf_iterator<char>(),
    };
}

} // namespace

int main(int argc, char** argv)
{
    assert(argc == 3);
    const std::string media = argv[1];
    const std::string output = argv[2];
    std::remove(output.c_str());

    auto sink = std::make_shared<qtav::WavAudioSink>(
        qtav::WavAudioSinkConfig {
            output,
            16'000,
            2,
            qtav::SampleFormat::S16,
            "stereo",
        });
    auto converter =
        std::make_shared<qtav::SwresampleAudioConverter>();
    std::mutex mutex;
    std::condition_variable changed;
    bool started = false;
    bool stoppedAfterStart = false;
    std::vector<std::string> audioErrors;
    std::atomic<int> decodedFrames { 0 };

    qtav::Player player;
    player
        .onStateChanged([&](qtav::State state) {
            {
                std::lock_guard<std::mutex> lock(mutex);
                if (state == qtav::State::Playing) {
                    started = true;
                } else if (state == qtav::State::Stopped && started) {
                    stoppedAfterStart = true;
                }
            }
            changed.notify_all();
        })
        .onAudioFrame([&](const qtav::AudioFrame&, int) {
            ++decodedFrames;
        })
        .onEvent([&](const qtav::MediaEvent& event) {
            if (event.category.find("audio.") == 0) {
                std::lock_guard<std::mutex> lock(mutex);
                audioErrors.push_back(event.category);
            }
            return false;
        })
        .setAudioFrameConverter(converter)
        .setAudioSink(sink);
    player.setPlaybackRate(20.0F);
    player.setMedia(media);
    player.setState(qtav::State::Playing);

    {
        std::unique_lock<std::mutex> lock(mutex);
        assert(changed.wait_for(
            lock,
            std::chrono::seconds(5),
            [&] { return stoppedAfterStart; }));
        assert(audioErrors.empty());
    }
    assert(decodedFrames.load() > 0);
    assert(sink->bytesWritten() == 64'000);

    const auto file = readFile(output);
    assert(file.size() == 64'044);
    assert(std::memcmp(file.data(), "RIFF", 4) == 0);
    assert(read32(file, 4) == 64'036);
    assert(read16(file, 20) == 1);
    assert(read16(file, 22) == 2);
    assert(read32(file, 24) == 16'000);
    assert(read32(file, 28) == 64'000);
    assert(read16(file, 32) == 4);
    assert(read16(file, 34) == 16);
    assert(read32(file, 40) == 64'000);

    bool hasNonZeroSample = false;
    for (std::size_t index = 44; index < file.size(); ++index) {
        hasNonZeroSample = hasNonZeroSample || file[index] != 0;
    }
    assert(hasNonZeroSample);

    std::remove(output.c_str());
    return 0;
}
