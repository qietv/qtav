// SPDX-License-Identifier: LGPL-2.1-or-later

#include "simulated_audio_sink.h"

#include <cassert>
#include <cstdint>
#include <vector>

namespace {

qtav::AudioBufferView makeBuffer(
    const qtav::AudioFormat& format,
    std::vector<std::uint8_t>& storage,
    std::int64_t timestamp,
    std::int64_t duration)
{
    const auto samples =
        static_cast<int>(duration * format.sampleRate / 1'000);
    storage.resize(
        static_cast<std::size_t>(samples * format.channels * 2));
    return {
        format,
        samples,
        { storage.data() },
        { static_cast<int>(storage.size()) },
        timestamp,
        duration,
    };
}

} // namespace

int main()
{
    const qtav::AudioFormat decoded {
        8'000,
        1,
        qtav::SampleFormat::S16,
        "mono",
    };
    const qtav::AudioFormat negotiated {
        16'000,
        2,
        qtav::SampleFormat::S16,
        "stereo",
    };
    qtav::test::SimulatedAudioSink sink({
        negotiated,
        100,
        20,
        250,
        0,
        true,
        true,
        true,
    });

    int underruns = 0;
    sink.setEventCallback([&](const qtav::AudioSinkEvent& event) {
        assert(event.type == qtav::AudioSinkEventType::Underrun);
        ++underruns;
    });
    const auto result = sink.open(decoded);
    assert(result.success);
    assert(result.deviceFormat.sampleRate == 16'000);
    assert(result.deviceFormat.channels == 2);

    std::vector<std::uint8_t> firstStorage;
    std::vector<std::uint8_t> secondStorage;
    std::vector<std::uint8_t> overflowStorage;
    auto first = makeBuffer(negotiated, firstStorage, 250, 60);
    auto second = makeBuffer(negotiated, secondStorage, 310, 40);
    auto overflow = makeBuffer(negotiated, overflowStorage, 350, 10);
    assert(sink.write(first));
    assert(sink.write(second));
    assert(!sink.write(overflow));

    auto state = sink.snapshot();
    assert(state.queuedMilliseconds == 100);
    assert(state.reportedLatencyMilliseconds == 120);
    assert(state.rejectedWriteCount == 1);

    sink.advance(20);
    assert(sink.clock().positionMilliseconds == 250);
    sink.advance(30);
    assert(sink.clock().positionMilliseconds == 280);
    assert(sink.snapshot().queuedMilliseconds == 70);

    sink.pause(true);
    sink.advance(50);
    assert(sink.clock().positionMilliseconds == 280);
    sink.pause(false);
    sink.advance(30);
    assert(sink.clock().positionMilliseconds == 310);

    sink.flush();
    state = sink.snapshot();
    assert(state.queuedMilliseconds == 0);
    assert(state.flushCount == 1);
    auto afterSeek = makeBuffer(negotiated, firstStorage, 500, 50);
    assert(sink.write(afterSeek));
    sink.advance(30);
    assert(sink.clock().positionMilliseconds == 510);
    sink.advance(100);
    assert(sink.clock().positionMilliseconds == 550);
    assert(underruns == 1);
    sink.advance(100);
    assert(underruns == 1);

    auto tail = makeBuffer(negotiated, secondStorage, 550, 40);
    assert(sink.write(tail));
    assert(sink.drain());
    state = sink.snapshot();
    assert(state.clockPositionMilliseconds == 590);
    assert(state.queuedMilliseconds == 0);
    assert(state.drainCount == 1);

    sink.setClockValid(false);
    assert(!sink.clock().valid);
    sink.close();
    assert(!sink.snapshot().open);
    return 0;
}
