// SPDX-License-Identifier: LGPL-2.1-or-later

#if defined(NDEBUG)
#  undef NDEBUG
#endif

#include "../backends/audio/aaudio/src/aaudio_pcm_queue.h"

#include <algorithm>
#include <atomic>
#include <cassert>
#include <cmath>
#include <cstdint>
#include <vector>
#include <thread>

namespace {

bool near(float left, float right)
{
    return std::abs(left - right) < 0.0001F;
}

} // namespace

int main()
{
    qtav::detail::AAudioPcmQueue queue;
    assert(!queue.configure(0, 2, 48'000));
    assert(queue.configure(5, 2, 1'000));
    assert(queue.capacityFrames() == 5);
    assert(queue.queuedFrames() == 0);

    const std::vector<float> first {
        1.0F, 1.5F,
        2.0F, 2.5F,
        3.0F, 3.5F,
    };
    assert(queue.push(first.data(), 3, 250));
    assert(queue.queuedFrames() == 3);

    std::vector<float> output(10, -1.0F);
    auto popped = queue.pop(output.data(), 2);
    assert(popped.frames == 2);
    assert(popped.firstTimestampNanoseconds == 250'000'000LL);
    assert(near(output[0], 1.0F));
    assert(near(output[1], 1.5F));
    assert(near(output[2], 2.0F));
    assert(near(output[3], 2.5F));

    const std::vector<float> wrapped {
        4.0F, 4.5F,
        5.0F, 5.5F,
        6.0F, 6.5F,
        7.0F, 7.5F,
    };
    assert(queue.push(wrapped.data(), 4, 400));
    assert(queue.queuedFrames() == 5);
    assert(!queue.push(wrapped.data(), 1, 500));

    std::fill(output.begin(), output.end(), -1.0F);
    popped = queue.pop(output.data(), 5);
    assert(popped.frames == 5);
    assert(popped.firstTimestampNanoseconds == 252'000'000LL);
    const std::vector<float> expected {
        3.0F, 3.5F,
        4.0F, 4.5F,
        5.0F, 5.5F,
        6.0F, 6.5F,
        7.0F, 7.5F,
    };
    for (std::size_t index = 0; index < expected.size(); ++index) {
        assert(near(output[index], expected[index]));
    }
    assert(queue.queuedFrames() == 0);
    assert(queue.pop(output.data(), 5).frames == 0);

    assert(queue.push(first.data(), 3, 800));
    queue.clear();
    assert(queue.queuedFrames() == 0);

    qtav::detail::AAudioPcmQueue concurrent;
    assert(concurrent.configure(64, 1, 1'000));
    constexpr int totalFrames = 10'000;
    std::atomic<bool> producerFinished { false };
    std::thread producer([&] {
        int produced = 0;
        std::vector<float> input(7);
        while (produced < totalFrames) {
            const int count =
                std::min<int>(input.size(), totalFrames - produced);
            for (int index = 0; index < count; ++index) {
                input[static_cast<std::size_t>(index)] =
                    static_cast<float>(produced + index);
            }
            if (concurrent.push(input.data(), count, produced)) {
                produced += count;
            } else {
                std::this_thread::yield();
            }
        }
        producerFinished = true;
    });
    int consumed = 0;
    std::vector<float> received(5);
    while (consumed < totalFrames) {
        const auto result =
            concurrent.pop(received.data(), received.size());
        if (result.frames == 0) {
            assert(!producerFinished.load() || consumed == totalFrames);
            std::this_thread::yield();
            continue;
        }
        assert(
            result.firstTimestampNanoseconds
            == static_cast<std::int64_t>(consumed) * 1'000'000LL);
        for (int index = 0; index < result.frames; ++index) {
            assert(near(
                received[static_cast<std::size_t>(index)],
                static_cast<float>(consumed + index)));
        }
        consumed += result.frames;
    }
    producer.join();
    assert(concurrent.queuedFrames() == 0);
    return 0;
}
