// SPDX-License-Identifier: LGPL-2.1-or-later
#pragma once

#include <cstdint>
#include <functional>
#include <memory>
#include <string>

#include <qtav/export.h>
#include <qtav/frame.h>
#include <qtav/hardware_decoder.h>
#include <qtav/media.h>

namespace qtav {

class AudioSink;
class AudioFrameConverter;
class VideoRenderAPI;

// Video scheduling counters accumulated since the current media was set.
// They distinguish decoded frames from frames discarded inside Player before
// onVideoFrame() and rendering callbacks are reached.
struct QTAV_CORE_EXPORT PlaybackStatistics {
    std::uint64_t decodedVideoFrames = 0;
    std::uint64_t videoQueueOverflowDrops = 0;
    std::uint64_t lateVideoDrops = 0;
    std::uint64_t deliveredVideoFrames = 0;
    std::uint64_t maximumQueuedVideoFrames = 0;
    std::uint64_t videoPresentationStarvations = 0;
    std::uint64_t maximumVideoPresentationStarvationMilliseconds = 0;
};

enum class VideoRenderStatus {
    Rendered,
    // No current frame exists, or its presentation generation was invalidated
    // before the backend completed. Wait for the next render callback.
    NoFrame,
    // Reserved for transient Player-side render-state contention.
    PlayerStateBusy,
    // The selected VideoRenderAPI declined this attempt. Backends may expose
    // more detailed diagnostics to decide whether it is retryable.
    RendererBusy,
};

// Detailed result for one render-thread attempt. A frame sequence is
// monotonically assigned when Player publishes a new current frame. Sequence
// zero means that no current frame was available. presentationGeneration
// changes whenever seek, stop, or media replacement invalidates queued video.
struct QTAV_CORE_EXPORT VideoRenderResult {
    VideoRenderStatus status = VideoRenderStatus::NoFrame;
    double timestamp = -1.0;
    std::uint64_t frameSequence = 0;
    std::uint64_t presentationGeneration = 0;
};

class QTAV_CORE_EXPORT Player {
public:
    using StateCallback = std::function<void(State)>;
    using StatusCallback = std::function<bool(MediaStatus, MediaStatus)>;
    using EventCallback = std::function<bool(const MediaEvent&)>;
    using PrepareCallback = std::function<void(std::int64_t, bool*)>;
    using SeekCallback = std::function<void(std::int64_t)>;
    using VideoFrameCallback = std::function<void(const VideoFrame&, int)>;
    using AudioFrameCallback = std::function<void(const AudioFrame&, int)>;
    // Runs on the video-decode worker as soon as a decoded video frame is within
    // the bounded decode window. Returning true transfers presentation
    // scheduling to the callback and suppresses the later
    // onVideoFrame()/renderer delivery. monotonicNanoseconds is the target
    // presentation time in the steady monotonic clock epoch.
    using VideoFrameScheduler = std::function<bool(
        const VideoFrame&,
        int,
        std::int64_t monotonicNanoseconds)>;
    using RenderCallback = std::function<void(void*)>;
    using VideoRenderer = std::function<void(const VideoFrame&, void*)>;

    Player();
    ~Player();

    Player(Player&&) noexcept;
    Player& operator=(Player&&) noexcept;
    Player(const Player&) = delete;
    Player& operator=(const Player&) = delete;

    void setMedia(std::string url);
    std::string url() const;

    void prepare(
        std::int64_t startPosition = 0,
        PrepareCallback callback = {},
        SeekFlag flags = SeekFlag::FromStart);
    bool seek(
        std::int64_t position,
        SeekFlag flags = SeekFlag::FromStart,
        SeekCallback callback = {});

    void setState(State state);
    State state() const;
    bool waitFor(State state, long timeoutMs = -1);

    MediaStatus mediaStatus() const;
    MediaInfo mediaInfo() const;
    std::int64_t position() const;
    PlaybackStatistics playbackStatistics() const noexcept;

    Player& onStateChanged(StateCallback callback);
    Player& onMediaStatus(StatusCallback callback);
    Player& onEvent(EventCallback callback);
    Player& onVideoFrame(VideoFrameCallback callback);
    Player& onAudioFrame(AudioFrameCallback callback);

    Player& setAudioSink(std::shared_ptr<AudioSink> sink);
    Player& setAudioFrameConverter(
        std::shared_ptr<AudioFrameConverter> converter);
    Player& setHardwareDecodeConfig(HardwareDecodeConfig config);
    HardwareDecodeConfig hardwareDecodeConfig() const;
    Player& setVideoFrameScheduler(VideoFrameScheduler scheduler);
    Player& setRenderCallback(RenderCallback callback);
    Player& setVideoRenderer(VideoRenderer renderer);
    Player& setVideoRenderAPI(
        std::shared_ptr<VideoRenderAPI> renderer,
        void* opaque = nullptr);
    // Synchronously renders the atomically published current-frame snapshot on
    // the caller's thread and preserves reason, sequence, and generation.
    VideoRenderResult renderVideoDetailed(void* opaque = nullptr);
    // Returns the rendered frame timestamp in seconds. A negative value means
    // renderVideoDetailed() did not return VideoRenderStatus::Rendered.
    double renderVideo(void* opaque = nullptr);

    void setPlaybackRate(float value);
    float playbackRate() const;
    void setLoop(int count);
    void setRange(std::int64_t start, std::int64_t end = MediaEnd);

    void setProperty(std::string key, std::string value);
    std::string property(
        const std::string& key,
        std::string defaultValue = {}) const;

private:
    class Impl;
    std::unique_ptr<Impl> impl_;
};

} // namespace qtav
