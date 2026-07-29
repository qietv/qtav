// SPDX-License-Identifier: LGPL-2.1-or-later

#include <qtav/coreaudio_audio_sink.h>

#include <algorithm>
#include <cassert>
#include <chrono>
#include <cstdint>
#include <iostream>
#include <thread>
#include <vector>

int main()
{
    qtav::CoreAudioAudioSink sink;
    const auto capabilities = sink.capabilities();
    assert(capabilities.supportsPause);
    assert(capabilities.hasDeviceClock);
    assert(capabilities.maximumChannels >= 1);
    assert(
        std::find(
            capabilities.sampleFormats.begin(),
            capabilities.sampleFormats.end(),
            qtav::SampleFormat::Float)
        != capabilities.sampleFormats.end());

    const qtav::AudioFormat decoded {
        48'000,
        2,
        qtav::SampleFormat::S16,
        "stereo",
    };
    const auto defaultDevice =
        qtav::CoreAudioAudioSink::defaultOutputDevice();
    if (!defaultDevice) {
        const auto unavailable = sink.open(decoded);
        assert(!unavailable.success);
        assert(!unavailable.error.empty());
        std::cout
            << "CoreAudio device integration skipped: no output device\n";
        return 0;
    }

    const auto opened = sink.open(decoded);
    if (!opened.success) {
        // Virtualized and headless macOS runners can expose a default HAL
        // object while refusing creation of an output AudioQueue.
        assert(!opened.error.empty());
        std::cout << "CoreAudio device integration skipped: "
                  << opened.error << '\n';
        return 0;
    }

    assert(opened.deviceFormat.isValid());
    assert(
        opened.deviceFormat.sampleFormat
        == qtav::SampleFormat::Float);
    assert(opened.deviceFormat.channels >= 1);
    assert(opened.deviceFormat.channels <= 2);
    assert(sink.device());
    assert(
        sink.deviceFormat().sampleRate
        == opened.deviceFormat.sampleRate);

    const int frames = std::max(
        1,
        opened.deviceFormat.sampleRate / 20);
    std::vector<float> silence(
        static_cast<std::size_t>(frames)
            * static_cast<std::size_t>(
                opened.deviceFormat.channels),
        0.0F);
    const qtav::AudioBufferView buffer {
        opened.deviceFormat,
        frames,
        {
            reinterpret_cast<const std::uint8_t*>(
                silence.data()),
        },
        {
            static_cast<int>(
                silence.size() * sizeof(float)),
        },
        250,
        50,
    };

    sink.pause(true);
    assert(!sink.write(buffer));
    sink.pause(false);
    assert(sink.write(buffer));

    qtav::AudioSinkClock clock;
    for (int attempt = 0; attempt < 50; ++attempt) {
        clock = sink.clock();
        if (clock.valid) {
            break;
        }
        std::this_thread::sleep_for(
            std::chrono::milliseconds(2));
    }
    assert(clock.latencyMilliseconds >= 0);
    if (clock.valid) {
        assert(clock.positionMilliseconds >= 250);
        assert(clock.positionMilliseconds <= 300);
    }

    assert(sink.drain());
    sink.flush();
    sink.close();
    assert(!sink.deviceFormat().isValid());
    return 0;
}
