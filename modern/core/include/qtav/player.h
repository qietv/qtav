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
#include <qtav/video_render_api.h>

namespace qtav {

class AudioSink;
class AudioFrameConverter;
class AudioTimeStretcher;
class VideoRenderAPI;

// Playback counters accumulated since the current media was set. Video fields
// distinguish decoded frames from frames discarded inside Player before
// onVideoFrame() and rendering callbacks are reached. Network fields count
// Player-level reopen attempts after protocol-level recovery has returned an
// error to the demuxer.
struct QTAV_CORE_EXPORT PlaybackStatistics {
    std::uint64_t decodedVideoFrames = 0;
    std::uint64_t videoQueueOverflowDrops = 0;
    std::uint64_t lateVideoDrops = 0;
    std::uint64_t deliveredVideoFrames = 0;
    std::uint64_t maximumQueuedVideoFrames = 0;
    std::uint64_t videoPresentationStarvations = 0;
    std::uint64_t maximumVideoPresentationStarvationMilliseconds = 0;
    // Subset of videoQueueOverflowDrops made while LivePlaybackPolicy keeps
    // the newest bounded presentation window instead of the oldest one.
    std::uint64_t lowLatencyVideoQueueDrops = 0;
    std::uint64_t networkRecoveryAttempts = 0;
    std::uint64_t successfulNetworkRecoveries = 0;
    std::uint64_t failedNetworkRecoveries = 0;
};

// Opt-in decoded-video policy for live and other latency-sensitive playback.
// It never drops compressed packets, audio, or subtitles. When enabled, the
// presentation queue keeps the newest bounded video window and skips a late
// frame whenever a newer frame is already available. Values are normalized by
// Player: the queue depth is clamped to [1, 24], and a negative late threshold
// becomes zero. The application owns all scheduling when VideoFrameScheduler
// accepts a frame, so this policy does not apply to accepted scheduler frames.
struct QTAV_CORE_EXPORT LivePlaybackPolicy {
    bool enabled = false;
    std::uint32_t maximumQueuedVideoFrames = 2;
    std::int64_t lateVideoFrameThresholdMilliseconds = 80;
};

// Player-level recovery applied only to recognized network URLs after FFmpeg's
// protocol-specific recovery has returned an error. maximumAttempts bounds
// fresh opens until selected demux timestamps advance by 500 ms; seek, track
// change, or media replacement starts a new continuity interval. Values are
// normalized by Player: attempts are clamped to [1, 32], negative delays
// become zero, and maximumRetryDelayMilliseconds is raised to the initial
// delay.
struct QTAV_CORE_EXPORT NetworkRecoveryPolicy {
    bool enabled = true;
    std::uint32_t maximumAttempts = 3;
    std::int64_t initialRetryDelayMilliseconds = 250;
    std::int64_t maximumRetryDelayMilliseconds = 2'000;
};

enum class NetworkRecoveryState {
    Idle,
    Waiting,
    Reopening,
    Recovered,
    Failed,
};

enum class NetworkRecoveryOperation {
    Open,
    Read,
};

enum class NetworkRecoveryInput {
    Primary,
    ExternalAudio,
    ExternalSubtitle,
};

// Snapshot for one network input. attempt is one-based while Waiting or
// Reopening. Recovered reports the successful attempt; Failed reports the
// number of attempts consumed. retryDelayMilliseconds is nonzero only while
// Waiting. Stop, seek, pause, media replacement, and track changes interrupt a
// pending read-recovery wait through the normal asynchronous control path.
struct QTAV_CORE_EXPORT NetworkRecoveryStatus {
    NetworkRecoveryState state = NetworkRecoveryState::Idle;
    NetworkRecoveryOperation operation = NetworkRecoveryOperation::Open;
    NetworkRecoveryInput input = NetworkRecoveryInput::Primary;
    std::string url;
    std::uint32_t attempt = 0;
    std::uint32_t maximumAttempts = 0;
    std::int64_t retryDelayMilliseconds = 0;
    int error = 0;
    std::int64_t resumePosition = 0;
    std::uint64_t presentationGeneration = 0;
};

// Optional spill storage for compressed packets that no longer fit in the
// in-memory reservoir. Files are created below the system temporary directory
// and are removed as soon as the disk-backed queue becomes empty. The disk
// cache is disabled by default, so these limits allocate no storage unless the
// application opts in.
struct QTAV_CORE_EXPORT PacketDiskCachePolicy {
    bool enabled = false;
    std::int64_t maximumCacheMilliseconds = 60'000;
    std::uint64_t maximumCacheBytes = 256U * 1024U * 1024U;
};

// Time-based packet buffering remains separate from decoder/output queues and
// from LivePlaybackPolicy. maximumBufferMilliseconds and
// maximumBufferBytes are the public in-memory limits. Values are normalized by
// Player: negative durations become zero, a zero byte limit uses the default,
// and the enabled memory-plus-disk duration is raised to the larger fill target.
struct QTAV_CORE_EXPORT PacketBufferPolicy {
    bool enabled = true;
    std::int64_t initialBufferMilliseconds = 500;
    std::int64_t rebufferMilliseconds = 750;
    std::int64_t maximumBufferMilliseconds = 5'000;
    std::uint64_t maximumBufferBytes = 32U * 1024U * 1024U;
    std::int64_t underflowDetectionMilliseconds = 120;
    PacketDiskCachePolicy diskCache;
};

enum class PacketBufferingReason {
    None,
    InitialPlayback,
    Seek,
    TrackSwitch,
    Underflow,
    NetworkRecovery,
};

// Snapshot reported while compressed audio/video packets are filling. The
// buffered duration is the minimum usable duration across active audio/video
// streams; bytes is their combined compressed size. A completed snapshot has
// buffering=false and progress=1.0. capacityLimited means playback resumed at
// a hard packet/time/byte bound before the requested duration was available.
struct QTAV_CORE_EXPORT PacketBufferStatus {
    bool buffering = false;
    PacketBufferingReason reason = PacketBufferingReason::None;
    std::int64_t bufferedMilliseconds = 0;
    std::int64_t targetMilliseconds = 0;
    std::uint64_t bufferedBytes = 0;
    std::uint64_t memoryBufferedBytes = 0;
    std::uint64_t diskBufferedBytes = 0;
    std::string diskCachePath;
    double progress = 1.0;
    std::uint64_t presentationGeneration = 0;
    bool capacityLimited = false;
};

enum class VideoRenderStatus {
    Rendered,
    // No current frame exists, or its presentation generation was invalidated
    // before the backend completed. Wait for the next render callback.
    NoFrame,
    // Reserved for transient Player-side render-state contention.
    PlayerStateBusy,
    // The selected VideoRenderAPI declined this attempt. Backends may expose
    // a retry delay; retry this same frame after bounded timer backoff.
    RendererBusy,
    // Retain this exact frame until the backend emits RedrawRequested.
    RendererDeferred,
    // The frame is terminally stale, superseded, or intentionally consumed.
    FrameDiscarded,
    // The native presentation surface generation must be recreated.
    SurfaceLost,
    // The active renderer cannot continue without replacement or fallback.
    RendererError,
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
    std::uint32_t retryAfterMilliseconds = 0;
    std::string detail;
    VideoRenderRetryReason retryReason =
        VideoRenderRetryReason::Unspecified;
};

class QTAV_CORE_EXPORT Player {
public:
    using StateCallback = std::function<void(State)>;
    using StatusCallback = std::function<bool(MediaStatus, MediaStatus)>;
    using PacketBufferStatusCallback =
        std::function<void(const PacketBufferStatus&)>;
    using NetworkRecoveryStatusCallback =
        std::function<void(const NetworkRecoveryStatus&)>;
    using EventCallback = std::function<bool(const MediaEvent&)>;
    using PrepareCallback = std::function<void(std::int64_t, bool*)>;
    using SeekCallback = std::function<void(std::int64_t)>;
    using VideoFrameCallback = std::function<void(const VideoFrame&, int)>;
    using AudioFrameCallback = std::function<void(const AudioFrame&, int)>;
    using SubtitleFrameCallback =
        std::function<void(const SubtitleFrame&, int)>;
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

