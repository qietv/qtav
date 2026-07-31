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

class QTAV_CORE_EXPORT Player {
public:
    using StateCallback = std::function<void(State)>;
    using StatusCallback = std::function<bool(MediaStatus, MediaStatus)>;
    using EventCallback = std::function<bool(const MediaEvent&)>;
    using PrepareCallback = std::function<void(std::int64_t, bool*)>;
    using SeekCallback = std::function<void(std::int64_t)>;
    using VideoFrameCallback = std::function<void(const VideoFrame&, int)>;
    using AudioFrameCallback = std::function<void(const AudioFrame&, int)>;
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
    Player& setRenderCallback(RenderCallback callback);
    Player& setVideoRenderer(VideoRenderer renderer);
    Player& setVideoRenderAPI(
        std::shared_ptr<VideoRenderAPI> renderer,
        void* opaque = nullptr);
    // Returns the rendered frame timestamp in seconds. A negative value means
    // there is no current frame or this retryable render attempt was declined
    // because player/backend state is temporarily busy.
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
