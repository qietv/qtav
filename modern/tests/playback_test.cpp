// SPDX-License-Identifier: LGPL-2.1-or-later

#include <qtav/player.h>
#include <qtav/video_render_api.h>

#include <atomic>
#include <cassert>
#include <chrono>
#include <condition_variable>
#include <cstdint>
#include <mutex>
#include <memory>
#include <utility>

namespace {

class CountingRenderer final : public qtav::VideoRenderAPI {
public:
    explicit CountingRenderer(std::atomic<int>& count)
        : count_(count)
    {
    }

    qtav::VideoRenderCapabilities capabilities() const override
    {
        return {};
    }
    void setEventCallback(EventCallback callback) override
    {
        callback_ = std::move(callback);
    }
    bool open(const qtav::VideoRenderConfig&) override { return true; }
    bool configure(const qtav::VideoRenderConfig&) override { return true; }
    bool render(const qtav::VideoFrame& frame) override
    {
        assert(frame);
        ++count_;
        return true;
    }
    void close() noexcept override {}

private:
    std::atomic<int>& count_;
    EventCallback callback_;
};

} // namespace

int main(int argc, char** argv)
{
    assert(argc == 2);

    qtav::Player player;
    std::mutex mutex;
    std::condition_variable finished;
    bool reachedEnd = false;
    bool failed = false;
    std::atomic<int> videoFrames { 0 };
    std::atomic<int> audioFrames { 0 };
    std::atomic<int> renderedFrames { 0 };
    int firstRenderKey = 1;
    int secondRenderKey = 2;
    auto firstRenderer = std::make_shared<CountingRenderer>(renderedFrames);
    auto secondRenderer = std::make_shared<CountingRenderer>(renderedFrames);

    player
        .onMediaStatus([&](qtav::MediaStatus, qtav::MediaStatus status) {
            if (status == qtav::MediaStatus::EndOfMedia
                || status == qtav::MediaStatus::Invalid) {
                {
                    std::lock_guard<std::mutex> lock(mutex);
                    reachedEnd = status == qtav::MediaStatus::EndOfMedia;
                    failed = status == qtav::MediaStatus::Invalid;
                }
                finished.notify_all();
            }
            return false;
        })
        .onVideoFrame([&](const qtav::VideoFrame& frame, int) {
            assert(frame);
            assert(frame.width() == 160);
            assert(frame.height() == 90);
            ++videoFrames;
        })
        .onAudioFrame([&](const qtav::AudioFrame& frame, int) {
            assert(frame);
            assert(frame.sampleRate() == 48'000);
            ++audioFrames;
        })
        .setVideoRenderer([&](const qtav::VideoFrame& frame, void*) {
            assert(frame);
            ++renderedFrames;
        })
        .setVideoRenderAPI(firstRenderer, &firstRenderKey)
        .setVideoRenderAPI(secondRenderer, &secondRenderKey)
        .setRenderCallback([&](void* opaque) { player.renderVideo(opaque); });

    player.setPlaybackRate(4.0F);
    player.setMedia(argv[1]);
    player.setState(qtav::State::Playing);

    std::unique_lock<std::mutex> lock(mutex);
    const bool completed = finished.wait_for(
        lock,
        std::chrono::seconds(10),
        [&] { return reachedEnd || failed; });
    lock.unlock();

    assert(completed);
    assert(!failed);
    assert(reachedEnd);
    assert(videoFrames.load() >= 10);
    assert(audioFrames.load() > 0);
    assert(renderedFrames.load() == videoFrames.load() * 3);
    assert(player.state() == qtav::State::Stopped);
    return 0;
}
