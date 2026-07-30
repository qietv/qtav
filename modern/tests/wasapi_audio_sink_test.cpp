// SPDX-License-Identifier: LGPL-2.1-or-later

#include <qtav/wasapi_audio_sink.h>

#include <algorithm>
#include <cassert>
#include <chrono>
#include <cstdint>
#include <iostream>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

int main()
{
    qtav::WasapiAudioSink sink;
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

    const auto invalid = sink.open({});
    assert(!invalid.success);
    assert(!invalid.error.empty());

    std::mutex eventMutex;
    std::vector<qtav::AudioSinkEvent> events;
    sink.setEventCallback(
        [&](const qtav::AudioSinkEvent& event) {
            std::lock_guard<std::mutex> lock(eventMutex);
            events.push_back(event);
        });

    const qtav::AudioFormat decoded {
        48'000,
        2,
        qtav::SampleFormat::S16,
        "stereo",
    };
    const auto opened = sink.open(decoded);
    if (!opened.success) {
        // Headless Windows runners can have no active render endpoint.
        assert(!opened.error.empty());
        std::cout << "WASAPI device integration skipped: "
                  << opened.error << '\n';
        return 77;
    }

    assert(opened.deviceFormat.isValid());
    assert(
        opened.deviceFormat.sampleFormat
        == qtav::SampleFormat::Float);
    assert(opened.deviceFormat.channels >= 1);
    assert(opened.deviceFormat.channels <= 2);
    assert(sink.endpoint());
    assert(
        sink.deviceFormat().sampleRate
        == opened.deviceFormat.sampleRate);

    const int frames = std::max(
        1,
        opened.deviceFormat.sampleRate / 10);
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
        100,
    };

    sink.pause(true);
    assert(!sink.write(buffer));
    sink.pause(false);
    assert(sink.write(buffer));

    qtav::AudioSinkClock clock;
    for (int attempt = 0; attempt < 100; ++attempt) {
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
        assert(clock.positionMilliseconds <= 350);
    }

    sink.pause(true);
    assert(!sink.write(buffer));
    sink.pause(false);
    assert(sink.drain());
    sink.flush();
    assert(!sink.clock().valid);
    assert(sink.write(buffer));
    assert(sink.drain());
    sink.close();
    assert(!sink.deviceFormat().isValid());

    std::lock_guard<std::mutex> lock(eventMutex);
    assert(std::none_of(
        events.begin(),
        events.end(),
        [](const qtav::AudioSinkEvent& event) {
            return event.type == qtav::AudioSinkEventType::Error
                || event.type
                    == qtav::AudioSinkEventType::DeviceLost;
        }));
    return 0;
}