    // Configures one external audio or subtitle input. An empty URL removes
    // that input. The configuration persists across setMedia() calls. If
    // media is loaded, changing it asynchronously reopens the current main
    // input at the current position while preserving play/pause intent.
    // Returns false for media types other than Audio and Subtitle.
    bool setExternalMedia(MediaType type, std::string url);
    std::string externalMedia(MediaType type) const;

    void prepare(
        std::int64_t startPosition = 0,
        PrepareCallback callback = {},
        SeekFlag flags = SeekFlag::FromStart);
    bool seek(
        std::int64_t position,
        SeekFlag flags = SeekFlag::FromStart,
        SeekCallback callback = {});
    // Publishes exactly one adjacent video frame and leaves playback paused.
    // The callback receives that frame's timestamp, or -1 when no adjacent
    // frame exists. Stepping requires loaded, seekable media with an active
    // video track.
    bool stepForward(SeekCallback callback = {});
    bool stepBackward(SeekCallback callback = {});

    void setState(State state);
    State state() const;
    bool waitFor(State state, long timeoutMs = -1);

    MediaStatus mediaStatus() const;
    MediaInfo mediaInfo() const;
    // Asynchronously selects one loaded audio, video, or subtitle track by
    // TrackInfo::index. Pass -1 to disable that media type. The request is
    // rejected when media is not loaded, the type is unsupported, or the
    // index does not identify a track of the requested type.
    bool setActiveTrack(MediaType type, int track);
    std::int64_t position() const;
    PlaybackStatistics playbackStatistics() const noexcept;
    Player& setLivePlaybackPolicy(LivePlaybackPolicy policy);
    LivePlaybackPolicy livePlaybackPolicy() const;
    Player& setNetworkRecoveryPolicy(NetworkRecoveryPolicy policy);
    NetworkRecoveryPolicy networkRecoveryPolicy() const;
    NetworkRecoveryStatus networkRecoveryStatus() const;
    Player& setPacketBufferPolicy(PacketBufferPolicy policy);
    PacketBufferPolicy packetBufferPolicy() const;
    PacketBufferStatus packetBufferStatus() const;
    std::string packetDiskCachePath() const;
    // Synchronously removes the temporary cache. If disk-backed packets are
    // active, all compressed prefetch is discarded and seekable playback is
    // restarted from the current position so cleared packets are not skipped.
    // Returns false without clearing active cache for a non-seekable input.
    // Do not call this blocking operation from a Player callback.
    bool clearPacketDiskCache();

    Player& onStateChanged(StateCallback callback);
    Player& onMediaStatus(StatusCallback callback);
    Player& onPacketBufferStatus(PacketBufferStatusCallback callback);
    Player& onNetworkRecoveryStatus(NetworkRecoveryStatusCallback callback);
    Player& onEvent(EventCallback callback);
    Player& onVideoFrame(VideoFrameCallback callback);
    Player& onAudioFrame(AudioFrameCallback callback);
    Player& onSubtitleFrame(SubtitleFrameCallback callback);

    Player& setAudioSink(std::shared_ptr<AudioSink> sink);
    Player& setAudioFrameConverter(
        std::shared_ptr<AudioFrameConverter> converter);
    // Installs the optional pitch-preserving stage used for device output at
    // playback rates other than 1.0. Decoded onAudioFrame() callbacks retain
    // the original PCM and are not processed by this stage.
    Player& setAudioTimeStretcher(
        std::shared_ptr<AudioTimeStretcher> stretcher);
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
