// SPDX-License-Identifier: LGPL-2.1-or-later

#include <qtav/player.h>
#include <qtav/audio_converter.h>
#include <qtav/audio_processor.h>
#include <qtav/audio_sink.h>
#include <qtav/audio_time_stretcher.h>
#include <qtav/video_processor.h>
#include <qtav/video_render_api.h>

#include <algorithm>
#include <atomic>
#include <cctype>
#include <chrono>
#include <cmath>
#include <condition_variable>
#include <cerrno>
#include <cstdlib>
#include <deque>
#include <filesystem>
#include <fstream>
#include <limits>
#include <memory>
#include <mutex>
#include <optional>
#include <stdexcept>
#include <thread>
#include <unordered_map>
#include <utility>
#include <vector>

#include "frame_internal.h"
#include "hardware_decode_device_internal.h"

extern "C" {
#include <libavcodec/avcodec.h>
#include <libavformat/avformat.h>
#include <libavutil/avutil.h>
#include <libavutil/dict.h>
#include <libavutil/error.h>
#include <libavutil/frame.h>
#include <libavutil/hwcontext.h>
#include <libavutil/pixdesc.h>
}

#if LIBAVCODEC_VERSION_MAJOR < 62
#  error "QtAVCore requires FFmpeg 8 or newer"
#endif

namespace qtav {
namespace {

using Clock = std::chrono::steady_clock;
using Milliseconds = std::chrono::milliseconds;

constexpr std::int64_t kMaximumQueuedAudioMilliseconds = 500;
constexpr std::int64_t kMaximumVideoDecodeLeadMilliseconds = 250;
constexpr std::int64_t kNetworkRecoveryProgressMilliseconds = 500;
// Encoded surface-decoder packets need enough lead for deep HEVC reordering,
// while a decoded Surface frame owns a finite decoder output buffer until the
// application presents or drops it. Keep those two windows independent: feed
// the decoder ahead, but never retain a deep queue of Surface output tokens.
// This follows legacy QtAV's packet-DTS pacing before decode without moving
// the wait behind hardware-frame acquisition.
constexpr std::int64_t kMaximumSurfacePacketDecodeLeadMilliseconds = 250;
constexpr std::int64_t kMaximumSurfaceOutputLeadMilliseconds = 80;
constexpr std::int64_t kMaximumVideoPrerollWaitMilliseconds = 500;
constexpr std::size_t kMinimumVideoPrerollFrames = 6;
constexpr std::size_t kMaximumQueuedSoftwareVideoFrames = 8;
constexpr std::size_t kMaximumQueuedHardwareVideoFrames = 24;
constexpr std::size_t kMaximumQueuedSubtitleFrames = 64;
constexpr std::size_t kMaximumQueuedAudioPackets = 128;
constexpr std::size_t kMaximumQueuedVideoPackets = 128;
constexpr std::size_t kMaximumQueuedDiskPacketMetadata = 16'384;
constexpr std::uint64_t kDefaultMaximumPacketBufferBytes =
    32U * 1024U * 1024U;
constexpr std::uint64_t kDefaultMaximumPacketDiskCacheBytes =
    256U * 1024U * 1024U;
constexpr std::int64_t kLateVideoFrameThresholdMilliseconds = 250;
constexpr std::int64_t kHttpReadTimeoutMicroseconds = 15'000'000;

std::string ffmpegError(int error)
{
    char text[AV_ERROR_MAX_STRING_SIZE] {};
    av_strerror(error, text, sizeof(text));
    return text;
}

std::string dictionaryValue(const AVDictionary* dictionary, const char* key)
{
    const auto* entry = av_dict_get(dictionary, key, nullptr, 0);
    return entry && entry->value ? entry->value : "";
}

MediaType mediaTypeFromFFmpeg(AVMediaType type)
{
    switch (type) {
    case AVMEDIA_TYPE_AUDIO:
        return MediaType::Audio;
    case AVMEDIA_TYPE_VIDEO:
        return MediaType::Video;
    case AVMEDIA_TYPE_SUBTITLE:
        return MediaType::Subtitle;
    default:
        return MediaType::Unknown;
    }
}

std::int64_t toMilliseconds(std::int64_t value, AVRational timeBase)
{
    if (value == AV_NOPTS_VALUE) {
        return 0;
    }
    return av_rescale_q(value, timeBase, AVRational { 1, 1000 });
}

bool sameAudioFormat(const AudioFormat& left, const AudioFormat& right)
{
    return left.sampleRate == right.sampleRate
        && left.channels == right.channels
        && left.sampleFormat == right.sampleFormat
        && left.channelLayout == right.channelLayout;
}

AVHWDeviceType ffmpegHardwareDeviceType(HardwareDeviceType type) noexcept
{
    switch (type) {
    case HardwareDeviceType::D3D11:
        return AV_HWDEVICE_TYPE_D3D11VA;
    case HardwareDeviceType::MediaCodec:
        return AV_HWDEVICE_TYPE_MEDIACODEC;
    case HardwareDeviceType::OHCodec:
        return AV_HWDEVICE_TYPE_OHCODEC;
    case HardwareDeviceType::Vulkan:
        return AV_HWDEVICE_TYPE_VULKAN;
    case HardwareDeviceType::Unknown:
    case HardwareDeviceType::OpenGL:
        return AV_HWDEVICE_TYPE_NONE;
    }
    return AV_HWDEVICE_TYPE_NONE;
}

bool isSurfaceOutputHardwareDevice(HardwareDeviceType type) noexcept
{
    return type == HardwareDeviceType::MediaCodec
        || type == HardwareDeviceType::OHCodec;
}

const AVCodec* decoderForWrapper(
    AVCodecID codecId,
    AVMediaType mediaType,
    const std::string& wrapper) noexcept
{
    if (wrapper.empty()) {
        return nullptr;
    }
    void* opaque = nullptr;
    while (const AVCodec* codec = av_codec_iterate(&opaque)) {
        if (!av_codec_is_decoder(codec)
            || codec->id != codecId
            || codec->type != mediaType
            || !codec->wrapper_name
            || wrapper != codec->wrapper_name) {
            continue;
        }
        return codec;
    }
    return nullptr;
}

bool isSoftwarePixelFormat(AVPixelFormat format) noexcept
{
    const auto* descriptor = av_pix_fmt_desc_get(format);
    return descriptor && (descriptor->flags & AV_PIX_FMT_FLAG_HWACCEL) == 0;
}

bool isHttpUrl(const std::string& url)
{
    return url.compare(0, 7, "http://") == 0
        || url.compare(0, 8, "https://") == 0;
}

std::string urlScheme(const std::string& url)
{
    const auto delimiter = url.find("://");
    if (delimiter == std::string::npos || delimiter == 0) {
        return {};
    }
    std::string scheme = url.substr(0, delimiter);
    std::transform(
        scheme.begin(),
        scheme.end(),
        scheme.begin(),
        [](unsigned char value) {
            return static_cast<char>(std::tolower(value));
        });
    return scheme;
}

bool isNetworkUrl(const std::string& url)
{
    const auto scheme = urlScheme(url);
    return scheme == "http" || scheme == "https" || scheme == "ftp"
        || scheme == "ftps" || scheme == "gopher" || scheme == "gophers"
        || scheme == "tcp" || scheme == "tls" || scheme == "udp"
        || scheme == "rtp" || scheme == "rtsp" || scheme == "rtmp"
        || scheme == "rtmps" || scheme == "rist" || scheme == "srt";
}

bool isRecoverableNetworkError(int error)
{
    if (error >= 0 || error == AVERROR_EOF || error == AVERROR_EXIT) {
        return false;
    }
    switch (error) {
    case AVERROR(ENOMEM):
    case AVERROR(EINVAL):
    case AVERROR(ENOSYS):
    case AVERROR(EACCES):
    case AVERROR(ENOENT):
    case AVERROR_HTTP_BAD_REQUEST:
    case AVERROR_HTTP_UNAUTHORIZED:
    case AVERROR_HTTP_FORBIDDEN:
    case AVERROR_HTTP_NOT_FOUND:
        return false;
    default:
        return true;
    }
}

std::string plainTextFromAss(const char* ass)
{
    if (!ass || !*ass) {
        return {};
    }

    std::string source(ass);
    std::size_t textOffset = 0;
    const bool dialogueLine = source.compare(0, 9, "Dialogue:") == 0;
    const int metadataFields = dialogueLine ? 9 : 8;
    for (int field = 0; field < metadataFields; ++field) {
        const auto comma = source.find(',', textOffset);
        if (comma == std::string::npos) {
            textOffset = 0;
            break;
        }
        textOffset = comma + 1;
    }

    std::string result;
    result.reserve(source.size() - textOffset);
    bool overrideBlock = false;
    for (std::size_t index = textOffset; index < source.size(); ++index) {
        const char value = source[index];
        if (value == '{') {
            overrideBlock = true;
            continue;
        }
        if (overrideBlock) {
            if (value == '}') {
                overrideBlock = false;
            }
            continue;
        }
        if (value == '\\' && index + 1 < source.size()) {
            const char escaped = source[index + 1];
            if (escaped == 'N' || escaped == 'n') {
                result.push_back('\n');
                ++index;
                continue;
            }
            if (escaped == 'h') {
                result.push_back(' ');
                ++index;
                continue;
            }
        }
        result.push_back(value);
    }

    const auto first = result.find_first_not_of("\r\n\t ");
    if (first == std::string::npos) {
        return {};
    }
    const auto last = result.find_last_not_of("\r\n\t ");
    return result.substr(first, last - first + 1);
}

void applyHttpRecoveryDefaults(
    const std::string& url,
    AVDictionary** options)
{
    if (!isHttpUrl(url)) {
        return;
    }
    av_dict_set_int(
        options,
        "rw_timeout",
        kHttpReadTimeoutMicroseconds,
        0);
    av_dict_set(options, "reconnect", "1", 0);
    av_dict_set(options, "reconnect_on_network_error", "1", 0);
    av_dict_set(options, "reconnect_delay_max", "2", 0);
    av_dict_set(options, "reconnect_max_retries", "5", 0);
    av_dict_set(options, "reconnect_delay_total_max", "10", 0);
}

class PacketDiskStore final
    : public std::enable_shared_from_this<PacketDiskStore> {
public:
    struct Entry {
        std::uint64_t id = 0;
        std::uint64_t generation = 0;
        std::uint64_t offset = 0;
        std::uint64_t size = 0;
    };

    struct StoreResult {
        std::shared_ptr<Entry> entry;
        bool capacityLimited = false;
        std::string error;
    };

    ~PacketDiskStore()
    {
        clear();
    }

    StoreResult store(
        const std::uint8_t* data,
        std::uint64_t size,
        std::uint64_t maximumBytes)
    {
        std::lock_guard<std::mutex> lock(mutex_);
        if (!data || size == 0) {
            return { {}, false, "The packet payload is empty" };
        }
        if (size > maximumBytes
            || activeBytes_ > maximumBytes - size) {
            return { {}, true, {} };
        }

        std::string error;
        if (!ensureOpenLocked(error)) {
            return { {}, false, std::move(error) };
        }
        if (appendOffset_ > maximumBytes - size
            && !compactLocked(error)) {
            return { {}, false, std::move(error) };
        }
        if (appendOffset_ > maximumBytes - size) {
            return { {}, true, {} };
        }

        file_.clear();
        file_.seekp(static_cast<std::streamoff>(appendOffset_));
        file_.write(
            reinterpret_cast<const char*>(data),
            static_cast<std::streamsize>(size));
        file_.flush();
        if (!file_) {
            file_.clear();
            return {
                {},
                false,
                "Could not write the packet-cache temporary file",
            };
        }

        const auto id = nextEntryId_++;
        const auto generation = generation_;
        const auto offset = appendOffset_;
        std::weak_ptr<PacketDiskStore> owner = shared_from_this();
        auto entry = std::shared_ptr<Entry>(
            new Entry { id, generation, offset, size },
            [owner](Entry* released) {
                if (auto store = owner.lock()) {
                    store->release(
                        released->generation,
                        released->size);
                }
                delete released;
            });
        entries_.push_back(entry);
        appendOffset_ += size;
        activeBytes_ += size;
        return { std::move(entry), false, {} };
    }

    bool load(
        const std::shared_ptr<Entry>& entry,
        std::vector<std::uint8_t>& payload,
        std::string& error)
    {
        if (!entry) {
            error = "The packet-cache entry is missing";
            return false;
        }
        std::lock_guard<std::mutex> lock(mutex_);
        if (entry->generation != generation_ || !file_.is_open()) {
            error = "The packet-cache entry was cleared before it was read";
            return false;
        }
        if (entry->size
            > static_cast<std::uint64_t>(
                std::numeric_limits<std::size_t>::max())) {
            error = "The packet-cache entry is too large for this process";
            return false;
        }

        payload.resize(static_cast<std::size_t>(entry->size));
        file_.clear();
        file_.seekg(static_cast<std::streamoff>(entry->offset));
        file_.read(
            reinterpret_cast<char*>(payload.data()),
            static_cast<std::streamsize>(entry->size));
        if (!file_) {
            file_.clear();
            payload.clear();
            error = "Could not read the packet-cache temporary file";
            return false;
        }
        return true;
    }

    std::uint64_t bytes() const
    {
        std::lock_guard<std::mutex> lock(mutex_);
        return activeBytes_;
    }

    bool empty() const
    {
        return bytes() == 0;
    }

    std::string path() const
    {
        std::lock_guard<std::mutex> lock(mutex_);
        return filePath_.u8string();
    }

    void clear()
    {
        std::lock_guard<std::mutex> lock(mutex_);
        clearFilesLocked();
    }

private:
    bool ensureOpenLocked(std::string& error)
    {
        if (file_.is_open()) {
            return true;
        }

        std::error_code filesystemError;
        const auto temporaryRoot =
            std::filesystem::temp_directory_path(filesystemError);
        if (filesystemError) {
            error = "Could not locate the system temporary directory: "
                + filesystemError.message();
            return false;
        }

        static std::atomic<std::uint64_t> nextDirectoryId { 1 };
        const auto timeId = static_cast<std::uint64_t>(
            Clock::now().time_since_epoch().count());
        for (int attempt = 0; attempt < 32; ++attempt) {
            const auto candidate = temporaryRoot
                / ("qtavcore-packet-cache-"
                   + std::to_string(timeId) + "-"
                   + std::to_string(
                       nextDirectoryId.fetch_add(
                           1,
                           std::memory_order_relaxed)));
            filesystemError.clear();
            if (std::filesystem::create_directory(
                    candidate,
                    filesystemError)) {
                directoryPath_ = candidate;
                break;
            }
            if (filesystemError) {
                error = "Could not create the packet-cache temporary "
                        "directory: "
                    + filesystemError.message();
                return false;
            }
        }
        if (directoryPath_.empty()) {
            error = "Could not reserve a unique packet-cache temporary "
                    "directory";
            return false;
        }

        filePath_ = directoryPath_ / "packets-0.cache";
        file_.open(
            filePath_,
            std::ios::in | std::ios::out | std::ios::binary
                | std::ios::trunc);
        if (!file_) {
            error = "Could not open the packet-cache temporary file";
            clearFilesLocked();
            return false;
        }
        return true;
    }

    bool compactLocked(std::string& error)
    {
        std::vector<std::shared_ptr<Entry>> activeEntries;
        activeEntries.reserve(entries_.size());
        for (const auto& candidate : entries_) {
            if (auto entry = candidate.lock()) {
                if (entry->generation == generation_) {
                    activeEntries.push_back(std::move(entry));
                }
            }
        }

        const auto replacementPath = directoryPath_
            / ("packets-" + std::to_string(++fileSerial_) + ".cache");
        std::fstream replacement(
            replacementPath,
            std::ios::in | std::ios::out | std::ios::binary
                | std::ios::trunc);
        if (!replacement) {
            error = "Could not create a compacted packet-cache file";
            return false;
        }

        std::vector<std::uint8_t> copyBuffer(64U * 1024U);
        std::vector<std::uint64_t> replacementOffsets;
        replacementOffsets.reserve(activeEntries.size());
        std::uint64_t replacementOffset = 0;
        file_.flush();
        for (const auto& entry : activeEntries) {
            replacementOffsets.push_back(replacementOffset);
            file_.clear();
            file_.seekg(static_cast<std::streamoff>(entry->offset));
            replacement.clear();
            replacement.seekp(
                static_cast<std::streamoff>(replacementOffset));
            auto remaining = entry->size;
            while (remaining > 0) {
                const auto chunk = static_cast<std::size_t>(std::min<
                    std::uint64_t>(remaining, copyBuffer.size()));
                file_.read(
                    reinterpret_cast<char*>(copyBuffer.data()),
                    static_cast<std::streamsize>(chunk));
                if (!file_) {
                    error = "Could not read the packet cache while compacting";
                    break;
                }
                replacement.write(
                    reinterpret_cast<const char*>(copyBuffer.data()),
                    static_cast<std::streamsize>(chunk));
                if (!replacement) {
                    error = "Could not write the compacted packet cache";
                    break;
                }
                remaining -= chunk;
                replacementOffset += chunk;
            }
            if (!error.empty()) {
                break;
            }
        }
        replacement.flush();
        if (!replacement || !error.empty()) {
            replacement.close();
            std::error_code ignored;
            std::filesystem::remove(replacementPath, ignored);
            file_.clear();
            return false;
        }

        const auto previousPath = filePath_;
        file_.close();
        file_ = std::move(replacement);
        filePath_ = replacementPath;
        appendOffset_ = replacementOffset;
        entries_.clear();
        for (std::size_t index = 0; index < activeEntries.size(); ++index) {
            activeEntries[index]->offset = replacementOffsets[index];
            entries_.push_back(activeEntries[index]);
        }
        std::error_code ignored;
        std::filesystem::remove(previousPath, ignored);
        return true;
    }

    void release(std::uint64_t generation, std::uint64_t size)
    {
        std::lock_guard<std::mutex> lock(mutex_);
        if (generation != generation_) {
            return;
        }
        activeBytes_ = size >= activeBytes_ ? 0 : activeBytes_ - size;
        if (activeBytes_ == 0) {
            clearFilesLocked();
        }
    }

    void clearFilesLocked()
    {
        if (file_.is_open()) {
            file_.close();
        }
        std::error_code ignored;
        if (!directoryPath_.empty()) {
            std::filesystem::remove_all(directoryPath_, ignored);
        }
        filePath_.clear();
        directoryPath_.clear();
        entries_.clear();
        activeBytes_ = 0;
        appendOffset_ = 0;
        fileSerial_ = 0;
        ++generation_;
    }

    mutable std::mutex mutex_;
    std::filesystem::path directoryPath_;
    std::filesystem::path filePath_;
    std::fstream file_;
    std::vector<std::weak_ptr<Entry>> entries_;
    std::uint64_t generation_ = 1;
    std::uint64_t nextEntryId_ = 1;
    std::uint64_t activeBytes_ = 0;
    std::uint64_t appendOffset_ = 0;
    std::uint64_t fileSerial_ = 0;
};

} // namespace

class Player::Impl {
public:
    struct VideoFrameSnapshot {
        VideoFrame frame;
        std::uint64_t sequence = 0;
        std::uint64_t generation = 0;
    };

    struct RenderBindingsSnapshot {
        RenderCallback callback;
        VideoRenderer legacyRenderer;
        std::unordered_map<void*, std::shared_ptr<VideoRenderAPI>> renderAPIs;
    };

    Impl()
    {
        audioSinkCallbackBridge_->owner = this;
        avformat_network_init();
        audioDecodeWorker_ =
            std::thread([this] { runAudioDecode(); });
        audioWorker_ = std::thread([this] { runAudioOutput(); });
        presentationWorker_ = std::thread([this] { runPresentation(); });
        videoDecodeWorker_ =
            std::thread([this] { runVideoDecode(); });
        worker_ = std::thread([this] { run(); });
    }

    ~Impl()
    {
        quitting_.store(true, std::memory_order_release);
        interruptEpoch_.fetch_add(1, std::memory_order_acq_rel);
        {
            std::lock_guard<std::mutex> lock(videoFrameSnapshotMutex_);
            presentationGeneration_.fetch_add(1, std::memory_order_acq_rel);
            clearCurrentVideoFrameSnapshot();
        }
        // Pair shutdown with each condition variable's wait mutex so a waiter
        // cannot observe the old predicate and go to sleep after notification.
        {
            std::lock_guard<std::mutex> lock(mutex_);
        }
        controlChanged_.notify_all();
        {
            std::lock_guard<std::mutex> lock(audioQueueMutex_);
        }
        audioQueueChanged_.notify_all();
        audioQueueSpace_.notify_all();
        audioQueueDrained_.notify_all();
        {
            std::lock_guard<std::mutex> lock(audioPacketMutex_);
        }
        audioPacketChanged_.notify_all();
        audioPacketSpace_.notify_all();
        audioPacketDrained_.notify_all();
        packetBufferChanged_.notify_all();
        {
            std::lock_guard<std::mutex> lock(presentationMutex_);
        }
        presentationChanged_.notify_all();
        presentationDrained_.notify_all();
        {
            std::lock_guard<std::mutex> lock(videoPacketMutex_);
        }
        videoPacketChanged_.notify_all();
        videoPacketSpace_.notify_all();
        videoPacketDrained_.notify_all();
        {
            std::lock_guard<std::mutex> lock(audioSinkCallbackBridge_->mutex);
            audioSinkCallbackBridge_->owner = nullptr;
        }
        if (worker_.joinable()) {
            worker_.join();
        }
        if (audioDecodeWorker_.joinable()) {
            audioDecodeWorker_.join();
        }
        if (audioWorker_.joinable()) {
            audioWorker_.join();
        }
        if (presentationWorker_.joinable()) {
            presentationWorker_.join();
        }
        if (videoDecodeWorker_.joinable()) {
            videoDecodeWorker_.join();
        }
        replaceAudioSink(
            nullptr,
            nullptr,
            nullptr,
            nullptr,
            audioSinkSerial_);
        closeVideoFrameProcessor();
        packetDiskStore_->clear();
        media_.reset();
        avformat_network_deinit();
    }

    enum class InputSource {
        Primary,
        ExternalAudio,
        ExternalSubtitle,
    };

    struct Decoder {
        AVCodecContext* context = nullptr;
        std::shared_ptr<AVCodecContext> contextLifetime;
        AVStream* stream = nullptr;
        int streamIndex = -1;
        int trackIndex = -1;
        std::int64_t startTimeUs = 0;
        InputSource source = InputSource::Primary;
        MediaType type = MediaType::Unknown;
        HardwareDeviceType hardwareDeviceType = HardwareDeviceType::Unknown;
        AVPixelFormat hardwarePixelFormat = AV_PIX_FMT_NONE;
        AVBufferRef* reusableHardwareFramesContext = nullptr;
        std::uintptr_t hardwareNativeIdentity = 0;
        std::uint32_t hardwareSurfaceGeneration = 0;
        bool allowSoftwareFallback = false;
        bool hardwareFallbackUsed = false;
        bool hardwareFallbackReported = false;

        Decoder() = default;
        Decoder(const Decoder&) = delete;
        Decoder& operator=(const Decoder&) = delete;
        Decoder(Decoder&& other) noexcept
        {
            swap(other);
        }
        Decoder& operator=(Decoder&& other) noexcept
        {
            if (this != &other) {
                Decoder replacement(std::move(other));
                swap(replacement);
            }
            return *this;
        }

        ~Decoder()
        {
            reset();
        }

        void reset()
        {
            av_buffer_unref(&reusableHardwareFramesContext);
            context = nullptr;
            contextLifetime.reset();
            stream = nullptr;
            streamIndex = -1;
            trackIndex = -1;
            startTimeUs = 0;
            source = InputSource::Primary;
            type = MediaType::Unknown;
            hardwareDeviceType = HardwareDeviceType::Unknown;
            hardwarePixelFormat = AV_PIX_FMT_NONE;
            hardwareNativeIdentity = 0;
            hardwareSurfaceGeneration = 0;
            allowSoftwareFallback = false;
            hardwareFallbackUsed = false;
            hardwareFallbackReported = false;
        }

        bool valid() const noexcept
        {
            return context && stream && streamIndex >= 0;
        }

        void swap(Decoder& other) noexcept
        {
            using std::swap;
            swap(context, other.context);
            swap(contextLifetime, other.contextLifetime);
            swap(stream, other.stream);
            swap(streamIndex, other.streamIndex);
            swap(trackIndex, other.trackIndex);
            swap(startTimeUs, other.startTimeUs);
            swap(source, other.source);
            swap(type, other.type);
            swap(hardwareDeviceType, other.hardwareDeviceType);
            swap(hardwarePixelFormat, other.hardwarePixelFormat);
            swap(reusableHardwareFramesContext,
                 other.reusableHardwareFramesContext);
            swap(hardwareNativeIdentity, other.hardwareNativeIdentity);
            swap(hardwareSurfaceGeneration, other.hardwareSurfaceGeneration);
            swap(allowSoftwareFallback, other.allowSoftwareFallback);
            swap(hardwareFallbackUsed, other.hardwareFallbackUsed);
            swap(hardwareFallbackReported, other.hardwareFallbackReported);

            // Hardware pixel-format negotiation keeps a pointer to the
            // owning Decoder in AVCodecContext::opaque. Preserve that
            // relationship when a prepared replacement is installed.
            if (context && context->opaque == &other) {
                context->opaque = this;
            }
            if (other.context && other.context->opaque == this) {
                other.context->opaque = &other;
            }
        }
    };

    struct InterruptContext {
        Impl* owner = nullptr;
        std::uint64_t epoch = 0;
    };

    struct DemuxState {
        std::shared_ptr<AVPacket> pendingPacket;
        std::int64_t pendingTimestamp = 0;
        std::int64_t pendingDuration = 0;
        bool end = false;

        void reset()
        {
            pendingPacket.reset();
            pendingTimestamp = 0;
            pendingDuration = 0;
            end = false;
        }
    };

    struct ReadRecoveryBudget {
        bool active = false;
        std::uint64_t mediaSerial = 0;
        std::int64_t progressTimestamp =
            std::numeric_limits<std::int64_t>::min();
        std::uint32_t attempts = 0;
    };

    struct ExternalInput {
        AVFormatContext* format = nullptr;
        std::string url;
        std::int64_t startTimeUs = 0;
        DemuxState demux;

        void resetDemux()
        {
            demux.reset();
        }

        void reset()
        {
            resetDemux();
            avformat_close_input(&format);
            url.clear();
            startTimeUs = 0;
        }
    };

    struct TrackSource {
        int index = -1;
        MediaType type = MediaType::Unknown;
        InputSource source = InputSource::Primary;
        int streamIndex = -1;
    };

    struct MediaContext {
        AVFormatContext* format = nullptr;
        DemuxState demux;
        ExternalInput externalAudio;
        ExternalInput externalSubtitle;
        std::vector<TrackSource> tracks;
        Decoder video;
        Decoder audio;
        Decoder subtitle;
        std::int64_t startTimeUs = 0;

        void reset()
        {
            video.reset();
            audio.reset();
            subtitle.reset();
            tracks.clear();
            demux.reset();
            externalAudio.reset();
            externalSubtitle.reset();
            avformat_close_input(&format);
            startTimeUs = 0;
        }
    };

    struct PrepareRequest {
        std::uint64_t id = 0;
        std::int64_t position = 0;
        SeekFlag flags = SeekFlag::FromStart;
        PrepareCallback callback;
    };

    struct SeekRequest {
        enum class Kind {
            Normal,
            Accurate,
            StepForward,
            StepBackwardExact,
            StepBackwardScan,
        };

        std::uint64_t id = 0;
        std::int64_t position = 0;
        std::int64_t selectionTarget = 0;
        SeekFlag flags = SeekFlag::FromStart;
        Kind kind = Kind::Normal;
        SeekCallback callback;
    };

    struct AccurateSeekState {
        std::uint64_t generation = 0;
        std::int64_t target = 0;
        SeekRequest::Kind kind = SeekRequest::Kind::Accurate;
        bool pauseAfter = false;
        bool selectionQueued = false;
        VideoFrame previousCandidate;
        VideoFrame candidateBeforePrevious;
        SeekCallback callback;
    };

    struct SeekCompletion {
        std::int64_t position = -1;
        SeekCallback callback;
    };

    struct AccurateVideoDecision {
        enum class Action {
            DeliverNormally,
            Drop,
            DeliverSelected,
        };

        Action action = Action::DeliverNormally;
        VideoFrame frame;
        bool forceImmediate = false;
        std::int64_t previousTimestamp = -1;
    };

    struct TrackSwitchRequest {
        std::uint64_t mediaSerial = 0;
        MediaType type = MediaType::Unknown;
        int track = -1;
        std::int64_t position = 0;
    };

    struct AudioSinkCallbackBridge {
        std::mutex mutex;
        Impl* owner = nullptr;
    };

    struct QueuedAudioFrame {
        AudioFrame frame;
        int track = -1;
        std::uint64_t generation = 0;
        std::int64_t duration = 0;
    };

    struct QueuedVideoPacket {
        std::shared_ptr<AVPacket> packet;
        std::shared_ptr<AVPacket> diskPacketProperties;
        std::shared_ptr<PacketDiskStore::Entry> diskEntry;
        std::uint64_t generation = 0;
        std::int64_t timestamp = 0;
        std::int64_t duration = 0;
        std::uint64_t bytes = 0;
        bool end = false;
    };

    using QueuedAudioPacket = QueuedVideoPacket;

    struct DiskPacketResult {
        std::shared_ptr<AVPacket> properties;
        std::shared_ptr<PacketDiskStore::Entry> entry;
        bool capacityLimited = false;
        std::string error;
    };

    struct PacketQueueMetrics {
        std::int64_t bufferedMilliseconds = 0;
        std::int64_t memoryBufferedMilliseconds = 0;
        std::int64_t diskBufferedMilliseconds = 0;
        std::uint64_t bytes = 0;
        std::uint64_t memoryBytes = 0;
        std::uint64_t diskBytes = 0;
        std::size_t packets = 0;
        std::size_t memoryPackets = 0;
        std::size_t diskPackets = 0;
    };

    struct PacketBufferState {
        bool buffering = false;
        PacketBufferingReason reason = PacketBufferingReason::None;
        std::int64_t targetMilliseconds = 0;
        std::uint64_t generation = 0;
        bool needsAudio = false;
        bool needsVideo = false;
        bool audioEnded = false;
        bool videoEnded = false;
        bool capacityLimited = false;
    };

    struct PresentationItem {
        enum class Type {
            Audio,
            Video,
            Subtitle,
        };

        Type type = Type::Video;
        AudioFrame audio;
        VideoFrame video;
        SubtitleFrame subtitle;
        int track = -1;
        std::uint64_t generation = 0;
        bool forceImmediate = false;
        bool completesAccurateSeek = false;
        std::int64_t previousVideoTimestamp = -1;

        std::int64_t timestamp() const noexcept
        {
            switch (type) {
            case Type::Audio:
                return audio.timestamp();
            case Type::Video:
                return video.timestamp();
            case Type::Subtitle:
                return subtitle.timestamp();
            }
            return 0;
        }
    };

    struct CachedAudioClock {
        bool valid = false;
        std::uint64_t sinkSerial = 0;
        std::int64_t position = 0;
        std::int64_t submittedUntil = 0;
        std::int64_t rawAnchor = 0;
        std::int64_t mediaAnchor = 0;
        double playbackRate = 1.0;
        bool hasRateAnchor = false;
        Clock::time_point sampledAt = Clock::now();
    };

    void setMedia(std::string value)
    {
        {
            std::lock_guard<std::mutex> lock(mutex_);
            url_ = std::move(value);
            ++mediaSerial_;
            loadedSerial_ = 0;
            prepareRequest_.reset();
            seekRequest_.reset();
            trackSwitchRequests_.clear();
            accurateSeek_.reset();
            seekCompletion_.reset();
            networkRecoveryStatus_ = {};
            if (url_.empty()) {
                requestedState_ = State::Stopped;
            }
        }
        resetPlaybackStatistics();
        resetPlaybackQueues();
        interruptEpoch_.fetch_add(1, std::memory_order_acq_rel);
        controlChanged_.notify_all();
    }

    std::string url() const
    {
        std::lock_guard<std::mutex> lock(mutex_);
        return url_;
    }

    bool setExternalMedia(MediaType type, std::string value)
    {
        if (type != MediaType::Audio && type != MediaType::Subtitle) {
            return false;
        }

        const auto reopenPosition = position();
        bool reopen = false;
        {
            std::lock_guard<std::mutex> lock(mutex_);
            auto& configured = type == MediaType::Audio
                ? externalAudioUrl_
                : externalSubtitleUrl_;
            if (configured == value) {
                return true;
            }
            configured = std::move(value);
            if (!url_.empty() && requestedState_ != State::Stopped) {
                ++mediaSerial_;
                loadedSerial_ = 0;
                seekRequest_.reset();
                trackSwitchRequests_.clear();
                accurateSeek_.reset();
                seekCompletion_.reset();
                prepareRequest_ = PrepareRequest {
                    ++requestSerial_,
                    reopenPosition,
                    SeekFlag::KeyFrame,
                    {},
                };
                reopen = true;
            }
        }
        if (reopen) {
            invalidatePlaybackQueues();
            interruptEpoch_.fetch_add(1, std::memory_order_acq_rel);
            controlChanged_.notify_all();
        }
        return true;
    }

    std::string externalMedia(MediaType type) const
    {
        std::lock_guard<std::mutex> lock(mutex_);
        if (type == MediaType::Audio) {
            return externalAudioUrl_;
        }
        if (type == MediaType::Subtitle) {
            return externalSubtitleUrl_;
        }
        return {};
    }

    void prepare(
        std::int64_t startPosition,
        PrepareCallback callback,
        SeekFlag flags)
    {
        {
            std::lock_guard<std::mutex> lock(mutex_);
            accurateSeek_.reset();
            seekCompletion_.reset();
            prepareRequest_ = PrepareRequest {
                ++requestSerial_,
                std::max<std::int64_t>(0, startPosition),
                flags,
                std::move(callback),
            };
            requestedState_ = State::Paused;
        }
        resetPlaybackQueues();
        interruptEpoch_.fetch_add(1, std::memory_order_acq_rel);
        controlChanged_.notify_all();
    }

    bool seek(
        std::int64_t target,
        SeekFlag flags,
        SeekCallback callback)
    {
        const auto current = hasFlag(flags, SeekFlag::FromNow)
            ? position()
            : static_cast<std::int64_t>(0);
        {
            std::lock_guard<std::mutex> lock(mutex_);
            if (!hasOpenMedia_ || currentState_ == State::Stopped) {
                return false;
            }
            if (hasFlag(flags, SeekFlag::FromNow)) {
                target += current;
            }
            target = std::max<std::int64_t>(0, target);
            if (mediaInfo_.duration > 0) {
                target = std::min(target, mediaInfo_.duration);
            }
            accurateSeek_.reset();
            seekCompletion_.reset();
            seekRequest_ = SeekRequest {
                ++requestSerial_,
                target,
                target,
                flags,
                hasFlag(flags, SeekFlag::Accurate)
                    ? SeekRequest::Kind::Accurate
                    : SeekRequest::Kind::Normal,
                std::move(callback),
            };
            // Publish the discontinuity before the playback worker can take
            // the request. Otherwise a callback thread can unlock here, let
            // the worker enter seekMedia(), and only then invalidate its new
            // interrupt epoch or presentation generation.
            interruptEpoch_.fetch_add(1, std::memory_order_acq_rel);
            invalidatePlaybackGeneration();
        }
        notifyPlaybackQueueInvalidation();
        controlChanged_.notify_all();
        return true;
    }

    bool stepFrame(bool forward, SeekCallback callback)
    {
        const auto snapshot = std::atomic_load_explicit(
            &currentVideoFrameSnapshot_,
            std::memory_order_acquire);
        {
            std::lock_guard<std::mutex> lock(mutex_);
            if (!hasOpenMedia_ || currentState_ == State::Stopped
                || !mediaInfo_.seekable
                || mediaInfo_.activeVideoTrack < 0) {
                return false;
            }

            const auto generation = presentationGeneration_.load(
                std::memory_order_acquire);
            const bool snapshotCurrent = snapshot && snapshot->frame
                && snapshot->generation == generation;
            const auto reference = snapshotCurrent
                ? snapshot->frame.timestamp()
                : currentPosition_;
            const bool exactPrevious = !forward
                && lastPresentedVideoTimestamp_ == reference
                && previousPresentedVideoTimestamp_ >= 0
                && previousPresentedVideoTimestamp_ < reference;
            const auto selectionTarget = exactPrevious
                ? previousPresentedVideoTimestamp_
                : reference;
            const auto seekPosition = !forward && !exactPrevious
                ? rangeStart_
                : selectionTarget;

            accurateSeek_.reset();
            seekCompletion_.reset();
            seekRequest_ = SeekRequest {
                ++requestSerial_,
                seekPosition,
                selectionTarget,
                SeekFlag::Accurate,
                forward
                    ? SeekRequest::Kind::StepForward
                    : (exactPrevious
                              ? SeekRequest::Kind::StepBackwardExact
                              : SeekRequest::Kind::StepBackwardScan),
                std::move(callback),
            };
            requestedState_ = State::Paused;
            interruptEpoch_.fetch_add(1, std::memory_order_acq_rel);
            invalidatePlaybackGeneration();
        }
        notifyPlaybackQueueInvalidation();
        controlChanged_.notify_all();
        return true;
    }

    bool stepForward(SeekCallback callback)
    {
        return stepFrame(true, std::move(callback));
    }

    bool stepBackward(SeekCallback callback)
    {
        return stepFrame(false, std::move(callback));
    }

    void setState(State value)
    {
        {
            std::lock_guard<std::mutex> lock(mutex_);
            requestedState_ = value;
            if (accurateSeek_
                && accurateSeek_->kind == SeekRequest::Kind::Accurate) {
                accurateSeek_->pauseAfter = value != State::Playing;
            }
            if (value == State::Stopped) {
                prepareRequest_.reset();
                seekRequest_.reset();
                trackSwitchRequests_.clear();
                accurateSeek_.reset();
                seekCompletion_.reset();
            }
        }
        if (value == State::Stopped) {
            resetPlaybackQueues();
            interruptEpoch_.fetch_add(1, std::memory_order_acq_rel);
        }
        controlChanged_.notify_all();
        audioQueueChanged_.notify_all();
        audioQueueSpace_.notify_all();
        presentationChanged_.notify_all();
        audioPacketChanged_.notify_all();
        audioPacketDrained_.notify_all();
        videoPacketChanged_.notify_all();
        videoPacketDrained_.notify_all();
    }

    State state() const
    {
        std::lock_guard<std::mutex> lock(mutex_);
        return currentState_;
    }

    bool waitFor(State value, long timeoutMs)
    {
        std::unique_lock<std::mutex> lock(mutex_);
        const auto predicate = [this, value] {
            return currentState_ == value
                || quitting_.load(std::memory_order_acquire);
        };
        if (timeoutMs < 0) {
            stateChanged_.wait(lock, predicate);
            return currentState_ == value;
        }
        return stateChanged_.wait_for(
                   lock,
                   Milliseconds(timeoutMs),
                   predicate)
            && currentState_ == value;
    }

    MediaStatus mediaStatus() const
    {
        std::lock_guard<std::mutex> lock(mutex_);
        return status_;
    }

    MediaInfo mediaInfo() const
    {
        std::lock_guard<std::mutex> lock(mutex_);
        return mediaInfo_;
    }

    bool setActiveTrack(MediaType type, int track)
    {
        if (type != MediaType::Audio && type != MediaType::Video
            && type != MediaType::Subtitle) {
            return false;
        }
        if (track < -1) {
            return false;
        }

        const auto requestedPosition = position();
        {
            std::lock_guard<std::mutex> lock(mutex_);
            if (!hasOpenMedia_ || loadedSerial_ != mediaSerial_
                || currentState_ == State::Stopped) {
                return false;
            }

            if (track >= 0) {
                const auto found = std::find_if(
                    mediaInfo_.tracks.begin(),
                    mediaInfo_.tracks.end(),
                    [type, track](const TrackInfo& candidate) {
                        return candidate.index == track
                            && candidate.type == type;
                    });
                if (found == mediaInfo_.tracks.end()) {
                    return false;
                }
            }

            int selected = mediaInfo_.activeSubtitleTrack;
            if (type == MediaType::Audio) {
                selected = mediaInfo_.activeAudioTrack;
            } else if (type == MediaType::Video) {
                selected = mediaInfo_.activeVideoTrack;
            }
            for (auto request = trackSwitchRequests_.rbegin();
                 request != trackSwitchRequests_.rend();
                 ++request) {
                if (request->type == type) {
                    selected = request->track;
                    break;
                }
            }
            if (selected == track) {
                return true;
            }

            trackSwitchRequests_.push_back({
                mediaSerial_,
                type,
                track,
                requestedPosition,
            });
            accurateSeek_.reset();
            seekCompletion_.reset();
        }

        // Match seek(): public control calls invalidate without waiting. The
        // playback worker drains in-flight decoder work before replacement.
        invalidatePlaybackQueues();
        interruptEpoch_.fetch_add(1, std::memory_order_acq_rel);
        controlChanged_.notify_all();
        return true;
    }

    std::int64_t position() const
    {
        if (const auto audioPosition = audioClockPosition()) {
            return clampPosition(*audioPosition);
        }
        std::lock_guard<std::mutex> lock(mutex_);
        return clockPositionLocked();
    }

    PlaybackStatistics playbackStatistics() const noexcept
    {
        return {
            decodedVideoFrames_.load(std::memory_order_relaxed),
            videoQueueOverflowDrops_.load(std::memory_order_relaxed),
            lateVideoDrops_.load(std::memory_order_relaxed),
            deliveredVideoFrames_.load(std::memory_order_relaxed),
            maximumQueuedVideoFrames_.load(std::memory_order_relaxed),
            videoPresentationStarvations_.load(
                std::memory_order_relaxed),
            maximumVideoPresentationStarvationMilliseconds_.load(
                std::memory_order_relaxed),
            lowLatencyVideoQueueDrops_.load(std::memory_order_relaxed),
            networkRecoveryAttempts_.load(std::memory_order_relaxed),
            successfulNetworkRecoveries_.load(std::memory_order_relaxed),
            failedNetworkRecoveries_.load(std::memory_order_relaxed),
        };
    }

    void setLivePlaybackPolicy(LivePlaybackPolicy policy)
    {
        policy.maximumQueuedVideoFrames = std::clamp<std::uint32_t>(
            policy.maximumQueuedVideoFrames,
            1,
            static_cast<std::uint32_t>(
                kMaximumQueuedHardwareVideoFrames));
        policy.lateVideoFrameThresholdMilliseconds =
            std::max<std::int64_t>(
                0,
                policy.lateVideoFrameThresholdMilliseconds);
        std::lock_guard<std::mutex> lock(mutex_);
        livePlaybackPolicy_ = policy;
    }

    LivePlaybackPolicy livePlaybackPolicy() const
    {
        std::lock_guard<std::mutex> lock(mutex_);
        return livePlaybackPolicy_;
    }

    void setNetworkRecoveryPolicy(NetworkRecoveryPolicy policy)
    {
        policy.maximumAttempts = std::clamp<std::uint32_t>(
            policy.maximumAttempts,
            1,
            32);
        policy.initialRetryDelayMilliseconds = std::max<std::int64_t>(
            0,
            policy.initialRetryDelayMilliseconds);
        policy.maximumRetryDelayMilliseconds = std::max(
            policy.initialRetryDelayMilliseconds,
            policy.maximumRetryDelayMilliseconds);
        std::lock_guard<std::mutex> lock(mutex_);
        networkRecoveryPolicy_ = policy;
    }

    NetworkRecoveryPolicy networkRecoveryPolicy() const
    {
        std::lock_guard<std::mutex> lock(mutex_);
        return networkRecoveryPolicy_;
    }

    NetworkRecoveryStatus networkRecoveryStatus() const
    {
        std::lock_guard<std::mutex> lock(mutex_);
        return networkRecoveryStatus_;
    }

    void setPacketBufferPolicy(PacketBufferPolicy policy)
    {
        policy.initialBufferMilliseconds =
            std::max<std::int64_t>(0, policy.initialBufferMilliseconds);
        policy.rebufferMilliseconds =
            std::max<std::int64_t>(0, policy.rebufferMilliseconds);
        policy.maximumBufferMilliseconds = std::max<std::int64_t>(
            1,
            policy.maximumBufferMilliseconds);
        if (policy.maximumBufferBytes == 0) {
            policy.maximumBufferBytes = kDefaultMaximumPacketBufferBytes;
        }
        policy.underflowDetectionMilliseconds = std::max<std::int64_t>(
            0,
            policy.underflowDetectionMilliseconds);
        policy.diskCache.maximumCacheMilliseconds =
            std::max<std::int64_t>(
                0,
                policy.diskCache.maximumCacheMilliseconds);
        if (policy.diskCache.maximumCacheBytes == 0) {
            policy.diskCache.maximumCacheBytes =
                kDefaultMaximumPacketDiskCacheBytes;
        }
        const auto largestTarget = std::max(
            policy.initialBufferMilliseconds,
            policy.rebufferMilliseconds);
        if (policy.diskCache.enabled) {
            const auto requiredDiskDuration = largestTarget
                    > policy.maximumBufferMilliseconds
                ? largestTarget - policy.maximumBufferMilliseconds
                : 0;
            policy.diskCache.maximumCacheMilliseconds = std::max(
                policy.diskCache.maximumCacheMilliseconds,
                requiredDiskDuration);
        } else {
            policy.maximumBufferMilliseconds = std::max(
                policy.maximumBufferMilliseconds,
                largestTarget);
        }
        {
            std::lock_guard<std::mutex> lock(mutex_);
            packetBufferPolicy_ = policy;
        }
        diskCacheUnavailable_.store(false, std::memory_order_release);
        std::uint64_t activeGeneration = 0;
        std::int64_t activeTarget = 0;
        {
            std::lock_guard<std::mutex> lock(packetBufferMutex_);
            if (packetBufferState_.buffering) {
                activeGeneration = packetBufferState_.generation;
                activeTarget = packetBufferState_.reason
                        == PacketBufferingReason::InitialPlayback
                    ? policy.initialBufferMilliseconds
                    : policy.rebufferMilliseconds;
                packetBufferState_.targetMilliseconds = activeTarget;
            }
        }
        if (activeGeneration != 0
            && (!policy.enabled || activeTarget <= 0)) {
            completePacketBuffering(activeGeneration);
        } else if (activeGeneration != 0) {
            updatePacketBuffering(activeGeneration);
        }
        packetBufferChanged_.notify_all();
        audioPacketChanged_.notify_all();
        videoPacketChanged_.notify_all();
    }

    PacketBufferPolicy packetBufferPolicy() const
    {
        std::lock_guard<std::mutex> lock(mutex_);
        return packetBufferPolicy_;
    }

    PacketBufferStatus packetBufferStatus() const
    {
        return currentPacketBufferStatus();
    }

    std::string packetDiskCachePath() const
    {
        return packetDiskStore_->path();
    }

    bool clearPacketDiskCache()
    {
        if (packetDiskStore_->empty()) {
            return true;
        }

        const auto restartPosition = position();
        bool restart = false;
        {
            std::lock_guard<std::mutex> lock(mutex_);
            restart = hasOpenMedia_ && currentState_ != State::Stopped;
            if (restart && !mediaInfo_.seekable) {
                return false;
            }
            if (restart) {
                seekRequest_ = SeekRequest {
                    ++requestSerial_,
                    restartPosition,
                    restartPosition,
                    SeekFlag::KeyFrame,
                    SeekRequest::Kind::Normal,
                    {},
                };
            }
        }

        if (restart) {
            resetPlaybackQueues();
            interruptEpoch_.fetch_add(1, std::memory_order_acq_rel);
            controlChanged_.notify_all();
        } else {
            packetDiskStore_->clear();
        }
        return true;
    }

    void onStateChanged(StateCallback callback)
    {
        std::lock_guard<std::mutex> lock(mutex_);
        stateCallback_ = std::move(callback);
    }

    void onMediaStatus(StatusCallback callback)
    {
        std::lock_guard<std::mutex> lock(mutex_);
        statusCallback_ = std::move(callback);
    }

    void onPacketBufferStatus(PacketBufferStatusCallback callback)
    {
        std::lock_guard<std::mutex> lock(mutex_);
        packetBufferStatusCallback_ = std::move(callback);
    }

    void onNetworkRecoveryStatus(NetworkRecoveryStatusCallback callback)
    {
        std::lock_guard<std::mutex> lock(mutex_);
        networkRecoveryStatusCallback_ = std::move(callback);
    }

    void onEvent(EventCallback callback)
    {
        std::lock_guard<std::mutex> lock(mutex_);
        eventCallback_ = std::move(callback);
    }

    void onVideoFrame(VideoFrameCallback callback)
    {
        std::lock_guard<std::mutex> lock(mutex_);
        videoFrameCallback_ = std::move(callback);
    }

    void onAudioFrame(AudioFrameCallback callback)
    {
        std::lock_guard<std::mutex> lock(mutex_);
        audioFrameCallback_ = std::move(callback);
    }

    void onSubtitleFrame(SubtitleFrameCallback callback)
    {
        std::lock_guard<std::mutex> lock(mutex_);
        subtitleFrameCallback_ = std::move(callback);
    }

    void setVideoFrameScheduler(VideoFrameScheduler scheduler)
    {
        std::lock_guard<std::mutex> lock(mutex_);
        videoFrameScheduler_ = std::move(scheduler);
    }

    void setAudioSink(std::shared_ptr<AudioSink> sink)
    {
        {
            std::lock_guard<std::mutex> lock(mutex_);
            audioSink_ = std::move(sink);
            ++audioSinkSerial_;
        }
        controlChanged_.notify_all();
    }

    void setAudioFrameConverter(
        std::shared_ptr<AudioFrameConverter> converter)
    {
        {
            std::lock_guard<std::mutex> lock(mutex_);
            audioFrameConverter_ = std::move(converter);
            ++audioSinkSerial_;
        }
        controlChanged_.notify_all();
    }

    void setAudioTimeStretcher(
        std::shared_ptr<AudioTimeStretcher> stretcher)
    {
        {
            std::lock_guard<std::mutex> lock(mutex_);
            audioTimeStretcher_ = std::move(stretcher);
            ++audioSinkSerial_;
        }
        controlChanged_.notify_all();
    }

    void setAudioFrameProcessor(
        std::shared_ptr<AudioFrameProcessor> processor)
    {
        {
            std::lock_guard<std::mutex> lock(mutex_);
            audioFrameProcessor_ = std::move(processor);
            ++audioSinkSerial_;
        }
        controlChanged_.notify_all();
    }

    void setVideoFrameProcessor(
        std::shared_ptr<VideoFrameProcessor> processor)
    {
        const auto reopenPosition = position();
        bool reopen = false;
        {
            std::lock_guard<std::mutex> lock(mutex_);
            if (videoFrameProcessor_ == processor) {
                return;
            }
            videoFrameProcessor_ = std::move(processor);
            ++videoFrameProcessorSerial_;
            if (!url_.empty() && requestedState_ != State::Stopped) {
                ++mediaSerial_;
                loadedSerial_ = 0;
                seekRequest_.reset();
                trackSwitchRequests_.clear();
                accurateSeek_.reset();
                seekCompletion_.reset();
                prepareRequest_ = PrepareRequest {
                    ++requestSerial_,
                    reopenPosition,
                    SeekFlag::KeyFrame,
                    {},
                };
                reopen = true;
            }
        }
        if (reopen) {
            invalidatePlaybackQueues();
            interruptEpoch_.fetch_add(1, std::memory_order_acq_rel);
        }
        controlChanged_.notify_all();
    }

    void setHardwareDecodeConfig(HardwareDecodeConfig config)
    {
        {
            std::lock_guard<std::mutex> lock(mutex_);
            if (hardwareDecodeConfig_.deviceType == config.deviceType
                && hardwareDecodeConfig_.allowSoftwareFallback
                    == config.allowSoftwareFallback
                && hardwareDecodeConfig_.device == config.device
                && hardwareDecodeConfig_.extraHardwareFrames
                    == config.extraHardwareFrames
                && hardwareDecodeConfig_.requireSuppliedDevice
                    == config.requireSuppliedDevice
                && hardwareDecodeConfig_.decoderWrapper
                    == config.decoderWrapper
                && hardwareDecodeConfig_.surfaceGeneration
                    == config.surfaceGeneration) {
                return;
            }
            hardwareDecodeConfig_ = config;
            ++mediaSerial_;
            loadedSerial_ = 0;
        }
        resetPlaybackQueues();
        interruptEpoch_.fetch_add(1, std::memory_order_acq_rel);
        controlChanged_.notify_all();
    }

    HardwareDecodeConfig hardwareDecodeConfig() const
    {
        std::lock_guard<std::mutex> lock(mutex_);
        return hardwareDecodeConfig_;
    }

    void setRenderCallback(RenderCallback callback)
    {
        std::lock_guard<std::mutex> lock(renderBindingsMutex_);
        auto updated = copyRenderBindings();
        updated->callback = std::move(callback);
        publishRenderBindings(std::move(updated));
    }

    void setVideoRenderer(VideoRenderer renderer)
    {
        std::lock_guard<std::mutex> lock(renderBindingsMutex_);
        auto updated = copyRenderBindings();
        updated->legacyRenderer = std::move(renderer);
        publishRenderBindings(std::move(updated));
    }

    void setVideoRenderAPI(
        std::shared_ptr<VideoRenderAPI> renderer,
        void* opaque)
    {
        std::lock_guard<std::mutex> lock(renderBindingsMutex_);
        auto updated = copyRenderBindings();
        if (renderer) {
            updated->renderAPIs[opaque] = std::move(renderer);
        } else {
            updated->renderAPIs.erase(opaque);
        }
        publishRenderBindings(std::move(updated));
    }

    VideoRenderResult renderVideoDetailed(void* opaque)
    {
        const auto frame = std::atomic_load_explicit(
            &currentVideoFrameSnapshot_,
            std::memory_order_acquire);
        const auto currentGeneration =
            presentationGeneration_.load(std::memory_order_acquire);
        if (!frame || frame->generation != currentGeneration) {
            return {
                VideoRenderStatus::NoFrame,
                -1.0,
                0,
                currentGeneration,
                0,
                {},
            };
        }

        const auto bindings = std::atomic_load_explicit(
            &renderBindings_,
            std::memory_order_acquire);
        std::shared_ptr<VideoRenderAPI> renderAPI;
        VideoRenderer legacyRenderer;
        if (bindings) {
            const auto found = bindings->renderAPIs.find(opaque);
            if (found != bindings->renderAPIs.end()) {
                renderAPI = found->second;
            }
            legacyRenderer = bindings->legacyRenderer;
        }

        VideoRenderAttemptResult attempt {
            VideoRenderAttemptStatus::Presented,
            0,
            {},
        };
        if (renderAPI) {
            attempt = renderAPI->renderDetailed(frame->frame);
        } else if (legacyRenderer) {
            legacyRenderer(frame->frame, opaque);
        }

        const auto completedGeneration =
            presentationGeneration_.load(std::memory_order_acquire);
        if (frame->generation != completedGeneration) {
            return {
                VideoRenderStatus::FrameDiscarded,
                static_cast<double>(frame->frame.timestamp()) / 1000.0,
                frame->sequence,
                completedGeneration,
                0,
                "The presentation generation changed during the render attempt",
            };
        }

        VideoRenderStatus status = VideoRenderStatus::RendererError;
        switch (attempt.status) {
        case VideoRenderAttemptStatus::Presented:
            status = VideoRenderStatus::Rendered;
            break;
        case VideoRenderAttemptStatus::DeferredUntilRedraw:
            status = VideoRenderStatus::RendererDeferred;
            break;
        case VideoRenderAttemptStatus::RetryAfterBackoff:
            status = VideoRenderStatus::RendererBusy;
            break;
        case VideoRenderAttemptStatus::Discarded:
            status = VideoRenderStatus::FrameDiscarded;
            break;
        case VideoRenderAttemptStatus::SurfaceLost:
            status = VideoRenderStatus::SurfaceLost;
            break;
        case VideoRenderAttemptStatus::FatalError:
            status = VideoRenderStatus::RendererError;
            break;
        }
        return {
            status,
            static_cast<double>(frame->frame.timestamp()) / 1000.0,
            frame->sequence,
            frame->generation,
            attempt.retryAfterMilliseconds,
            std::move(attempt.detail),
            attempt.retryReason,
        };
    }

    double renderVideo(void* opaque)
    {
        const auto result = renderVideoDetailed(opaque);
        return result.status == VideoRenderStatus::Rendered
            ? result.timestamp
            : -1.0;
    }

    void setPlaybackRate(float value)
    {
        if (!(value > 0.0F)) {
            return;
        }
        const auto current = position();
        bool restartAtCurrentPosition = false;
        {
            std::lock_guard<std::mutex> lock(mutex_);
            if (std::abs(playbackRate_ - value) <= 0.000001F) {
                return;
            }
            playbackRate_ = value;
            resetClockLocked(current);
            // A stretcher is configured against one fixed rate, and a native
            // sink may already own PCM produced at the previous rate. Reopen
            // the chain. Seekable media also re-decodes from the current
            // media position so flushing the device queue cannot skip audio
            // that had already crossed the core output worker.
            ++audioSinkSerial_;
            restartAtCurrentPosition = audioSink_ && hasOpenMedia_
                && currentState_ != State::Stopped
                && mediaInfo_.seekable;
        }
        invalidateAudioClock();
        if (restartAtCurrentPosition
            && seek(current, SeekFlag::Accurate, {})) {
            return;
        }
        controlChanged_.notify_all();
        audioQueueChanged_.notify_all();
        presentationChanged_.notify_all();
    }

    float playbackRate() const
    {
        std::lock_guard<std::mutex> lock(mutex_);
        return playbackRate_;
    }

    void setLoop(int count)
    {
        std::lock_guard<std::mutex> lock(mutex_);
        loopCount_ = count;
        loopsCompleted_ = 0;
    }

    void setRange(std::int64_t start, std::int64_t end)
    {
        std::lock_guard<std::mutex> lock(mutex_);
        rangeStart_ = std::max<std::int64_t>(0, start);
        rangeEnd_ = end <= 0 ? MediaEnd : std::max(rangeStart_, end);
        loopsCompleted_ = 0;
    }

    void setProperty(std::string key, std::string value)
    {
        const auto originalKey = key;
        const auto originalValue = value;
        {
            std::lock_guard<std::mutex> lock(mutex_);
            properties_[std::move(key)] = std::move(value);
        }

        try {
            if (originalKey == "speed" || originalKey == "playbackRate") {
                setPlaybackRate(std::stof(originalValue));
            } else if (originalKey == "loop") {
                setLoop(std::stoi(originalValue));
            }
        } catch (const std::exception&) {
            publishEvent({
                "property.error",
                "Invalid value for property '" + originalKey + "'",
                AVERROR(EINVAL),
            });
        }
    }

    std::string property(
        const std::string& key,
        std::string defaultValue) const
    {
        std::lock_guard<std::mutex> lock(mutex_);
        const auto found = properties_.find(key);
        return found == properties_.end() ? std::move(defaultValue)
                                          : found->second;
    }

private:
    std::shared_ptr<RenderBindingsSnapshot> copyRenderBindings() const
    {
        const auto current = std::atomic_load_explicit(
            &renderBindings_,
            std::memory_order_acquire);
        return current
            ? std::make_shared<RenderBindingsSnapshot>(*current)
            : std::make_shared<RenderBindingsSnapshot>();
    }

    void publishRenderBindings(
        std::shared_ptr<RenderBindingsSnapshot> bindings)
    {
        std::shared_ptr<const RenderBindingsSnapshot> immutable =
            std::move(bindings);
        std::atomic_store_explicit(
            &renderBindings_,
            std::move(immutable),
            std::memory_order_release);
    }

    void clearCurrentVideoFrameSnapshot() noexcept
    {
        std::atomic_store_explicit(
            &currentVideoFrameSnapshot_,
            std::shared_ptr<const VideoFrameSnapshot> {},
            std::memory_order_release);
    }

    bool hasCurrentVideoFrame() const noexcept
    {
        const auto frame = std::atomic_load_explicit(
            &currentVideoFrameSnapshot_,
            std::memory_order_acquire);
        return frame
            && frame->generation
                == presentationGeneration_.load(std::memory_order_acquire);
    }

    static int interruptCallback(void* opaque)
    {
        const auto* context = static_cast<InterruptContext*>(opaque);
        if (!context || !context->owner) {
            return 0;
        }
        return context->owner->quitting_.load(std::memory_order_acquire)
                || context->owner->interruptEpoch_.load(
                       std::memory_order_acquire)
                    != context->epoch
            ? 1
            : 0;
    }

    static AVPixelFormat selectHardwarePixelFormat(
        AVCodecContext* context,
        const AVPixelFormat* formats)
    {
        auto* decoder =
            context ? static_cast<Decoder*>(context->opaque) : nullptr;
        if (!decoder || !formats) {
            return AV_PIX_FMT_NONE;
        }
        for (const auto* format = formats; *format != AV_PIX_FMT_NONE;
             ++format) {
            if (*format == decoder->hardwarePixelFormat) {
                if (decoder->hardwareDeviceType
                        == HardwareDeviceType::D3D11
                    && context->hw_device_ctx) {
                    configureReusableHardwareFramesContext(
                        context,
                        *decoder,
                        *format);
                }
                return *format;
            }
        }
        if (!decoder->allowSoftwareFallback) {
            return AV_PIX_FMT_NONE;
        }
        decoder->hardwareFallbackUsed = true;
        for (const auto* format = formats; *format != AV_PIX_FMT_NONE;
             ++format) {
            if (isSoftwarePixelFormat(*format)) {
                return *format;
            }
        }
        return AV_PIX_FMT_NONE;
    }

    static bool hardwareFramesContextsAreCompatible(
        const AVBufferRef* initializedReference,
        const AVBufferRef* requiredReference) noexcept
    {
        if (!initializedReference || !initializedReference->data
            || !requiredReference || !requiredReference->data) {
            return false;
        }
        const auto* initialized = reinterpret_cast<const AVHWFramesContext*>(
            initializedReference->data);
        const auto* required = reinterpret_cast<const AVHWFramesContext*>(
            requiredReference->data);
        const int requiredPoolSize = required->initial_pool_size > 0
            ? required->initial_pool_size + 3
            : 0;
        return initialized->device_ref && required->device_ref
            && initialized->device_ref->data == required->device_ref->data
            && initialized->format == required->format
            && initialized->sw_format == required->sw_format
            && initialized->width == required->width
            && initialized->height == required->height
            && initialized->initial_pool_size >= requiredPoolSize;
    }

    static void configureReusableHardwareFramesContext(
        AVCodecContext* context,
        Decoder& decoder,
        AVPixelFormat hardwarePixelFormat)
    {
        AVBufferRef* requiredReference = nullptr;
        const int parametersError = avcodec_get_hw_frames_parameters(
            context,
            context->hw_device_ctx,
            hardwarePixelFormat,
            &requiredReference);
        if (parametersError < 0 || !requiredReference) {
            return;
        }

        if (hardwareFramesContextsAreCompatible(
                decoder.reusableHardwareFramesContext,
                requiredReference)) {
            context->hw_frames_ctx =
                av_buffer_ref(decoder.reusableHardwareFramesContext);
            av_buffer_unref(&requiredReference);
            return;
        }

        auto* required = reinterpret_cast<AVHWFramesContext*>(
            requiredReference->data);
        if (required->initial_pool_size > 0) {
            // Match ff_decode_get_hw_frames_ctx(): the public parameters API
            // guarantees one work surface and FFmpeg's automatic path adds
            // three more before initializing a fixed-size decoder pool.
            required->initial_pool_size += 3;
        }
        if (av_hwframe_ctx_init(requiredReference) < 0) {
            av_buffer_unref(&requiredReference);
            return;
        }

        context->hw_frames_ctx = requiredReference;
        requiredReference = nullptr;
        auto* retainedReference = av_buffer_ref(context->hw_frames_ctx);
        if (retainedReference) {
            av_buffer_unref(&decoder.reusableHardwareFramesContext);
            decoder.reusableHardwareFramesContext = retainedReference;
        }
    }

    void run()
    {
        while (!quitting_.load(std::memory_order_acquire)) {
            std::unique_lock<std::mutex> lock(mutex_);
            controlChanged_.wait(lock, [this] {
                return quitting_.load(std::memory_order_acquire)
                    || hasWorkLocked();
            });
            if (quitting_.load(std::memory_order_acquire)) {
                break;
            }

            if (appliedAudioSinkSerial_ != audioSinkSerial_) {
                const auto sink = audioSink_;
                const auto converter = audioFrameConverter_;
                const auto stretcher = audioTimeStretcher_;
                const auto processor = audioFrameProcessor_;
                const auto serial = audioSinkSerial_;
                lock.unlock();
                replaceAudioSink(
                    sink,
                    converter,
                    stretcher,
                    processor,
                    serial);
                continue;
            }

            if (requestedState_ == State::Stopped) {
                const bool needsStop =
                    hasOpenMedia_ || currentState_ != State::Stopped
                    || status_ == MediaStatus::Loading;
                lock.unlock();
                if (needsStop) {
                    stopPlayback(false);
                }
                continue;
            }

            if (url_.empty()) {
                lock.unlock();
                stopPlayback(false);
                continue;
            }

            if (!hasOpenMedia_ || loadedSerial_ != mediaSerial_) {
                const auto serial = mediaSerial_;
                const auto url = url_;
                const auto prepare = prepareRequest_;
                lock.unlock();
                openForPlayback(serial, url, prepare);
                continue;
            }

            if (seekCompletion_) {
                auto completion = std::move(*seekCompletion_);
                seekCompletion_.reset();
                lock.unlock();
                if (completion.callback) {
                    completion.callback(completion.position);
                }
                videoPacketChanged_.notify_all();
                audioPacketChanged_.notify_all();
                continue;
            }

            if (!trackSwitchRequests_.empty()) {
                const auto request = trackSwitchRequests_.front();
                trackSwitchRequests_.pop_front();
                lock.unlock();
                handleTrackSwitch(request);
                continue;
            }

            if (seekRequest_) {
                const auto request = std::move(*seekRequest_);
                seekRequest_.reset();
                lock.unlock();
                handleSeek(std::move(request));
                continue;
            }

            if (prepareRequest_) {
                const auto request = std::move(*prepareRequest_);
                prepareRequest_.reset();
                lock.unlock();
                handlePrepare(std::move(request));
                continue;
            }

            if (currentState_ != requestedState_) {
                const auto requested = requestedState_;
                const auto wasPlaying = currentState_ == State::Playing;
                lock.unlock();
                const auto transitionPosition =
                    wasPlaying ? position() : currentPosition();
                setAudioSinkPaused(requested != State::Playing);
                std::uint64_t packetBufferGeneration = 0;
                bool startPacketBuffer = false;
                {
                    std::lock_guard<std::mutex> stateLock(mutex_);
                    if (wasPlaying) {
                        currentPosition_ = transitionPosition;
                    }
                    if (requested == State::Playing) {
                        packetBufferGeneration = presentationGeneration_.load(
                            std::memory_order_acquire);
                        startPacketBuffer = true;
                        if (audioSinkOpen_ && audioSinkHasClock_) {
                            // A paused device clock cannot be extrapolated
                            // across resume. Allow the first queued output to
                            // prime the restarted sink, then wait for a fresh
                            // device-clock sample before pacing later frames.
                            primeOutputWaitLocked(
                                currentPosition_,
                                presentationGeneration_.load(
                                    std::memory_order_acquire),
                                false,
                                true);
                        } else if (!hasCurrentVideoFrame()
                                   && !audioSinkOpen_) {
                            primeOutputWaitLocked(
                                currentPosition_,
                                presentationGeneration_.load(
                                    std::memory_order_acquire),
                                media_.video.valid()
                                    && media_.audio.valid(),
                                false);
                        } else {
                            resetClockLocked(currentPosition_);
                        }
                    }
                }
                publishState(requested);
                const auto packetStreams = activePacketStreams();
                if (startPacketBuffer
                    && beginPacketBuffering(
                        PacketBufferingReason::InitialPlayback,
                        packetBufferGeneration,
                        packetStreams.first,
                        packetStreams.second)) {
                    beginOutputWait(
                        packetBufferGeneration,
                        transitionPosition);
                    updatePacketBuffering(packetBufferGeneration);
                }
                audioQueueChanged_.notify_all();
                audioQueueSpace_.notify_all();
                presentationChanged_.notify_all();
                continue;
            }

            const bool accuratePausedRead =
                currentState_ != State::Playing
                && accurateSeekNeedsInputLocked();
            if (currentState_ != State::Playing && !accuratePausedRead) {
                continue;
            }

            lock.unlock();
            const auto result = readAndDecodeOnePacket();
            const bool reachedRangeEnd =
                reachedRangeEnd_.exchange(
                    false,
                    std::memory_order_acq_rel);
            if (result == DecodeResult::End) {
                const auto generation = presentationGeneration_.load(
                    std::memory_order_acquire);
                const bool accurateFailed = failAccurateSeek(generation);
                if (!accuratePausedRead && !accurateFailed) {
                    handlePlaybackEnd();
                }
            } else if (reachedRangeEnd) {
                const auto generation =
                    presentationGeneration_.load(
                        std::memory_order_acquire);
                if (finishQueuedDecoding(generation)) {
                    reachedRangeEnd_.store(
                        false,
                        std::memory_order_release);
                    handlePlaybackEnd();
                }
            } else if (result == DecodeResult::Error) {
                const auto generation = presentationGeneration_.load(
                    std::memory_order_acquire);
                if (failAccurateSeek(generation)) {
                    publishEvent({
                        "seek.accurate",
                        "Accurate seek could not decode a target video frame",
                        AVERROR_INVALIDDATA,
                    });
                    continue;
                }
                bool controlPending = false;
                {
                    std::lock_guard<std::mutex> stateLock(mutex_);
                    controlPending = requestedState_ != State::Playing
                        || loadedSerial_ != mediaSerial_
                        || !trackSwitchRequests_.empty()
                        || seekRequest_.has_value()
                        || seekCompletion_.has_value();
                }
                if (!controlPending) {
                    stopPlayback(false, true);
                }
            }
        }
    }

    bool hasWorkLocked() const
    {
        if (appliedAudioSinkSerial_ != audioSinkSerial_) {
            return true;
        }
        if (requestedState_ == State::Stopped) {
            return hasOpenMedia_ || currentState_ != State::Stopped
                || status_ == MediaStatus::Loading
                || (url_.empty() && status_ != MediaStatus::NoMedia);
        }
        if (url_.empty()) {
            return currentState_ != State::Stopped
                || status_ != MediaStatus::NoMedia;
        }
        if (!hasOpenMedia_ || loadedSerial_ != mediaSerial_) {
            return true;
        }
        if (!trackSwitchRequests_.empty() || seekRequest_ || prepareRequest_
            || seekCompletion_ || accurateSeekNeedsInputLocked()
            || currentState_ != requestedState_) {
            return true;
        }
        return currentState_ == State::Playing;
    }

    int openInput(const std::string& inputUrl, AVFormatContext** output)
    {
        if (!output) {
            return AVERROR(EINVAL);
        }
        *output = nullptr;
        auto* format = avformat_alloc_context();
        if (!format) {
            return AVERROR(ENOMEM);
        }
        format->interrupt_callback = { &Impl::interruptCallback, &interrupt_ };

        AVDictionary* options = nullptr;
        applyHttpRecoveryDefaults(inputUrl, &options);
        {
            std::lock_guard<std::mutex> lock(mutex_);
            for (const auto& [key, value] : properties_) {
                constexpr const char prefix[] = "avformat.";
                if (key.compare(0, sizeof(prefix) - 1, prefix) == 0) {
                    av_dict_set(
                        &options,
                        key.c_str() + sizeof(prefix) - 1,
                        value.c_str(),
                        0);
                }
            }
        }

        int error = avformat_open_input(
            &format,
            inputUrl.c_str(),
            nullptr,
            &options);
        av_dict_free(&options);
        if (error >= 0) {
            error = avformat_find_stream_info(format, nullptr);
        }
        if (error < 0) {
            avformat_close_input(&format);
            return error;
        }
        *output = format;
        return 0;
    }

    AVFormatContext* formatForSource(InputSource source) const noexcept
    {
        switch (source) {
        case InputSource::Primary:
            return media_.format;
        case InputSource::ExternalAudio:
            return media_.externalAudio.format;
        case InputSource::ExternalSubtitle:
            return media_.externalSubtitle.format;
        }
        return nullptr;
    }

    std::int64_t startTimeForSource(InputSource source) const noexcept
    {
        switch (source) {
        case InputSource::Primary:
            return media_.startTimeUs;
        case InputSource::ExternalAudio:
            return media_.externalAudio.startTimeUs;
        case InputSource::ExternalSubtitle:
            return media_.externalSubtitle.startTimeUs;
        }
        return 0;
    }

    DemuxState& demuxForSource(InputSource source) noexcept
    {
        switch (source) {
        case InputSource::Primary:
            return media_.demux;
        case InputSource::ExternalAudio:
            return media_.externalAudio.demux;
        case InputSource::ExternalSubtitle:
            return media_.externalSubtitle.demux;
        }
        return media_.demux;
    }

    ReadRecoveryBudget& readRecoveryBudgetForSource(
        InputSource source) noexcept
    {
        switch (source) {
        case InputSource::Primary:
            return primaryReadRecoveryBudget_;
        case InputSource::ExternalAudio:
            return externalAudioReadRecoveryBudget_;
        case InputSource::ExternalSubtitle:
            return externalSubtitleReadRecoveryBudget_;
        }
        return primaryReadRecoveryBudget_;
    }

    std::int64_t& demuxProgressForSource(InputSource source) noexcept
    {
        switch (source) {
        case InputSource::Primary:
            return primaryDemuxProgress_;
        case InputSource::ExternalAudio:
            return externalAudioDemuxProgress_;
        case InputSource::ExternalSubtitle:
            return externalSubtitleDemuxProgress_;
        }
        return primaryDemuxProgress_;
    }

    void resetReadRecoveryTracking() noexcept
    {
        primaryReadRecoveryBudget_ = {};
        externalAudioReadRecoveryBudget_ = {};
        externalSubtitleReadRecoveryBudget_ = {};
        primaryDemuxProgress_ = std::numeric_limits<std::int64_t>::min();
        externalAudioDemuxProgress_ =
            std::numeric_limits<std::int64_t>::min();
        externalSubtitleDemuxProgress_ =
            std::numeric_limits<std::int64_t>::min();
    }

    AVFormatContext*& formatSlotForSource(InputSource source) noexcept
    {
        switch (source) {
        case InputSource::Primary:
            return media_.format;
        case InputSource::ExternalAudio:
            return media_.externalAudio.format;
        case InputSource::ExternalSubtitle:
            return media_.externalSubtitle.format;
        }
        return media_.format;
    }

    std::int64_t& startTimeSlotForSource(InputSource source) noexcept
    {
        switch (source) {
        case InputSource::Primary:
            return media_.startTimeUs;
        case InputSource::ExternalAudio:
            return media_.externalAudio.startTimeUs;
        case InputSource::ExternalSubtitle:
            return media_.externalSubtitle.startTimeUs;
        }
        return media_.startTimeUs;
    }

    std::string urlForSource(InputSource source) const
    {
        if (source == InputSource::ExternalAudio) {
            return media_.externalAudio.url;
        }
        if (source == InputSource::ExternalSubtitle) {
            return media_.externalSubtitle.url;
        }
        std::lock_guard<std::mutex> lock(mutex_);
        return url_;
    }

    static NetworkRecoveryInput publicRecoveryInput(
        InputSource source) noexcept
    {
        switch (source) {
        case InputSource::Primary:
            return NetworkRecoveryInput::Primary;
        case InputSource::ExternalAudio:
            return NetworkRecoveryInput::ExternalAudio;
        case InputSource::ExternalSubtitle:
            return NetworkRecoveryInput::ExternalSubtitle;
        }
        return NetworkRecoveryInput::Primary;
    }

    static const char* inputDescription(InputSource source) noexcept
    {
        switch (source) {
        case InputSource::Primary:
            return "main";
        case InputSource::ExternalAudio:
            return "external audio";
        case InputSource::ExternalSubtitle:
            return "external subtitle";
        }
        return "unknown";
    }

    static bool formatIsSeekable(const AVFormatContext* format) noexcept
    {
        return format
            && (!format->pb
                || (format->pb->seekable & AVIO_SEEKABLE_NORMAL) != 0);
    }

    bool networkRecoveryControlValid(
        std::uint64_t serial,
        NetworkRecoveryOperation operation) const
    {
        std::lock_guard<std::mutex> lock(mutex_);
        if (quitting_.load(std::memory_order_acquire)
            || serial != mediaSerial_
            || requestedState_ == State::Stopped) {
            return false;
        }
        if (operation == NetworkRecoveryOperation::Open) {
            return true;
        }
        return loadedSerial_ == mediaSerial_
            && currentState_ == State::Playing
            && requestedState_ == State::Playing
            && !seekRequest_ && !prepareRequest_
            && trackSwitchRequests_.empty();
    }

    bool waitForNetworkRetry(
        std::uint64_t serial,
        NetworkRecoveryOperation operation,
        std::int64_t delayMilliseconds)
    {
        if (delayMilliseconds <= 0) {
            return networkRecoveryControlValid(serial, operation);
        }
        std::unique_lock<std::mutex> lock(mutex_);
        const auto canceled = [this, serial, operation] {
            if (quitting_.load(std::memory_order_acquire)
                || serial != mediaSerial_
                || requestedState_ == State::Stopped) {
                return true;
            }
            return operation == NetworkRecoveryOperation::Read
                && (loadedSerial_ != mediaSerial_
                    || currentState_ != State::Playing
                    || requestedState_ != State::Playing || seekRequest_
                    || prepareRequest_ || !trackSwitchRequests_.empty());
        };
        return !controlChanged_.wait_for(
            lock,
            Milliseconds(delayMilliseconds),
            canceled);
    }

    static std::int64_t nextNetworkRetryDelay(
        std::int64_t current,
        std::int64_t maximum) noexcept
    {
        if (current <= 0 || current >= maximum) {
            return std::max<std::int64_t>(0, std::min(current, maximum));
        }
        if (current > maximum / 2) {
            return maximum;
        }
        return std::min(maximum, current * 2);
    }

    int openInputWithRecovery(
        const std::string& inputUrl,
        InputSource source,
        std::uint64_t serial,
        AVFormatContext** output)
    {
        int error = openInput(inputUrl, output);
        const auto policy = networkRecoveryPolicy();
        if (error >= 0 || !policy.enabled || !isNetworkUrl(inputUrl)
            || !isRecoverableNetworkError(error)) {
            return error;
        }

        auto delay = policy.initialRetryDelayMilliseconds;
        std::uint32_t attemptsConsumed = 0;
        for (std::uint32_t attempt = 1;
             attempt <= policy.maximumAttempts;
             ++attempt) {
            NetworkRecoveryStatus status;
            status.state = NetworkRecoveryState::Waiting;
            status.operation = NetworkRecoveryOperation::Open;
            status.input = publicRecoveryInput(source);
            status.url = inputUrl;
            status.attempt = attempt;
            status.maximumAttempts = policy.maximumAttempts;
            status.retryDelayMilliseconds = delay;
            status.error = error;
            status.presentationGeneration = presentationGeneration_.load(
                std::memory_order_acquire);
            publishNetworkRecoveryStatus(status);
            if (!waitForNetworkRetry(
                    serial,
                    NetworkRecoveryOperation::Open,
                    delay)) {
                publishNetworkRecoveryStatus({});
                return AVERROR_EXIT;
            }

            status.state = NetworkRecoveryState::Reopening;
            status.retryDelayMilliseconds = 0;
            publishNetworkRecoveryStatus(status);
            attemptsConsumed = attempt;
            networkRecoveryAttempts_.fetch_add(1, std::memory_order_relaxed);
            error = openInput(inputUrl, output);
            if (error >= 0) {
                status.state = NetworkRecoveryState::Recovered;
                status.error = 0;
                publishNetworkRecoveryStatus(status);
                successfulNetworkRecoveries_.fetch_add(
                    1,
                    std::memory_order_relaxed);
                publishEvent({
                    "network.recovered",
                    "Recovered while opening the "
                        + std::string(inputDescription(source))
                        + " network input on attempt "
                        + std::to_string(attempt),
                    0,
                });
                return 0;
            }
            if (!isRecoverableNetworkError(error)) {
                break;
            }
            delay = nextNetworkRetryDelay(
                delay,
                policy.maximumRetryDelayMilliseconds);
        }

        NetworkRecoveryStatus failed;
        failed.state = NetworkRecoveryState::Failed;
        failed.operation = NetworkRecoveryOperation::Open;
        failed.input = publicRecoveryInput(source);
        failed.url = inputUrl;
        failed.attempt = attemptsConsumed;
        failed.maximumAttempts = policy.maximumAttempts;
        failed.error = error;
        failed.presentationGeneration = presentationGeneration_.load(
            std::memory_order_acquire);
        publishNetworkRecoveryStatus(failed);
        failedNetworkRecoveries_.fetch_add(1, std::memory_order_relaxed);
        publishEvent({
            "network.recovery.failed",
            "Could not recover the "
                + std::string(inputDescription(source))
                + " network input while opening: " + ffmpegError(error),
            error,
        });
        return error;
    }

    const TrackSource* trackSource(int index, MediaType type) const noexcept
    {
        const auto found = std::find_if(
            media_.tracks.begin(),
            media_.tracks.end(),
            [index, type](const TrackSource& candidate) {
                return candidate.index == index && candidate.type == type;
            });
        return found == media_.tracks.end() ? nullptr : &*found;
    }

    void openForPlayback(
        std::uint64_t serial,
        const std::string& mediaUrl,
        std::optional<PrepareRequest> prepare)
    {
        resetPlaybackQueues();
        resetReadRecoveryTracking();
        closeAudioSink(true);
        closeVideoFrameProcessor();
        media_.reset();
        {
            std::lock_guard<std::mutex> lock(mutex_);
            hasOpenMedia_ = false;
            mediaInfo_ = {};
            currentPosition_ = 0;
            loopsCompleted_ = 0;
        }
        publishStatus(MediaStatus::Loading);

        interrupt_.owner = this;
        interrupt_.epoch = interruptEpoch_.load(std::memory_order_acquire);

        std::string externalAudioUrl;
        std::string externalSubtitleUrl;
        {
            std::lock_guard<std::mutex> lock(mutex_);
            externalAudioUrl = externalAudioUrl_;
            externalSubtitleUrl = externalSubtitleUrl_;
        }

        int error = openInputWithRecovery(
            mediaUrl,
            InputSource::Primary,
            serial,
            &media_.format);
        if (error < 0) {
            if (wasCanceled(serial)) {
                return;
            }
            failOpen(error, "Could not open media '" + mediaUrl + "'");
            return;
        }
        media_.startTimeUs =
            media_.format->start_time == AV_NOPTS_VALUE
            ? 0
            : media_.format->start_time;

        const auto openExternal = [&](ExternalInput& input,
                                      const std::string& inputUrl,
                                      AVMediaType expectedType,
                                      const char* description) {
            if (inputUrl.empty()) {
                return 0;
            }
            const auto source = expectedType == AVMEDIA_TYPE_AUDIO
                ? InputSource::ExternalAudio
                : InputSource::ExternalSubtitle;
            const int openError = openInputWithRecovery(
                inputUrl,
                source,
                serial,
                &input.format);
            if (openError < 0) {
                return openError;
            }
            input.url = inputUrl;
            input.startTimeUs =
                input.format->start_time == AV_NOPTS_VALUE
                ? 0
                : input.format->start_time;
            const AVCodec* decoder = nullptr;
            const int stream = av_find_best_stream(
                input.format,
                expectedType,
                -1,
                -1,
                &decoder,
                0);
            if (stream < 0 || !decoder) {
                publishEvent({
                    "external.error",
                    std::string(description) + " '" + inputUrl
                        + "' contains no supported "
                        + (expectedType == AVMEDIA_TYPE_AUDIO
                                ? "audio"
                                : "subtitle")
                        + " track",
                    stream,
                });
                return stream;
            }
            return 0;
        };

        error = openExternal(
            media_.externalAudio,
            externalAudioUrl,
            AVMEDIA_TYPE_AUDIO,
            "External audio input");
        if (error >= 0) {
            error = openExternal(
                media_.externalSubtitle,
                externalSubtitleUrl,
                AVMEDIA_TYPE_SUBTITLE,
                "External subtitle input");
        }
        if (error < 0) {
            if (wasCanceled(serial)) {
                media_.reset();
                return;
            }
            failOpen(error, "Could not open the configured external media");
            media_.reset();
            return;
        }

        auto info = buildMediaInfo(mediaUrl);

        HardwareDecodeConfig hardwareDecodeConfig;
        {
            std::lock_guard<std::mutex> lock(mutex_);
            hardwareDecodeConfig = hardwareDecodeConfig_;
        }
        openBestDecoder(
            InputSource::Primary,
            AVMEDIA_TYPE_VIDEO,
            media_.video,
            hardwareDecodeConfig);
        if (!openBestDecoder(
                InputSource::Primary,
                AVMEDIA_TYPE_AUDIO,
                media_.audio)) {
            openBestDecoder(
                InputSource::ExternalAudio,
                AVMEDIA_TYPE_AUDIO,
                media_.audio);
        }
        if (!openBestDecoder(
                InputSource::Primary,
                AVMEDIA_TYPE_SUBTITLE,
                media_.subtitle)) {
            openBestDecoder(
                InputSource::ExternalSubtitle,
                AVMEDIA_TYPE_SUBTITLE,
                media_.subtitle);
        }
        if (!media_.video.valid() && !media_.audio.valid()) {
            failOpen(
                AVERROR_DECODER_NOT_FOUND,
                "No supported audio or video decoder was found");
            media_.reset();
            return;
        }

        info.activeVideoTrack = media_.video.trackIndex;
        info.activeAudioTrack = media_.audio.trackIndex;
        info.activeSubtitleTrack = media_.subtitle.trackIndex;

        {
            std::lock_guard<std::mutex> lock(mutex_);
            if (serial != mediaSerial_ || requestedState_ == State::Stopped) {
                media_.reset();
                return;
            }
            mediaInfo_ = info;
            hasOpenMedia_ = true;
            loadedSerial_ = serial;
            currentPosition_ = prepare ? prepare->position : rangeStart_;
            resetClockLocked(currentPosition_);
            if (prepareRequest_ && prepare
                && prepareRequest_->id == prepare->id) {
                prepareRequest_.reset();
            }
        }

        std::int64_t preparedPosition = currentPosition();
        if (preparedPosition > 0) {
            const int seekError =
                seekMedia(preparedPosition, prepare ? prepare->flags
                                                    : SeekFlag::FromStart);
            if (seekError < 0) {
                publishEvent({
                    "seek.error",
                    "Initial seek failed: " + ffmpegError(seekError),
                    seekError,
                });
                preparedPosition = -1;
            }
        }

        publishStatus(MediaStatus::Loaded);
        State requested;
        std::uint64_t packetBufferGeneration = 0;
        {
            std::lock_guard<std::mutex> lock(mutex_);
            requested = requestedState_;
            if (requested == State::Playing) {
                packetBufferGeneration = presentationGeneration_.load(
                    std::memory_order_acquire);
                const auto position =
                    std::max<std::int64_t>(0, preparedPosition);
                primeOutputWaitLocked(
                    position,
                    presentationGeneration_.load(
                        std::memory_order_acquire),
                    media_.video.valid() && media_.audio.valid(),
                    false);
            }
        }
        publishState(requested);
        const auto packetStreams = activePacketStreams();
        if (requested == State::Playing
            && beginPacketBuffering(
                PacketBufferingReason::InitialPlayback,
                packetBufferGeneration,
                packetStreams.first,
                packetStreams.second)) {
            updatePacketBuffering(packetBufferGeneration);
        }

        if (prepare && preparedPosition >= 0
            && hasFlag(prepare->flags, SeekFlag::Accurate)) {
            handlePrepare(std::move(*prepare));
            return;
        }
        if (prepare && prepare->callback) {
            bool boost = true;
            prepare->callback(preparedPosition, &boost);
        }
    }

    bool openBestDecoder(
        InputSource source,
        AVMediaType type,
        Decoder& result,
        HardwareDecodeConfig hardwareDecodeConfig = {},
        int requestedStreamIndex = -1,
        int requestedTrackIndex = -1)
    {
        result.reset();
        auto* format = formatForSource(source);
        if (!format) {
            return false;
        }
        const AVCodec* softwareDecoder = nullptr;
        int streamIndex = requestedStreamIndex;
        if (requestedStreamIndex >= 0) {
            if (static_cast<unsigned>(requestedStreamIndex)
                    >= format->nb_streams
                || format->streams[requestedStreamIndex]
                        ->codecpar->codec_type
                    != type) {
                return false;
            }
            softwareDecoder = avcodec_find_decoder(
                format->streams[requestedStreamIndex]
                    ->codecpar->codec_id);
        } else {
            streamIndex = av_find_best_stream(
                format,
                type,
                -1,
                -1,
                &softwareDecoder,
                0);
        }
        if (streamIndex < 0 || !softwareDecoder) {
            return false;
        }

        auto* stream = format->streams[streamIndex];
        const auto requestedDevice =
            type == AVMEDIA_TYPE_VIDEO
            ? hardwareDecodeConfig.deviceType
            : HardwareDeviceType::Unknown;
        const auto ffmpegDevice = ffmpegHardwareDeviceType(requestedDevice);
        const bool suppliedDeviceMismatch =
            hardwareDecodeConfig.device
            && hardwareDecodeConfig.device.deviceType() != requestedDevice;
        const bool requiredDeviceMissing =
            hardwareDecodeConfig.requireSuppliedDevice
            && !hardwareDecodeConfig.device;
        const AVCodec* hardwareDecoder = softwareDecoder;
        if (requestedDevice != HardwareDeviceType::Unknown
            && !hardwareDecodeConfig.decoderWrapper.empty()) {
            hardwareDecoder = decoderForWrapper(
                stream->codecpar->codec_id,
                type,
                hardwareDecodeConfig.decoderWrapper);
        }
        const AVCodecHWConfig* selectedHardwareConfig = nullptr;
        if (ffmpegDevice != AV_HWDEVICE_TYPE_NONE && hardwareDecoder) {
            for (int index = 0;; ++index) {
                const auto* candidate =
                    avcodec_get_hw_config(hardwareDecoder, index);
                if (!candidate) {
                    break;
                }
                if (candidate->device_type == ffmpegDevice
                    && (candidate->methods
                        & AV_CODEC_HW_CONFIG_METHOD_HW_DEVICE_CTX)
                        != 0) {
                    selectedHardwareConfig = candidate;
                    break;
                }
            }
        }

        auto openContext =
            [&](const AVCodec* decoder, bool hardware, int& error) {
            if (!decoder) {
                error = AVERROR_DECODER_NOT_FOUND;
                return static_cast<AVCodecContext*>(nullptr);
            }
            auto* context = avcodec_alloc_context3(decoder);
            if (!context) {
                error = AVERROR(ENOMEM);
                return static_cast<AVCodecContext*>(nullptr);
            }
            error =
                avcodec_parameters_to_context(context, stream->codecpar);
            if (error >= 0) {
                context->pkt_timebase = stream->time_base;
            }
            if (error >= 0 && !hardware && type == AVMEDIA_TYPE_VIDEO) {
                // libavcodec defaults to a single decode thread. Match the
                // legacy QtAV software decoder's "threads = 0" policy so
                // codecs with frame or slice threading can select an
                // appropriate worker count for the host.
                context->thread_count = 0;
                context->thread_type = FF_THREAD_FRAME | FF_THREAD_SLICE;
            }
            if (error >= 0 && hardware) {
                result.hardwareDeviceType = requestedDevice;
                result.hardwarePixelFormat =
                    selectedHardwareConfig->pix_fmt;
                result.hardwareNativeIdentity =
                    hardwareDecodeConfig.device.nativeIdentity();
                result.hardwareSurfaceGeneration =
                    hardwareDecodeConfig.surfaceGeneration;
                result.allowSoftwareFallback =
                    hardwareDecodeConfig.allowSoftwareFallback;
                context->opaque = &result;
                context->get_format = &Impl::selectHardwarePixelFormat;
                context->extra_hw_frames = std::clamp(
                    hardwareDecodeConfig.extraHardwareFrames,
                    0,
                    64);
                if (hardwareDecodeConfig.device) {
                    context->hw_device_ctx =
                        detail::HardwareDecodeDevicePrivate::contextRef(
                            hardwareDecodeConfig.device);
                    if (!context->hw_device_ctx) {
                        error = AVERROR(ENOMEM);
                    }
                } else {
                    error = av_hwdevice_ctx_create(
                        &context->hw_device_ctx,
                        ffmpegDevice,
                        nullptr,
                        nullptr,
                        0);
                }
            }
            if (error >= 0) {
                error = avcodec_open2(context, decoder, nullptr);
            }
            if (error < 0) {
                avcodec_free_context(&context);
            }
            return context;
        };

        int error = 0;
        AVCodecContext* context = nullptr;
        if (requestedDevice != HardwareDeviceType::Unknown) {
            if (suppliedDeviceMismatch || requiredDeviceMissing) {
                error = AVERROR(EINVAL);
            } else if (!hardwareDecoder
                || ffmpegDevice == AV_HWDEVICE_TYPE_NONE
                || !selectedHardwareConfig) {
                error = AVERROR(ENOSYS);
            } else {
                context = openContext(hardwareDecoder, true, error);
            }
            if (!context && hardwareDecodeConfig.allowSoftwareFallback) {
                const std::string requestedDecoder =
                    hardwareDecoder && hardwareDecoder->name
                    ? hardwareDecoder->name
                    : hardwareDecodeConfig.decoderWrapper.empty()
                        ? softwareDecoder->name
                        : hardwareDecodeConfig.decoderWrapper;
                publishEvent({
                    "decoder.hardware.fallback",
                    "Hardware decode is unavailable for decoder '"
                        + requestedDecoder
                        + "'; using software decode: "
                        + ffmpegError(error),
                    error,
                });
                result.reset();
                context = openContext(softwareDecoder, false, error);
            }
        } else {
            context = openContext(softwareDecoder, false, error);
        }

        if (!context) {
            const std::string requestedDecoder =
                hardwareDecoder && hardwareDecoder->name
                ? hardwareDecoder->name
                : hardwareDecodeConfig.decoderWrapper.empty()
                    ? softwareDecoder->name
                    : hardwareDecodeConfig.decoderWrapper;
            publishEvent({
                requestedDevice == HardwareDeviceType::Unknown
                    ? "decoder.error"
                    : "decoder.hardware.error",
                "Could not open "
                    + std::string(
                        requestedDevice == HardwareDeviceType::Unknown
                            ? ""
                            : "the requested hardware path for ")
                    + "decoder '" + requestedDecoder + "': "
                    + ffmpegError(error),
                error,
            });
            return false;
        }

        result.contextLifetime = std::shared_ptr<AVCodecContext>(
            context,
            [](AVCodecContext* ownedContext) {
                avcodec_free_context(&ownedContext);
            });
        result.context = result.contextLifetime.get();
        result.stream = stream;
        result.streamIndex = streamIndex;
        result.source = source;
        result.startTimeUs = startTimeForSource(source);
        if (requestedTrackIndex >= 0) {
            result.trackIndex = requestedTrackIndex;
        } else {
            const auto found = std::find_if(
                media_.tracks.begin(),
                media_.tracks.end(),
                [source, streamIndex, type](const TrackSource& candidate) {
                    return candidate.source == source
                        && candidate.streamIndex == streamIndex
                        && candidate.type == mediaTypeFromFFmpeg(type);
                });
            if (found == media_.tracks.end()) {
                result.reset();
                return false;
            }
            result.trackIndex = found->index;
        }
        result.type = mediaTypeFromFFmpeg(type);
        return true;
    }

    MediaInfo buildMediaInfo(const std::string& mediaUrl)
    {
        MediaInfo result;
        result.url = mediaUrl;
        result.startTime =
            media_.format->start_time == AV_NOPTS_VALUE
            ? 0
            : media_.format->start_time / 1000;
        result.duration =
            media_.format->duration == AV_NOPTS_VALUE
            ? 0
            : media_.format->duration / 1000;
        result.seekable =
            !media_.format->pb
            || (media_.format->pb->seekable & AVIO_SEEKABLE_NORMAL) != 0;
        const auto externalAudioStreams = media_.externalAudio.format
            ? media_.externalAudio.format->nb_streams
            : 0;
        const auto externalSubtitleStreams = media_.externalSubtitle.format
            ? media_.externalSubtitle.format->nb_streams
            : 0;
        result.tracks.reserve(
            media_.format->nb_streams + externalAudioStreams
            + externalSubtitleStreams);
        media_.tracks.clear();
        media_.tracks.reserve(result.tracks.capacity());

        const auto appendTrack = [&](AVStream* stream,
                                     int index,
                                     int streamIndex,
                                     InputSource source,
                                     const std::string& sourceUrl,
                                     bool external) {
            const auto* parameters = stream->codecpar;
            TrackInfo track;
            track.index = index;
            track.streamIndex = streamIndex;
            track.type = mediaTypeFromFFmpeg(parameters->codec_type);
            track.sourceUrl = sourceUrl;
            track.external = external;
            track.codec = avcodec_get_name(parameters->codec_id);
            if (const auto* descriptor =
                    avcodec_descriptor_get(parameters->codec_id)) {
                track.codecDescription =
                    descriptor->long_name ? descriptor->long_name : "";
            }
            track.language = dictionaryValue(stream->metadata, "language");
            track.title = dictionaryValue(stream->metadata, "title");
            track.bitRate = parameters->bit_rate;
            track.width = parameters->width;
            track.height = parameters->height;
            track.sampleRate = parameters->sample_rate;
            track.channels = parameters->ch_layout.nb_channels;
            media_.tracks.push_back({
                track.index,
                track.type,
                source,
                track.streamIndex,
            });
            result.tracks.push_back(std::move(track));
        };

        for (unsigned index = 0; index < media_.format->nb_streams; ++index) {
            appendTrack(
                media_.format->streams[index],
                static_cast<int>(index),
                static_cast<int>(index),
                InputSource::Primary,
                mediaUrl,
                false);
        }

        int nextIndex = static_cast<int>(media_.format->nb_streams);
        const auto appendExternal = [&](const ExternalInput& input,
                                        InputSource source,
                                        MediaType expectedType) {
            if (!input.format) {
                return;
            }
            for (unsigned streamIndex = 0;
                 streamIndex < input.format->nb_streams;
                 ++streamIndex) {
                auto* stream = input.format->streams[streamIndex];
                if (mediaTypeFromFFmpeg(stream->codecpar->codec_type)
                    != expectedType) {
                    continue;
                }
                appendTrack(
                    stream,
                    nextIndex++,
                    static_cast<int>(streamIndex),
                    source,
                    input.url,
                    true);
            }
        };
        appendExternal(
            media_.externalAudio,
            InputSource::ExternalAudio,
            MediaType::Audio);
        appendExternal(
            media_.externalSubtitle,
            InputSource::ExternalSubtitle,
            MediaType::Subtitle);
        return result;
    }

    void handleTrackSwitch(const TrackSwitchRequest& request)
    {
        const auto typeName = [](MediaType type) {
            switch (type) {
            case MediaType::Audio:
                return "audio";
            case MediaType::Video:
                return "video";
            case MediaType::Subtitle:
                return "subtitle";
            case MediaType::Unknown:
                break;
            }
            return "unknown";
        };
        {
            std::lock_guard<std::mutex> lock(mutex_);
            if (!hasOpenMedia_ || request.mediaSerial != mediaSerial_
                || loadedSerial_ != mediaSerial_
                || requestedState_ == State::Stopped) {
                return;
            }
            int active = mediaInfo_.activeSubtitleTrack;
            if (request.type == MediaType::Audio) {
                active = mediaInfo_.activeAudioTrack;
            } else if (request.type == MediaType::Video) {
                active = mediaInfo_.activeVideoTrack;
            }
            if (active == request.track) {
                return;
            }
        }

        Decoder replacement;
        if (request.track >= 0) {
            const auto* selected = trackSource(request.track, request.type);
            if (!selected) {
                publishEvent({
                    "track.error",
                    "Could not resolve "
                        + std::string(typeName(request.type)) + " track "
                        + std::to_string(request.track),
                    AVERROR_STREAM_NOT_FOUND,
                });
                return;
            }
            HardwareDecodeConfig hardwareDecodeConfig;
            if (request.type == MediaType::Video) {
                std::lock_guard<std::mutex> lock(mutex_);
                hardwareDecodeConfig = hardwareDecodeConfig_;
            }
            AVMediaType ffmpegType = AVMEDIA_TYPE_SUBTITLE;
            if (request.type == MediaType::Audio) {
                ffmpegType = AVMEDIA_TYPE_AUDIO;
            } else if (request.type == MediaType::Video) {
                ffmpegType = AVMEDIA_TYPE_VIDEO;
            }
            if (!openBestDecoder(
                    selected->source,
                    ffmpegType,
                    replacement,
                    hardwareDecodeConfig,
                    selected->streamIndex,
                    selected->index)) {
                publishEvent({
                    "track.error",
                    "Could not open " + std::string(typeName(request.type))
                        + " track " + std::to_string(request.track),
                    AVERROR_DECODER_NOT_FOUND,
                });
                // The public call already invalidated queued output. Re-seek
                // the unchanged decoders so playback resumes coherently.
                if (mediaInfo().seekable) {
                    seekMedia(
                        request.position,
                        SeekFlag::KeyFrame,
                        PacketBufferingReason::TrackSwitch);
                }
                return;
            }
        }

        if (wasCanceled(request.mediaSerial)) {
            return;
        }

        resetPlaybackQueues();
        resetReadRecoveryTracking();
        // A replacement audio track can have a different native format. Close
        // rather than merely flush so the next frame renegotiates both the
        // sink and optional converter. This is also safe for video switches
        // and keeps a single A/V generation boundary.
        closeAudioSink(true);
        if (request.type == MediaType::Video) {
            closeVideoFrameProcessor();
        }

        if (request.type == MediaType::Audio) {
            media_.audio = std::move(replacement);
        } else if (request.type == MediaType::Video) {
            media_.video = std::move(replacement);
        } else {
            media_.subtitle = std::move(replacement);
        }

        bool seekable = false;
        {
            std::lock_guard<std::mutex> lock(mutex_);
            if (!hasOpenMedia_ || request.mediaSerial != mediaSerial_
                || loadedSerial_ != mediaSerial_) {
                return;
            }
            if (request.type == MediaType::Audio) {
                mediaInfo_.activeAudioTrack = request.track;
            } else if (request.type == MediaType::Video) {
                mediaInfo_.activeVideoTrack = request.track;
            } else {
                mediaInfo_.activeSubtitleTrack = request.track;
            }
            seekable = mediaInfo_.seekable;
        }

        int seekError = 0;
        if (seekable) {
            seekError = seekMedia(
                request.position,
                SeekFlag::KeyFrame,
                PacketBufferingReason::TrackSwitch);
        } else {
            media_.demux.reset();
            media_.externalAudio.resetDemux();
            media_.externalSubtitle.resetDemux();
            if (media_.video.valid()) {
                avcodec_flush_buffers(media_.video.context);
            }
            if (media_.audio.valid()) {
                avcodec_flush_buffers(media_.audio.context);
            }
            if (media_.subtitle.valid()) {
                avcodec_flush_buffers(media_.subtitle.context);
            }

            bool publishBuffering = false;
            {
                std::lock_guard<std::mutex> lock(mutex_);
                currentPosition_ = request.position;
                resetClockLocked(request.position);
                if (currentState_ == State::Playing
                    && requestedState_ == State::Playing
                    && (media_.video.valid() || media_.audio.valid())) {
                    primeOutputWaitLocked(
                        request.position,
                        presentationGeneration_.load(
                            std::memory_order_acquire),
                        media_.video.valid() && media_.audio.valid(),
                        false);
                    publishBuffering = status_ == MediaStatus::Loaded;
                }
            }
            if (publishBuffering) {
                publishStatus(MediaStatus::Buffering);
            }
            const auto generation = presentationGeneration_.load(
                std::memory_order_acquire);
            const auto packetStreams = activePacketStreams();
            if (beginPacketBuffering(
                    PacketBufferingReason::TrackSwitch,
                    generation,
                    packetStreams.first,
                    packetStreams.second)) {
                updatePacketBuffering(generation);
            }
        }

        if (seekError < 0) {
            publishEvent({
                "track.seek",
                "The track changed, but the playback position could not be "
                "restored: "
                    + ffmpegError(seekError),
                seekError,
            });
        }
        publishEvent({
            "track.changed",
            std::string(typeName(request.type))
                + (request.track >= 0
                        ? " track changed to "
                            + std::to_string(request.track)
                        : " tracks disabled"),
            0,
        });
    }

    void handleSeek(SeekRequest request)
    {
        const int error = seekMedia(request.position, request.flags);
        if (error < 0) {
            publishEvent({
                "seek.error",
                "Seek failed: " + ffmpegError(error),
                error,
            });
            if (request.callback) {
                request.callback(-1);
            }
            return;
        }

        if (request.kind == SeekRequest::Kind::Normal
            || !media_.video.valid()) {
            if (request.callback) {
                request.callback(request.position);
            }
            return;
        }

        const auto generation = presentationGeneration_.load(
            std::memory_order_acquire);
        bool pauseAfter = true;
        {
            std::lock_guard<std::mutex> lock(mutex_);
            pauseAfter = request.kind != SeekRequest::Kind::Accurate
                || requestedState_ != State::Playing;
        }
        if (pauseAfter) {
            setAudioSinkPaused(true);
        }
        {
            std::lock_guard<std::mutex> lock(mutex_);
            currentPosition_ = clampPositionLocked(request.selectionTarget);
            resetClockLocked(currentPosition_);
            lastPresentedVideoTimestamp_ = -1;
            previousPresentedVideoTimestamp_ = -1;
            accurateSeek_ = AccurateSeekState {
                generation,
                request.selectionTarget,
                request.kind,
                pauseAfter,
                false,
                {},
                {},
                std::move(request.callback),
            };
            accuratePresentationFloorGeneration_ = generation;
            accuratePresentationFloor_ = request.selectionTarget;
        }
        controlChanged_.notify_all();
        videoPacketChanged_.notify_all();
        audioPacketChanged_.notify_all();
    }

    void handlePrepare(PrepareRequest request)
    {
        if (hasFlag(request.flags, SeekFlag::Accurate)) {
            auto callback = std::move(request.callback);
            handleSeek(SeekRequest {
                request.id,
                request.position,
                request.position,
                request.flags,
                SeekRequest::Kind::Accurate,
                [callback = std::move(callback)](
                    std::int64_t position) mutable {
                    if (callback) {
                        bool boost = position >= 0;
                        callback(position, &boost);
                    }
                },
            });
            setAudioSinkPaused(true);
            publishState(State::Paused);
            return;
        }

        const int error = seekMedia(request.position, request.flags);
        const auto result = error < 0 ? static_cast<std::int64_t>(-1)
                                     : request.position;
        if (error < 0) {
            publishEvent({
                "prepare.error",
                "Prepare seek failed: " + ffmpegError(error),
                error,
            });
        }
        setAudioSinkPaused(true);
        publishState(State::Paused);
        if (request.callback) {
            bool boost = error >= 0;
            request.callback(result, &boost);
        }
    }

    bool accurateSeekActive(std::uint64_t generation) const
    {
        std::lock_guard<std::mutex> lock(mutex_);
        return accurateSeek_
            && accurateSeek_->generation == generation;
    }

    bool accurateSelectionShouldPresentImmediately(
        std::uint64_t generation) const
    {
        std::lock_guard<std::mutex> lock(mutex_);
        return accurateSeek_ && accurateSeek_->generation == generation
            && accurateSeek_->pauseAfter;
    }

    bool accurateSeekNeedsInputLocked() const
    {
        return accurateSeek_
            && accurateSeek_->generation
                == presentationGeneration_.load(std::memory_order_acquire)
            && !accurateSeek_->selectionQueued;
    }

    bool shouldDropNonVideoForAccurateSeek(
        std::int64_t timestamp,
        std::uint64_t generation) const
    {
        std::lock_guard<std::mutex> lock(mutex_);
        if (accuratePresentationFloorGeneration_ == generation
            && timestamp < accuratePresentationFloor_) {
            return true;
        }
        return accurateSeek_ && accurateSeek_->generation == generation
            && accurateSeek_->pauseAfter;
    }

    AccurateVideoDecision classifyAccurateVideoFrame(
        VideoFrame frame,
        std::uint64_t generation)
    {
        std::lock_guard<std::mutex> lock(mutex_);
        if (!accurateSeek_ || accurateSeek_->generation != generation) {
            return {
                AccurateVideoDecision::Action::DeliverNormally,
                std::move(frame),
                false,
                -1,
            };
        }

        auto& accurate = *accurateSeek_;
        if (accurate.selectionQueued) {
            return {
                accurate.pauseAfter
                    ? AccurateVideoDecision::Action::Drop
                    : AccurateVideoDecision::Action::DeliverNormally,
                accurate.pauseAfter ? VideoFrame {} : std::move(frame),
                false,
                -1,
            };
        }

        const auto timestamp = frame.timestamp();
        bool selected = false;
        VideoFrame selectedFrame;
        switch (accurate.kind) {
        case SeekRequest::Kind::Accurate:
        case SeekRequest::Kind::StepBackwardExact:
            selected = timestamp >= accurate.target;
            break;
        case SeekRequest::Kind::StepForward:
            selected = timestamp > accurate.target;
            break;
        case SeekRequest::Kind::StepBackwardScan:
            if (timestamp >= accurate.target
                && accurate.previousCandidate) {
                selected = true;
                selectedFrame = accurate.previousCandidate;
            }
            break;
        case SeekRequest::Kind::Normal:
            break;
        }

        if (!selected) {
            if (timestamp < accurate.target
                || accurate.kind == SeekRequest::Kind::StepForward) {
                accurate.candidateBeforePrevious =
                    std::move(accurate.previousCandidate);
                accurate.previousCandidate = std::move(frame);
            }
            return {
                AccurateVideoDecision::Action::Drop,
                {},
                false,
                -1,
            };
        }

        const auto& previousFrame =
            accurate.kind == SeekRequest::Kind::StepBackwardScan
            ? accurate.candidateBeforePrevious
            : accurate.previousCandidate;
        const auto previousTimestamp = previousFrame
            ? previousFrame.timestamp()
            : static_cast<std::int64_t>(-1);
        if (!selectedFrame) {
            selectedFrame = std::move(frame);
        }
        accurate.selectionQueued = true;
        accurate.previousCandidate = {};
        accurate.candidateBeforePrevious = {};
        return {
            AccurateVideoDecision::Action::DeliverSelected,
            std::move(selectedFrame),
            true,
            previousTimestamp,
        };
    }

    AccurateVideoDecision finishAccurateVideoAtEnd(
        std::uint64_t generation)
    {
        std::lock_guard<std::mutex> lock(mutex_);
        if (!accurateSeek_ || accurateSeek_->generation != generation
            || accurateSeek_->selectionQueued
            || !accurateSeek_->previousCandidate
            || (accurateSeek_->kind != SeekRequest::Kind::Accurate
                && accurateSeek_->kind
                    != SeekRequest::Kind::StepBackwardScan)) {
            return {
                AccurateVideoDecision::Action::Drop,
                {},
                false,
                -1,
            };
        }

        auto frame = std::move(accurateSeek_->previousCandidate);
        const auto previousTimestamp =
            accurateSeek_->candidateBeforePrevious
            ? accurateSeek_->candidateBeforePrevious.timestamp()
            : static_cast<std::int64_t>(-1);
        accurateSeek_->candidateBeforePrevious = {};
        accurateSeek_->selectionQueued = true;
        return {
            AccurateVideoDecision::Action::DeliverSelected,
            std::move(frame),
            true,
            previousTimestamp,
        };
    }

    void completeAccurateSeek(
        std::uint64_t generation,
        std::int64_t position)
    {
        {
            std::lock_guard<std::mutex> lock(mutex_);
            if (!accurateSeek_ || accurateSeek_->generation != generation
                || !accurateSeek_->selectionQueued) {
                return;
            }
            if (accurateSeek_->callback) {
                seekCompletion_ = SeekCompletion {
                    position,
                    std::move(accurateSeek_->callback),
                };
            }
            accurateSeek_.reset();
        }
        controlChanged_.notify_all();
        videoPacketChanged_.notify_all();
        audioPacketChanged_.notify_all();
    }

    bool failAccurateSeek(std::uint64_t generation)
    {
        {
            std::lock_guard<std::mutex> lock(mutex_);
            if (!accurateSeek_ || accurateSeek_->generation != generation
                || accurateSeek_->selectionQueued) {
                return false;
            }
            if (accurateSeek_->callback) {
                seekCompletion_ = SeekCompletion {
                    -1,
                    std::move(accurateSeek_->callback),
                };
            }
            accurateSeek_.reset();
        }
        controlChanged_.notify_all();
        videoPacketChanged_.notify_all();
        audioPacketChanged_.notify_all();
        return true;
    }

    int seekMedia(
        std::int64_t targetMs,
        SeekFlag flags,
        PacketBufferingReason bufferingReason =
            PacketBufferingReason::Seek)
    {
        if (!media_.format) {
            return AVERROR(EINVAL);
        }

        const auto previousPosition = position();
        resetPlaybackQueues();
        resetReadRecoveryTracking();
        flushAudioSink();
        bool waitForData = false;
        {
            std::lock_guard<std::mutex> lock(mutex_);
            currentPosition_ = targetMs;
            lastPresentedVideoTimestamp_ = -1;
            previousPresentedVideoTimestamp_ = -1;
            resetClockLocked(targetMs);
            waitForData = currentState_ == State::Playing
                && requestedState_ == State::Playing
                && (media_.video.valid() || media_.audio.valid());
            if (waitForData) {
                primeOutputWaitLocked(
                    targetMs,
                    presentationGeneration_.load(
                        std::memory_order_acquire),
                    media_.video.valid() && media_.audio.valid(),
                    audioSinkOpen_ && audioSinkHasClock_);
            }
        }
        if (waitForData) {
            publishStatus(MediaStatus::Buffering);
        }

        interrupt_.epoch = interruptEpoch_.load(std::memory_order_acquire);
        int ffmpegFlags = AVSEEK_FLAG_BACKWARD;
        if (!hasFlag(flags, SeekFlag::Accurate)) {
            if (hasFlag(flags, SeekFlag::AnyFrame)) {
                ffmpegFlags |= AVSEEK_FLAG_ANY;
            }
            if (!hasFlag(flags, SeekFlag::KeyFrame)) {
                ffmpegFlags &= ~AVSEEK_FLAG_BACKWARD;
            }
        }

        media_.demux.reset();
        media_.externalAudio.resetDemux();
        media_.externalSubtitle.resetDemux();

        const auto sourceIsActive = [&](InputSource source) {
            return (media_.video.valid() && media_.video.source == source)
                || (media_.audio.valid() && media_.audio.source == source)
                || (media_.subtitle.valid()
                    && media_.subtitle.source == source);
        };
        const auto seekSource = [&](InputSource source) {
            auto* format = formatForSource(source);
            if (!format) {
                return AVERROR(EINVAL);
            }
            const auto timestamp = startTimeForSource(source)
                + targetMs * static_cast<std::int64_t>(1000);
            return avformat_seek_file(
                format,
                -1,
                std::numeric_limits<std::int64_t>::min(),
                timestamp,
                std::numeric_limits<std::int64_t>::max(),
                ffmpegFlags);
        };

        int error = seekSource(InputSource::Primary);
        if (error >= 0 && sourceIsActive(InputSource::ExternalAudio)) {
            error = seekSource(InputSource::ExternalAudio);
        }
        if (error >= 0 && sourceIsActive(InputSource::ExternalSubtitle)) {
            error = seekSource(InputSource::ExternalSubtitle);
        }
        if (error < 0) {
            bool publishLoaded = false;
            {
                std::lock_guard<std::mutex> lock(mutex_);
                const bool superseded = seekRequest_.has_value();
                waitingForOutput_ = false;
                outputWaitPrimed_ = false;
                outputWaitGeneration_ = 0;
                outputWaitRequiresDeviceClock_ = false;
                if (!superseded) {
                    currentPosition_ = previousPosition;
                    resetClockLocked(previousPosition);
                    publishLoaded = status_ == MediaStatus::Buffering;
                }
            }
            if (publishLoaded) {
                publishStatus(MediaStatus::Loaded);
            }
            return error;
        }
        if (media_.video.valid()) {
            avcodec_flush_buffers(media_.video.context);
        }
        if (media_.audio.valid()) {
            avcodec_flush_buffers(media_.audio.context);
        }
        if (media_.subtitle.valid()) {
            avcodec_flush_buffers(media_.subtitle.context);
        }
        if (waitForData) {
            const auto generation = presentationGeneration_.load(
                std::memory_order_acquire);
            const auto packetStreams = activePacketStreams();
            if (beginPacketBuffering(
                    bufferingReason,
                    generation,
                    packetStreams.first,
                    packetStreams.second)) {
                updatePacketBuffering(generation);
            }
        }
        return 0;
    }

    enum class DecodeResult {
        Continue,
        End,
        Error,
    };

    enum class InputReadResult {
        Ready,
        End,
        Recovered,
        Error,
    };

    bool sourceIsActive(InputSource source) const noexcept
    {
        if (source == InputSource::Primary && media_.format) {
            // The main input remains the duration/end-of-media authority even
            // when every selected decoder reads from a sidecar (for example,
            // an audio-only main input with external audio selected).
            return true;
        }
        return (media_.video.valid() && media_.video.source == source)
            || (media_.audio.valid() && media_.audio.source == source)
            || (media_.subtitle.valid()
                && media_.subtitle.source == source);
    }

    bool selectedStream(InputSource source, int streamIndex) const noexcept
    {
        const bool selected =
            (media_.video.valid() && media_.video.source == source
                && media_.video.streamIndex == streamIndex)
            || (media_.audio.valid() && media_.audio.source == source
                && media_.audio.streamIndex == streamIndex)
            || (media_.subtitle.valid()
                && media_.subtitle.source == source
                && media_.subtitle.streamIndex == streamIndex);
        if (selected || source != InputSource::Primary) {
            return selected;
        }
        const bool hasPrimaryDecoder =
            (media_.video.valid()
             && media_.video.source == InputSource::Primary)
            || (media_.audio.valid()
                && media_.audio.source == InputSource::Primary)
            || (media_.subtitle.valid()
                && media_.subtitle.source == InputSource::Primary);
        return !hasPrimaryDecoder;
    }

    int validateRecoveredInput(
        InputSource source,
        const AVFormatContext* replacement) const noexcept
    {
        if (!replacement) {
            return AVERROR(EINVAL);
        }
        const auto validate = [source, replacement](const Decoder& decoder) {
            if (!decoder.valid() || decoder.source != source) {
                return true;
            }
            if (decoder.streamIndex < 0
                || static_cast<unsigned>(decoder.streamIndex)
                    >= replacement->nb_streams) {
                return false;
            }
            const auto* parameters =
                replacement->streams[decoder.streamIndex]->codecpar;
            return parameters
                && parameters->codec_type == decoder.context->codec_type
                && parameters->codec_id == decoder.context->codec_id;
        };
        return validate(media_.video) && validate(media_.audio)
                && validate(media_.subtitle)
            ? 0
            : AVERROR_STREAM_NOT_FOUND;
    }

    static int seekRecoveredFormat(
        AVFormatContext* format,
        std::int64_t startTimeUs,
        std::int64_t positionMilliseconds)
    {
        if (!formatIsSeekable(format)) {
            return 0;
        }
        const auto timestamp = startTimeUs
            + std::max<std::int64_t>(0, positionMilliseconds) * 1000;
        return avformat_seek_file(
            format,
            -1,
            std::numeric_limits<std::int64_t>::min(),
            timestamp,
            std::numeric_limits<std::int64_t>::max(),
            AVSEEK_FLAG_BACKWARD);
    }

    int readRecoveredPacket(
        InputSource source,
        AVFormatContext* format,
        AVPacket** output,
        bool* reachedEnd)
    {
        if (!format || !output || !reachedEnd) {
            return AVERROR(EINVAL);
        }
        *output = nullptr;
        *reachedEnd = false;
        auto* packet = av_packet_alloc();
        if (!packet) {
            return AVERROR(ENOMEM);
        }
        while (true) {
            const int error = av_read_frame(format, packet);
            if (error == AVERROR_EOF) {
                av_packet_free(&packet);
                *reachedEnd = true;
                return 0;
            }
            if (error < 0) {
                av_packet_free(&packet);
                return error;
            }
            if (selectedStream(source, packet->stream_index)
                && (packet->flags & AV_PKT_FLAG_CORRUPT) == 0) {
                *output = packet;
                return 0;
            }
            av_packet_unref(packet);
        }
    }

    void storeDemuxPacket(
        InputSource source,
        AVFormatContext* format,
        AVPacket* packet)
    {
        auto& demux = demuxForSource(source);
        const auto* stream = format->streams[packet->stream_index];
        const auto packetTimestamp = packet->dts != AV_NOPTS_VALUE
            ? packet->dts
            : packet->pts;
        demux.pendingTimestamp = packetTimestamp == AV_NOPTS_VALUE
            ? std::numeric_limits<std::int64_t>::min()
            : (av_rescale_q(
                   packetTimestamp,
                   stream->time_base,
                   AV_TIME_BASE_Q)
                - startTimeForSource(source))
                / 1000;
        demux.pendingDuration = packet->duration > 0
            ? std::max<std::int64_t>(
                0,
                av_rescale_q(
                    packet->duration,
                    stream->time_base,
                    AVRational { 1, 1000 }))
            : 0;
        if (demux.pendingTimestamp
            != std::numeric_limits<std::int64_t>::min()) {
            auto& progress = demuxProgressForSource(source);
            progress = std::max(progress, demux.pendingTimestamp);
        }
        demux.pendingPacket = std::shared_ptr<AVPacket>(
            packet,
            [](AVPacket* owned) { av_packet_free(&owned); });
    }

    int installRecoveredInput(
        InputSource source,
        AVFormatContext*& replacement,
        AVPacket*& firstPacket,
        bool reachedEnd,
        std::uint64_t serial,
        std::int64_t resumePosition)
    {
        if (!replacement
            || !networkRecoveryControlValid(
                serial,
                NetworkRecoveryOperation::Read)) {
            return AVERROR_EXIT;
        }

        resetPlaybackQueues();
        flushAudioSink();
        if (!networkRecoveryControlValid(
                serial,
                NetworkRecoveryOperation::Read)) {
            return AVERROR_EXIT;
        }

        auto*& slot = formatSlotForSource(source);
        auto* retired = slot;
        slot = replacement;
        replacement = nullptr;

        const auto actualStartTime = slot->start_time == AV_NOPTS_VALUE
            ? 0
            : slot->start_time;
        const auto normalizedStartTime = formatIsSeekable(slot)
            ? actualStartTime
            : actualStartTime - resumePosition * 1000;
        startTimeSlotForSource(source) = normalizedStartTime;

        media_.demux.reset();
        media_.externalAudio.resetDemux();
        media_.externalSubtitle.resetDemux();

        const auto updateDecoder = [&](Decoder& decoder) {
            if (!decoder.valid() || decoder.source != source) {
                return;
            }
            decoder.stream = slot->streams[decoder.streamIndex];
            decoder.startTimeUs = normalizedStartTime;
            decoder.context->pkt_timebase = decoder.stream->time_base;
        };
        updateDecoder(media_.video);
        updateDecoder(media_.audio);
        updateDecoder(media_.subtitle);

        if (firstPacket) {
            storeDemuxPacket(source, slot, firstPacket);
            firstPacket = nullptr;
        } else if (reachedEnd) {
            demuxForSource(source).end = true;
        }

        for (const auto other : {
                 InputSource::Primary,
                 InputSource::ExternalAudio,
                 InputSource::ExternalSubtitle,
             }) {
            if (other == source || !sourceIsActive(other)) {
                continue;
            }
            auto* format = formatForSource(other);
            if (!formatIsSeekable(format)) {
                continue;
            }
            const int alignmentError = seekRecoveredFormat(
                format,
                startTimeForSource(other),
                resumePosition);
            if (alignmentError < 0) {
                publishEvent({
                    "network.recovery.alignment",
                    "Recovered the failed network input, but could not "
                    "realign the "
                        + std::string(inputDescription(other))
                        + " input: " + ffmpegError(alignmentError),
                    alignmentError,
                });
            }
        }

        if (media_.video.valid()) {
            avcodec_flush_buffers(media_.video.context);
        }
        if (media_.audio.valid()) {
            avcodec_flush_buffers(media_.audio.context);
        }
        if (media_.subtitle.valid()) {
            avcodec_flush_buffers(media_.subtitle.context);
        }
        avformat_close_input(&retired);

        bool waitForData = false;
        {
            std::lock_guard<std::mutex> lock(mutex_);
            if (source == InputSource::Primary) {
                mediaInfo_.startTime = actualStartTime / 1000;
                if (slot->duration != AV_NOPTS_VALUE) {
                    mediaInfo_.duration = slot->duration / 1000;
                }
                mediaInfo_.seekable = formatIsSeekable(slot);
            }
            currentPosition_ = clampPositionLocked(resumePosition);
            resetClockLocked(currentPosition_);
            waitForData = currentState_ == State::Playing
                && requestedState_ == State::Playing
                && (media_.video.valid() || media_.audio.valid());
            if (waitForData) {
                primeOutputWaitLocked(
                    currentPosition_,
                    presentationGeneration_.load(std::memory_order_acquire),
                    media_.video.valid() && media_.audio.valid(),
                    audioSinkOpen_ && audioSinkHasClock_);
            }
        }
        if (waitForData) {
            publishStatus(MediaStatus::Buffering);
            const auto generation = presentationGeneration_.load(
                std::memory_order_acquire);
            const auto packetStreams = activePacketStreams();
            if (beginPacketBuffering(
                    PacketBufferingReason::NetworkRecovery,
                    generation,
                    packetStreams.first,
                    packetStreams.second)) {
                updatePacketBuffering(generation);
            }
        }
        return 0;
    }

    int recoverNetworkRead(InputSource source, int initialError)
    {
        const auto inputUrl = urlForSource(source);
        const auto policy = networkRecoveryPolicy();
        if (!policy.enabled || !isNetworkUrl(inputUrl)
            || !isRecoverableNetworkError(initialError)) {
            return initialError;
        }

        std::uint64_t serial = 0;
        {
            std::lock_guard<std::mutex> lock(mutex_);
            serial = mediaSerial_;
        }
        auto& budget = readRecoveryBudgetForSource(source);
        const auto progress = demuxProgressForSource(source);
        const bool newBudget = !budget.active
            || budget.mediaSerial != serial;
        const bool madeProgress = !newBudget
            && budget.progressTimestamp
                != std::numeric_limits<std::int64_t>::min()
            && progress >= budget.progressTimestamp
                    + kNetworkRecoveryProgressMilliseconds;
        if (newBudget || madeProgress) {
            budget.active = true;
            budget.mediaSerial = serial;
            budget.progressTimestamp = progress;
            budget.attempts = 0;
        } else if (budget.progressTimestamp
                       == std::numeric_limits<std::int64_t>::min()
                   && progress
                       != std::numeric_limits<std::int64_t>::min()) {
            // Establish an anchor without forgiving an attempt that only
            // produced one early packet before failing again.
            budget.progressTimestamp = progress;
        }
        int error = initialError;
        auto delay = policy.initialRetryDelayMilliseconds;
        for (std::uint32_t consumed = 0;
             consumed < budget.attempts;
             ++consumed) {
            delay = nextNetworkRetryDelay(
                delay,
                policy.maximumRetryDelayMilliseconds);
        }
        std::uint32_t attemptsConsumed = budget.attempts;
        for (std::uint32_t attempt = budget.attempts + 1;
             attempt <= policy.maximumAttempts;
             ++attempt) {
            const auto resumePosition = position();
            NetworkRecoveryStatus status;
            status.state = NetworkRecoveryState::Waiting;
            status.operation = NetworkRecoveryOperation::Read;
            status.input = publicRecoveryInput(source);
            status.url = inputUrl;
            status.attempt = attempt;
            status.maximumAttempts = policy.maximumAttempts;
            status.retryDelayMilliseconds = delay;
            status.error = error;
            status.resumePosition = resumePosition;
            status.presentationGeneration = presentationGeneration_.load(
                std::memory_order_acquire);
            publishNetworkRecoveryStatus(status);
            if (!waitForNetworkRetry(
                    serial,
                    NetworkRecoveryOperation::Read,
                    delay)) {
                publishNetworkRecoveryStatus({});
                return AVERROR_EXIT;
            }

            status.state = NetworkRecoveryState::Reopening;
            status.retryDelayMilliseconds = 0;
            publishNetworkRecoveryStatus(status);
            attemptsConsumed = attempt;
            budget.attempts = attempt;
            networkRecoveryAttempts_.fetch_add(1, std::memory_order_relaxed);

            AVFormatContext* replacement = nullptr;
            AVPacket* firstPacket = nullptr;
            bool reachedEnd = false;
            error = openInput(inputUrl, &replacement);
            if (error >= 0) {
                error = validateRecoveredInput(source, replacement);
            }
            const auto replacementStartTime = replacement
                    && replacement->start_time != AV_NOPTS_VALUE
                ? replacement->start_time
                : 0;
            if (error >= 0 && formatIsSeekable(replacement)) {
                error = seekRecoveredFormat(
                    replacement,
                    replacementStartTime,
                    resumePosition);
            }
            if (error >= 0) {
                // A successful open is provisional until the replacement can
                // return a selected packet (or a clean EOF). Keep immediate
                // post-open failures inside this bounded recovery cycle.
                error = readRecoveredPacket(
                    source,
                    replacement,
                    &firstPacket,
                    &reachedEnd);
            }
            if (error >= 0) {
                error = installRecoveredInput(
                    source,
                    replacement,
                    firstPacket,
                    reachedEnd,
                    serial,
                    resumePosition);
            }
            av_packet_free(&firstPacket);
            avformat_close_input(&replacement);

            if (error >= 0) {
                status.state = NetworkRecoveryState::Recovered;
                status.error = 0;
                status.resumePosition = resumePosition;
                status.presentationGeneration = presentationGeneration_.load(
                    std::memory_order_acquire);
                publishNetworkRecoveryStatus(status);
                successfulNetworkRecoveries_.fetch_add(
                    1,
                    std::memory_order_relaxed);
                publishEvent({
                    "network.recovered",
                    "Recovered the "
                        + std::string(inputDescription(source))
                        + " network input at "
                        + std::to_string(resumePosition)
                        + " ms on attempt " + std::to_string(attempt),
                    0,
                });
                return 0;
            }
            if (error == AVERROR_EXIT
                || !networkRecoveryControlValid(
                    serial,
                    NetworkRecoveryOperation::Read)) {
                publishNetworkRecoveryStatus({});
                return AVERROR_EXIT;
            }
            if (!isRecoverableNetworkError(error)) {
                break;
            }
            delay = nextNetworkRetryDelay(
                delay,
                policy.maximumRetryDelayMilliseconds);
        }

        NetworkRecoveryStatus failed;
        failed.state = NetworkRecoveryState::Failed;
        failed.operation = NetworkRecoveryOperation::Read;
        failed.input = publicRecoveryInput(source);
        failed.url = inputUrl;
        failed.attempt = attemptsConsumed;
        failed.maximumAttempts = policy.maximumAttempts;
        failed.error = error;
        failed.resumePosition = position();
        failed.presentationGeneration = presentationGeneration_.load(
            std::memory_order_acquire);
        publishNetworkRecoveryStatus(failed);
        failedNetworkRecoveries_.fetch_add(1, std::memory_order_relaxed);
        publishEvent({
            "network.recovery.failed",
            "Could not recover the "
                + std::string(inputDescription(source))
                + " network input after a read error: "
                + ffmpegError(error),
            error,
        });
        return error;
    }

    InputReadResult ensureDemuxPacket(InputSource source)
    {
        auto& demux = demuxForSource(source);
        if (!sourceIsActive(source)) {
            demux.reset();
            return InputReadResult::End;
        }
        if (demux.pendingPacket) {
            return InputReadResult::Ready;
        }
        if (demux.end) {
            return InputReadResult::End;
        }

        auto* format = formatForSource(source);
        if (!format) {
            return InputReadResult::End;
        }
        AVPacket* packet = av_packet_alloc();
        if (!packet) {
            publishEvent({
                "decode.error",
                "Could not allocate an FFmpeg packet",
                AVERROR(ENOMEM),
            });
            return InputReadResult::Error;
        }

        while (true) {
            const int error = av_read_frame(format, packet);
            if (error == AVERROR_EOF) {
                av_packet_free(&packet);
                demux.end = true;
                return InputReadResult::End;
            }
            if (error < 0) {
                av_packet_free(&packet);
                const int recoveryError = recoverNetworkRead(source, error);
                if (recoveryError >= 0) {
                    return InputReadResult::Recovered;
                }
                if (recoveryError != AVERROR_EXIT) {
                    publishEvent({
                        "reader.error",
                        "Could not read "
                            + std::string(inputDescription(source))
                            + " packet: " + ffmpegError(recoveryError),
                        recoveryError,
                    });
                }
                return InputReadResult::Error;
            }
            if (!selectedStream(source, packet->stream_index)) {
                av_packet_unref(packet);
                continue;
            }

            storeDemuxPacket(source, format, packet);
            return InputReadResult::Ready;
        }
    }

    DecodeResult readAndDecodeOnePacket()
    {
        const InputSource sources[] = {
            InputSource::Primary,
            InputSource::ExternalAudio,
            InputSource::ExternalSubtitle,
        };
        InputSource selectedSource = InputSource::Primary;
        bool hasPacket = false;
        std::int64_t selectedTimestamp =
            std::numeric_limits<std::int64_t>::max();
        for (const auto source : sources) {
            const auto result = ensureDemuxPacket(source);
            if (result == InputReadResult::Recovered) {
                // Installing an external-input replacement invalidates every
                // source's pending demux packet. Restart the whole merge scan
                // so a candidate selected earlier in this loop cannot outlive
                // that generation reset.
                return DecodeResult::Continue;
            }
            if (result == InputReadResult::Error) {
                return DecodeResult::Error;
            }
            if (result == InputReadResult::End) {
                const auto generation = presentationGeneration_.load(
                    std::memory_order_acquire);
                if (media_.audio.valid()
                    && media_.audio.source == source) {
                    markPacketStreamEnded(generation, false);
                }
                if (media_.video.valid()
                    && media_.video.source == source) {
                    markPacketStreamEnded(generation, true);
                }
            }
            auto& demux = demuxForSource(source);
            if (result == InputReadResult::Ready
                && (!hasPacket
                    || demux.pendingTimestamp < selectedTimestamp)) {
                hasPacket = true;
                selectedSource = source;
                selectedTimestamp = demux.pendingTimestamp;
            }
        }

        if (media_.demux.end) {
            std::int64_t duration = 0;
            {
                std::lock_guard<std::mutex> lock(mutex_);
                duration = mediaInfo_.duration;
            }
            if (duration > 0) {
                for (const auto source : {
                         InputSource::ExternalAudio,
                         InputSource::ExternalSubtitle,
                     }) {
                    auto& demux = demuxForSource(source);
                    if (demux.pendingPacket
                        && demux.pendingTimestamp > duration) {
                        demux.pendingPacket.reset();
                        demux.end = true;
                        if (selectedSource == source) {
                            hasPacket = false;
                        }
                    }
                }
                if (!hasPacket) {
                    for (const auto source : sources) {
                        auto& demux = demuxForSource(source);
                        if (demux.pendingPacket
                            && (!hasPacket
                                || demux.pendingTimestamp
                                    < selectedTimestamp)) {
                            hasPacket = true;
                            selectedSource = source;
                            selectedTimestamp = demux.pendingTimestamp;
                        }
                    }
                }
            }
        }

        if (!hasPacket) {
            const auto generation =
                presentationGeneration_.load(std::memory_order_acquire);
            if (!finishQueuedDecoding(generation)) {
                return DecodeResult::Error;
            }
            return DecodeResult::End;
        }

        auto& selectedDemux = demuxForSource(selectedSource);
        auto packet = std::move(selectedDemux.pendingPacket);
        const auto packetTimestamp = selectedDemux.pendingTimestamp;
        const auto packetDuration = selectedDemux.pendingDuration;

        bool ok = true;
        if (media_.video.valid()
            && media_.video.source == selectedSource
            && packet->stream_index == media_.video.streamIndex) {
            AVPacket* copy = av_packet_clone(packet.get());
            if (!copy) {
                ok = false;
            } else {
                std::shared_ptr<AVPacket> retained(
                    copy,
                    [](AVPacket* owned) {
                        av_packet_free(&owned);
                    });
                ok = enqueueVideoPacket(
                    std::move(retained),
                    presentationGeneration_.load(
                        std::memory_order_acquire),
                    packetTimestamp,
                    packetDuration,
                    false);
            }
        } else if (media_.audio.valid()
                   && media_.audio.source == selectedSource
                   && packet->stream_index == media_.audio.streamIndex) {
            AVPacket* copy = av_packet_clone(packet.get());
            if (!copy) {
                ok = false;
            } else {
                std::shared_ptr<AVPacket> retained(
                    copy,
                    [](AVPacket* owned) {
                        av_packet_free(&owned);
                    });
                ok = enqueueAudioPacket(
                    std::move(retained),
                    presentationGeneration_.load(
                        std::memory_order_acquire),
                    packetTimestamp,
                    packetDuration,
                    false);
            }
        } else if (media_.subtitle.valid()
                   && media_.subtitle.source == selectedSource
                   && packet->stream_index == media_.subtitle.streamIndex) {
            ok = decodeSubtitlePacket(
                packet.get(),
                presentationGeneration_.load(
                    std::memory_order_acquire));
        }
        {
            std::lock_guard<std::mutex> lock(videoPacketMutex_);
            if (videoDecodeFailed_
                && videoDecodeFailureGeneration_
                    == presentationGeneration_.load(
                        std::memory_order_acquire)) {
                ok = false;
            }
        }
        {
            std::lock_guard<std::mutex> lock(audioPacketMutex_);
            if (audioDecodeFailed_
                && audioDecodeFailureGeneration_
                    == presentationGeneration_.load(
                        std::memory_order_acquire)) {
                ok = false;
            }
        }
        return ok ? DecodeResult::Continue : DecodeResult::Error;
    }

    bool decodeSubtitlePacket(
        const AVPacket* packet,
        std::uint64_t generation)
    {
        if (!media_.subtitle.valid()) {
            return true;
        }

        AVPacket remaining {};
        if (packet) {
            remaining = *packet;
        } else {
            remaining.pts = AV_NOPTS_VALUE;
            remaining.dts = AV_NOPTS_VALUE;
        }
        const int maximumIterations = packet ? 64 : 32;
        for (int iteration = 0; iteration < maximumIterations; ++iteration) {
            if (generation
                != presentationGeneration_.load(
                    std::memory_order_acquire)) {
                return false;
            }

            AVSubtitle subtitle {};
            int produced = 0;
            const int error = avcodec_decode_subtitle2(
                media_.subtitle.context,
                &subtitle,
                &produced,
                &remaining);
            if (error < 0) {
                avsubtitle_free(&subtitle);
                publishEvent({
                    "subtitle.decode",
                    "Could not decode a subtitle packet: "
                        + ffmpegError(error),
                    error,
                });
                return false;
            }

            if (produced) {
                std::string text;
                std::vector<std::string> assEvents;
                bool forced = false;
                for (unsigned index = 0; index < subtitle.num_rects; ++index) {
                    const auto* rectangle = subtitle.rects[index];
                    if (!rectangle) {
                        continue;
                    }
                    forced = forced
                        || (rectangle->flags & AV_SUBTITLE_FLAG_FORCED) != 0;
                    std::string rectangleText;
                    if (rectangle->type == SUBTITLE_TEXT && rectangle->text) {
                        rectangleText = rectangle->text;
                    } else if (rectangle->type == SUBTITLE_ASS
                               && rectangle->ass) {
                        assEvents.emplace_back(rectangle->ass);
                        rectangleText = plainTextFromAss(rectangle->ass);
                    } else if (rectangle->text) {
                        rectangleText = rectangle->text;
                    }
                    if (!rectangleText.empty()) {
                        if (!text.empty()) {
                            text.push_back('\n');
                        }
                        text += rectangleText;
                    }
                }

                if (!text.empty() || !assEvents.empty()) {
                    std::int64_t packetPtsUs = subtitle.pts;
                    if (packetPtsUs == AV_NOPTS_VALUE && packet
                        && packet->pts != AV_NOPTS_VALUE) {
                        packetPtsUs = av_rescale_q(
                            packet->pts,
                            media_.subtitle.stream->time_base,
                            AV_TIME_BASE_Q);
                    }
                    if (packetPtsUs == AV_NOPTS_VALUE) {
                        packetPtsUs = media_.subtitle.startTimeUs;
                    }
                    const auto timestampMs = std::max<std::int64_t>(
                        0,
                        (packetPtsUs - media_.subtitle.startTimeUs) / 1000
                            + subtitle.start_display_time);
                    std::int64_t durationMs =
                        subtitle.end_display_time
                            > subtitle.start_display_time
                        ? subtitle.end_display_time
                            - subtitle.start_display_time
                        : 0;
                    if (durationMs <= 0 && packet && packet->duration > 0) {
                        durationMs = toMilliseconds(
                            packet->duration,
                            media_.subtitle.stream->time_base);
                    }

                    std::int64_t rangeStart;
                    std::int64_t rangeEnd;
                    {
                        std::lock_guard<std::mutex> lock(mutex_);
                        rangeStart = rangeStart_;
                        rangeEnd = rangeEnd_;
                    }
                    if (!shouldDropNonVideoForAccurateSeek(
                            timestampMs,
                            generation)
                        && timestampMs >= rangeStart
                        && (rangeEnd == MediaEnd
                            || timestampMs < rangeEnd)) {
                        std::string assHeader;
                        if (media_.subtitle.context->subtitle_header
                            && media_.subtitle.context->subtitle_header_size
                                > 0) {
                            assHeader.assign(
                                reinterpret_cast<const char*>(
                                    media_.subtitle.context->subtitle_header),
                                static_cast<std::size_t>(
                                    media_.subtitle.context
                                        ->subtitle_header_size));
                        }
                        auto frame = detail::FrameFactory::subtitle(
                            std::move(text),
                            std::move(assEvents),
                            std::move(assHeader),
                            timestampMs,
                            durationMs,
                            forced,
                            media_.subtitle.trackIndex,
                            generation);
                        if (frame) {
                            PresentationItem item;
                            item.type = PresentationItem::Type::Subtitle;
                            item.subtitle = std::move(frame);
                            item.track = media_.subtitle.trackIndex;
                            item.generation = generation;
                            enqueuePresentation(std::move(item));
                        }
                    }
                }
            }
            avsubtitle_free(&subtitle);

            if (!packet) {
                if (!produced) {
                    return true;
                }
                continue;
            }
            if (error <= 0 || error >= remaining.size) {
                return true;
            }
            remaining.data += error;
            remaining.size -= error;
        }

        publishEvent({
            "subtitle.decode",
            "The subtitle decoder did not consume the packet within the "
            "bounded iteration limit",
            AVERROR_INVALIDDATA,
        });
        return false;
    }

    bool decodeRequestValid(std::uint64_t generation) const
    {
        if (quitting_.load(std::memory_order_acquire)
            || generation
                != presentationGeneration_.load(
                    std::memory_order_acquire)) {
            return false;
        }
        std::lock_guard<std::mutex> lock(mutex_);
        return hasOpenMedia_
            && !seekCompletion_
            && (requestedState_ == State::Playing
                || (accurateSeek_
                    && accurateSeek_->generation == generation));
    }

    std::pair<bool, bool> activePacketStreams() const
    {
        std::lock_guard<std::mutex> lock(mutex_);
        return {
            hasOpenMedia_ && mediaInfo_.activeAudioTrack >= 0,
            hasOpenMedia_ && mediaInfo_.activeVideoTrack >= 0,
        };
    }

    template <typename Queue>
    static PacketQueueMetrics packetQueueMetrics(const Queue& queue)
    {
        PacketQueueMetrics metrics;
        std::int64_t firstTimestamp =
            std::numeric_limits<std::int64_t>::max();
        std::int64_t lastEnd =
            std::numeric_limits<std::int64_t>::min();
        std::int64_t summedDuration = 0;
        std::int64_t firstMemoryTimestamp = firstTimestamp;
        std::int64_t lastMemoryEnd = lastEnd;
        std::int64_t summedMemoryDuration = 0;
        std::int64_t firstDiskTimestamp = firstTimestamp;
        std::int64_t lastDiskEnd = lastEnd;
        std::int64_t summedDiskDuration = 0;
        const auto accumulateDuration = [](
                                            const auto& queued,
                                            std::int64_t& first,
                                            std::int64_t& last,
                                            std::int64_t& summed) {
            summed += std::max<std::int64_t>(0, queued.duration);
            if (queued.timestamp
                == std::numeric_limits<std::int64_t>::min()) {
                return;
            }
            first = std::min(first, queued.timestamp);
            last = std::max(
                last,
                queued.timestamp
                    + std::max<std::int64_t>(0, queued.duration));
        };
        const auto duration = [](std::int64_t first,
                                 std::int64_t last,
                                 std::int64_t summed) {
            const auto span = first
                    != std::numeric_limits<std::int64_t>::max()
                    && last >= first
                ? last - first
                : 0;
            return std::max(span, summed);
        };
        for (const auto& queued : queue) {
            if (!queued.packet || queued.end) {
                if (!queued.diskEntry || queued.end) {
                    continue;
                }
            }
            ++metrics.packets;
            metrics.bytes += queued.bytes;
            accumulateDuration(
                queued,
                firstTimestamp,
                lastEnd,
                summedDuration);
            if (queued.diskEntry) {
                ++metrics.diskPackets;
                metrics.diskBytes += queued.bytes;
                accumulateDuration(
                    queued,
                    firstDiskTimestamp,
                    lastDiskEnd,
                    summedDiskDuration);
            } else {
                ++metrics.memoryPackets;
                metrics.memoryBytes += queued.bytes;
                accumulateDuration(
                    queued,
                    firstMemoryTimestamp,
                    lastMemoryEnd,
                    summedMemoryDuration);
            }
        }
        metrics.bufferedMilliseconds = duration(
            firstTimestamp,
            lastEnd,
            summedDuration);
        metrics.memoryBufferedMilliseconds = duration(
            firstMemoryTimestamp,
            lastMemoryEnd,
            summedMemoryDuration);
        metrics.diskBufferedMilliseconds = duration(
            firstDiskTimestamp,
            lastDiskEnd,
            summedDiskDuration);
        return metrics;
    }

    PacketBufferStatus currentPacketBufferStatus() const
    {
        PacketBufferState state;
        {
            std::lock_guard<std::mutex> lock(packetBufferMutex_);
            state = packetBufferState_;
        }

        PacketQueueMetrics audio;
        PacketQueueMetrics video;
        {
            std::lock_guard<std::mutex> lock(audioPacketMutex_);
            audio = packetQueueMetrics(audioPackets_);
        }
        {
            std::lock_guard<std::mutex> lock(videoPacketMutex_);
            video = packetQueueMetrics(videoPackets_);
        }

        std::int64_t buffered = state.targetMilliseconds;
        bool hasTimedStream = false;
        if (state.needsAudio && !state.audioEnded) {
            buffered = audio.bufferedMilliseconds;
            hasTimedStream = true;
        }
        if (state.needsVideo && !state.videoEnded) {
            buffered = hasTimedStream
                ? std::min(buffered, video.bufferedMilliseconds)
                : video.bufferedMilliseconds;
            hasTimedStream = true;
        }
        if (!hasTimedStream) {
            buffered = state.targetMilliseconds;
        }
        const auto progress = state.buffering
                && state.targetMilliseconds > 0
            ? std::clamp(
                static_cast<double>(buffered)
                    / static_cast<double>(state.targetMilliseconds),
                0.0,
                1.0)
            : 1.0;
        const auto memoryBytes = audio.memoryBytes + video.memoryBytes;
        const auto diskBytes = audio.diskBytes + video.diskBytes;
        return {
            state.buffering,
            state.reason,
            std::max<std::int64_t>(0, buffered),
            state.targetMilliseconds,
            memoryBytes + diskBytes,
            memoryBytes,
            diskBytes,
            packetDiskStore_->path(),
            progress,
            state.generation,
            state.capacityLimited,
        };
    }

    static bool materiallyDifferent(
        const PacketBufferStatus& left,
        const PacketBufferStatus& right) noexcept
    {
        return left.buffering != right.buffering
            || left.reason != right.reason
            || left.targetMilliseconds != right.targetMilliseconds
            || left.presentationGeneration != right.presentationGeneration
            || left.capacityLimited != right.capacityLimited
            || std::abs(left.progress - right.progress) >= 0.01
            || (left.bufferedBytes == 0) != (right.bufferedBytes == 0)
            || (left.diskBufferedBytes == 0)
                != (right.diskBufferedBytes == 0)
            || left.diskCachePath != right.diskCachePath;
    }

    void publishPacketBufferStatus(bool force = false)
    {
        const auto status = currentPacketBufferStatus();
        {
            std::lock_guard<std::mutex> lock(packetBufferMutex_);
            if (!force && lastPublishedPacketBufferStatus_
                && !materiallyDifferent(
                    *lastPublishedPacketBufferStatus_,
                    status)) {
                return;
            }
            lastPublishedPacketBufferStatus_ = status;
        }
        PacketBufferStatusCallback callback;
        {
            std::lock_guard<std::mutex> lock(mutex_);
            callback = packetBufferStatusCallback_;
        }
        if (callback) {
            callback(status);
        }
    }

    bool beginPacketBuffering(
        PacketBufferingReason reason,
        std::uint64_t generation,
        bool needsAudio,
        bool needsVideo)
    {
        const auto policy = packetBufferPolicy();
        const auto target = reason == PacketBufferingReason::InitialPlayback
            ? policy.initialBufferMilliseconds
            : policy.rebufferMilliseconds;
        if (!policy.enabled || target <= 0 || (!needsAudio && !needsVideo)) {
            return false;
        }
        {
            std::lock_guard<std::mutex> lock(packetBufferMutex_);
            if (packetBufferState_.buffering
                && packetBufferState_.generation == generation) {
                return true;
            }
            packetBufferState_ = {
                true,
                reason,
                target,
                generation,
                needsAudio,
                needsVideo,
                audioPacketInputEnded_.load(std::memory_order_acquire),
                videoPacketInputEnded_.load(std::memory_order_acquire),
                false,
            };
            lastPublishedPacketBufferStatus_.reset();
        }
        publishPacketBufferStatus(true);
        publishStatus(MediaStatus::Buffering);
        packetBufferChanged_.notify_all();
        return true;
    }

    void completePacketBuffering(
        std::uint64_t generation,
        bool capacityLimited = false)
    {
        {
            std::lock_guard<std::mutex> lock(packetBufferMutex_);
            if (!packetBufferState_.buffering
                || packetBufferState_.generation != generation) {
                return;
            }
            packetBufferState_.buffering = false;
            packetBufferState_.capacityLimited = capacityLimited;
        }
        packetBufferChanged_.notify_all();
        audioPacketChanged_.notify_all();
        videoPacketChanged_.notify_all();
        publishPacketBufferStatus(true);
    }

    void updatePacketBuffering(std::uint64_t generation)
    {
        const auto status = currentPacketBufferStatus();
        bool complete = false;
        {
            std::lock_guard<std::mutex> lock(packetBufferMutex_);
            if (!packetBufferState_.buffering
                || packetBufferState_.generation != generation) {
                return;
            }
            const bool audioReady = !packetBufferState_.needsAudio
                || packetBufferState_.audioEnded
                || status.bufferedMilliseconds
                    >= packetBufferState_.targetMilliseconds;
            const bool videoReady = !packetBufferState_.needsVideo
                || packetBufferState_.videoEnded
                || status.bufferedMilliseconds
                    >= packetBufferState_.targetMilliseconds;
            complete = audioReady && videoReady;
        }
        if (complete) {
            completePacketBuffering(generation);
        } else {
            publishPacketBufferStatus();
        }
    }

    void markPacketStreamEnded(
        std::uint64_t generation,
        bool video)
    {
        bool alreadyEnded = false;
        if (video) {
            alreadyEnded = videoPacketInputEnded_.exchange(
                true,
                std::memory_order_acq_rel);
        } else {
            alreadyEnded = audioPacketInputEnded_.exchange(
                true,
                std::memory_order_acq_rel);
        }
        if (alreadyEnded) {
            return;
        }
        {
            std::lock_guard<std::mutex> lock(packetBufferMutex_);
            if (packetBufferState_.generation != generation) {
                return;
            }
            if (video) {
                packetBufferState_.videoEnded = true;
            } else {
                packetBufferState_.audioEnded = true;
            }
        }
        updatePacketBuffering(generation);
    }

    void resetPacketBuffering()
    {
        bool publishReset = false;
        {
            std::lock_guard<std::mutex> lock(packetBufferMutex_);
            publishReset = packetBufferState_.buffering;
            packetBufferState_ = {};
            lastPublishedPacketBufferStatus_.reset();
        }
        packetBufferChanged_.notify_all();
        if (publishReset) {
            publishPacketBufferStatus(true);
        }
    }

    bool waitForPacketBuffering(std::uint64_t generation)
    {
        std::unique_lock<std::mutex> lock(packetBufferMutex_);
        packetBufferChanged_.wait(lock, [this, generation] {
            return quitting_.load(std::memory_order_acquire)
                || packetBufferState_.generation != generation
                || !packetBufferState_.buffering;
        });
        return !quitting_.load(std::memory_order_acquire)
            && generation
                == presentationGeneration_.load(std::memory_order_acquire)
            && (packetBufferState_.generation != generation
                || !packetBufferState_.buffering);
    }

    bool memoryPacketQueueCanAccept(
        const PacketQueueMetrics& metrics,
        std::size_t hardPacketLimit,
        std::uint64_t packetBytes,
        const PacketBufferPolicy& policy) const
    {
        const auto totalBytes = queuedMemoryPacketBytes_.load(
            std::memory_order_relaxed);
        const bool bytesFit = totalBytes == 0
            || (totalBytes < policy.maximumBufferBytes
                && packetBytes
                    <= policy.maximumBufferBytes - totalBytes);
        return metrics.memoryPackets < hardPacketLimit
            && metrics.memoryBufferedMilliseconds
                < policy.maximumBufferMilliseconds
            && bytesFit;
    }

    bool diskPacketQueueCanAccept(
        const PacketQueueMetrics& metrics,
        const PacketBufferPolicy& policy) const
    {
        return policy.diskCache.enabled
            && policy.diskCache.maximumCacheMilliseconds > 0
            && !diskCacheUnavailable_.load(std::memory_order_acquire)
            && metrics.packets < kMaximumQueuedDiskPacketMetadata
            && metrics.diskBufferedMilliseconds
                < policy.diskCache.maximumCacheMilliseconds;
    }

    DiskPacketResult storePacketOnDisk(
        const std::shared_ptr<AVPacket>& packet,
        const PacketBufferPolicy& policy)
    {
        if (!packet || packet->size <= 0 || !packet->data) {
            return { {}, {}, true, {} };
        }

        auto* properties = av_packet_alloc();
        if (!properties) {
            return {
                {},
                {},
                false,
                "Could not allocate packet metadata for the disk cache",
            };
        }
        const int propertiesError = av_packet_copy_props(
            properties,
            packet.get());
        if (propertiesError < 0) {
            av_packet_free(&properties);
            return {
                {},
                {},
                false,
                "Could not copy packet metadata for the disk cache: "
                    + ffmpegError(propertiesError),
            };
        }
        properties->stream_index = packet->stream_index;
        auto retainedProperties = std::shared_ptr<AVPacket>(
            properties,
            [](AVPacket* owned) { av_packet_free(&owned); });
        auto stored = packetDiskStore_->store(
            packet->data,
            static_cast<std::uint64_t>(packet->size),
            policy.diskCache.maximumCacheBytes);
        return {
            std::move(retainedProperties),
            std::move(stored.entry),
            stored.capacityLimited,
            std::move(stored.error),
        };
    }

    void reportPacketDiskCacheError(const std::string& detail)
    {
        if (detail.empty()
            || diskCacheUnavailable_.exchange(
                true,
                std::memory_order_acq_rel)) {
            return;
        }
        publishEvent({
            "packet.disk_cache.error",
            detail + "; continuing with the in-memory packet buffer",
            AVERROR(EIO),
        });
    }

    bool materializeDiskPacket(QueuedVideoPacket& queued)
    {
        if (queued.packet || queued.end) {
            return true;
        }
        if (!queued.diskPacketProperties || !queued.diskEntry) {
            publishEvent({
                "packet.disk_cache.read",
                "A disk-backed packet is missing its cache metadata",
                AVERROR_INVALIDDATA,
            });
            return false;
        }

        std::vector<std::uint8_t> payload;
        std::string error;
        if (!packetDiskStore_->load(queued.diskEntry, payload, error)) {
            publishEvent({
                "packet.disk_cache.read",
                std::move(error),
                AVERROR(EIO),
            });
            return false;
        }
        if (queued.diskEntry->size
            > static_cast<std::uint64_t>(
                std::numeric_limits<int>::max())) {
            publishEvent({
                "packet.disk_cache.read",
                "A disk-backed packet exceeds FFmpeg's packet-size limit",
                AVERROR(EINVAL),
            });
            return false;
        }

        auto* packet = av_packet_alloc();
        int packetError = packet
            ? av_new_packet(
                  packet,
                  static_cast<int>(queued.diskEntry->size))
            : AVERROR(ENOMEM);
        if (packetError >= 0) {
            packetError = av_packet_copy_props(
                packet,
                queued.diskPacketProperties.get());
        }
        if (packetError < 0) {
            av_packet_free(&packet);
            publishEvent({
                "packet.disk_cache.read",
                "Could not restore a disk-backed packet: "
                    + ffmpegError(packetError),
                packetError,
            });
            return false;
        }
        if (!payload.empty()) {
            std::copy(payload.begin(), payload.end(), packet->data);
        }
        packet->stream_index =
            queued.diskPacketProperties->stream_index;
        queued.packet = std::shared_ptr<AVPacket>(
            packet,
            [](AVPacket* owned) { av_packet_free(&owned); });
        queued.diskEntry.reset();
        queued.diskPacketProperties.reset();
        return true;
    }

    bool packetBufferingActive(std::uint64_t generation) const
    {
        std::lock_guard<std::mutex> lock(packetBufferMutex_);
        return packetBufferState_.buffering
            && packetBufferState_.generation == generation;
    }

    bool enqueueVideoPacket(
        std::shared_ptr<AVPacket> packet,
        std::uint64_t generation,
        std::int64_t timestamp,
        std::int64_t duration,
        bool end)
    {
        while (decodeRequestValid(generation)) {
            std::unique_lock<std::mutex> lock(videoPacketMutex_);
            const auto metrics = packetQueueMetrics(videoPackets_);
            const auto bytes = packet && packet->size > 0
                ? static_cast<std::uint64_t>(packet->size)
                : 0;
            const auto policy = packetBufferPolicy();
            if (memoryPacketQueueCanAccept(
                    metrics,
                    kMaximumQueuedVideoPackets,
                    bytes,
                    policy)) {
                videoPackets_.push_back({
                    std::move(packet),
                    {},
                    {},
                    generation,
                    timestamp,
                    duration,
                    bytes,
                    end,
                });
                queuedMemoryPacketBytes_.fetch_add(
                    bytes,
                    std::memory_order_relaxed);
                lock.unlock();
                if (end) {
                    markPacketStreamEnded(generation, true);
                } else {
                    updatePacketBuffering(generation);
                }
                videoPacketChanged_.notify_one();
                return true;
            }

            DiskPacketResult diskPacket;
            if (packet && diskPacketQueueCanAccept(metrics, policy)) {
                diskPacket = storePacketOnDisk(packet, policy);
                if (diskPacket.entry) {
                    videoPackets_.push_back({
                        {},
                        std::move(diskPacket.properties),
                        std::move(diskPacket.entry),
                        generation,
                        timestamp,
                        duration,
                        bytes,
                        end,
                    });
                    lock.unlock();
                    updatePacketBuffering(generation);
                    videoPacketChanged_.notify_one();
                    return true;
                }
            }
            if (!diskPacket.error.empty()) {
                const auto error = std::move(diskPacket.error);
                lock.unlock();
                reportPacketDiskCacheError(error);
                if (packetBufferingActive(generation)) {
                    completePacketBuffering(generation, true);
                }
                continue;
            }
            if (packetBufferingActive(generation)) {
                lock.unlock();
                completePacketBuffering(generation, true);
                continue;
            }
            videoPacketSpace_.wait_for(lock, Milliseconds(20));
        }
        return false;
    }

    bool enqueueAudioPacket(
        std::shared_ptr<AVPacket> packet,
        std::uint64_t generation,
        std::int64_t timestamp,
        std::int64_t duration,
        bool end)
    {
        while (decodeRequestValid(generation)) {
            std::unique_lock<std::mutex> lock(audioPacketMutex_);
            const auto metrics = packetQueueMetrics(audioPackets_);
            const auto bytes = packet && packet->size > 0
                ? static_cast<std::uint64_t>(packet->size)
                : 0;
            const auto policy = packetBufferPolicy();
            if (memoryPacketQueueCanAccept(
                    metrics,
                    kMaximumQueuedAudioPackets,
                    bytes,
                    policy)) {
                audioPackets_.push_back({
                    std::move(packet),
                    {},
                    {},
                    generation,
                    timestamp,
                    duration,
                    bytes,
                    end,
                });
                queuedMemoryPacketBytes_.fetch_add(
                    bytes,
                    std::memory_order_relaxed);
                lock.unlock();
                if (end) {
                    markPacketStreamEnded(generation, false);
                } else {
                    updatePacketBuffering(generation);
                }
                audioPacketChanged_.notify_one();
                return true;
            }

            DiskPacketResult diskPacket;
            if (packet && diskPacketQueueCanAccept(metrics, policy)) {
                diskPacket = storePacketOnDisk(packet, policy);
                if (diskPacket.entry) {
                    audioPackets_.push_back({
                        {},
                        std::move(diskPacket.properties),
                        std::move(diskPacket.entry),
                        generation,
                        timestamp,
                        duration,
                        bytes,
                        end,
                    });
                    lock.unlock();
                    updatePacketBuffering(generation);
                    audioPacketChanged_.notify_one();
                    return true;
                }
            }
            if (!diskPacket.error.empty()) {
                const auto error = std::move(diskPacket.error);
                lock.unlock();
                reportPacketDiskCacheError(error);
                if (packetBufferingActive(generation)) {
                    completePacketBuffering(generation, true);
                }
                continue;
            }
            if (packetBufferingActive(generation)) {
                lock.unlock();
                completePacketBuffering(generation, true);
                continue;
            }
            audioPacketSpace_.wait_for(lock, Milliseconds(20));
        }
        return false;
    }

    bool waitForVideoPacketsDrained(std::uint64_t generation)
    {
        std::unique_lock<std::mutex> lock(videoPacketMutex_);
        while (true) {
            if (quitting_.load(std::memory_order_acquire)
                || generation
                    != presentationGeneration_.load(
                        std::memory_order_acquire)) {
                return false;
            }
            if (videoPackets_.empty() && !videoPacketInFlight_) {
                return !videoDecodeFailed_;
            }
            videoPacketDrained_.wait_for(lock, Milliseconds(20));
            lock.unlock();
            const bool requestValid = decodeRequestValid(generation);
            lock.lock();
            if (!requestValid) {
                return false;
            }
        }
    }

    bool waitForAudioPacketsDrained(std::uint64_t generation)
    {
        std::unique_lock<std::mutex> lock(audioPacketMutex_);
        while (true) {
            if (quitting_.load(std::memory_order_acquire)
                || generation
                    != presentationGeneration_.load(
                        std::memory_order_acquire)) {
                return false;
            }
            if (audioPackets_.empty() && !audioPacketInFlight_) {
                return !audioDecodeFailed_;
            }
            audioPacketDrained_.wait_for(lock, Milliseconds(20));
            lock.unlock();
            const bool requestValid = decodeRequestValid(generation);
            lock.lock();
            if (!requestValid) {
                return false;
            }
        }
    }

    bool finishQueuedDecoding(std::uint64_t generation)
    {
        const bool videoEndQueued = !media_.video.valid()
            || enqueueVideoPacket({}, generation, 0, 0, true);
        const bool audioEndQueued = !media_.audio.valid()
            || enqueueAudioPacket({}, generation, 0, 0, true);
        const bool primaryDecodersFinished =
            videoEndQueued && audioEndQueued
            && (!media_.video.valid()
                || waitForVideoPacketsDrained(generation))
            && (!media_.audio.valid()
                || waitForAudioPacketsDrained(generation));
        return primaryDecodersFinished
            && (!media_.subtitle.valid()
                || decodeSubtitlePacket(nullptr, generation));
    }

    void resetVideoPacketQueue()
    {
        std::unique_lock<std::mutex> lock(videoPacketMutex_);
        videoPackets_.clear();
        videoPacketSpace_.notify_all();
        videoPacketChanged_.notify_all();
        videoPacketDrained_.wait(lock, [this] {
            return quitting_.load(std::memory_order_acquire)
                || !videoPacketInFlight_;
        });
        videoPackets_.clear();
        videoDecodeFailed_ = false;
        videoDecodeFailureGeneration_ = 0;
    }

    void resetAudioPacketQueue()
    {
        std::unique_lock<std::mutex> lock(audioPacketMutex_);
        audioPackets_.clear();
        audioPacketSpace_.notify_all();
        audioPacketChanged_.notify_all();
        audioPacketDrained_.wait(lock, [this] {
            return quitting_.load(std::memory_order_acquire)
                || !audioPacketInFlight_;
        });
        audioPackets_.clear();
        audioDecodeFailed_ = false;
        audioDecodeFailureGeneration_ = 0;
    }

    void runAudioDecode()
    {
        while (!quitting_.load(std::memory_order_acquire)) {
            QueuedAudioPacket queued;
            {
                std::unique_lock<std::mutex> lock(audioPacketMutex_);
                while (!quitting_.load(std::memory_order_acquire)) {
                    if (audioPackets_.empty()) {
                        const auto generation =
                            presentationGeneration_.load(
                                std::memory_order_acquire);
                        if (packetBufferingActive(generation)) {
                            lock.unlock();
                            waitForPacketBuffering(generation);
                            lock.lock();
                            continue;
                        }
                        const auto policy = packetBufferPolicy();
                        const bool ready = audioPacketChanged_.wait_for(
                            lock,
                            Milliseconds(
                                policy.underflowDetectionMilliseconds),
                            [this] {
                                return quitting_.load(
                                           std::memory_order_acquire)
                                    || !audioPackets_.empty();
                            });
                        if (!ready
                            && !audioPacketInputEnded_.load(
                                std::memory_order_acquire)) {
                            lock.unlock();
                            if (accurateSeekActive(generation)) {
                                lock.lock();
                                continue;
                            }
                            const auto packetStreams =
                                activePacketStreams();
                            if (packetStreams.first
                                && decodeRequestValid(generation)
                                && beginPacketBuffering(
                                    PacketBufferingReason::Underflow,
                                    generation,
                                    packetStreams.first,
                                    packetStreams.second)) {
                                beginOutputWait(generation, position());
                            }
                            lock.lock();
                            continue;
                        }
                    }
                    if (quitting_.load(std::memory_order_acquire)) {
                        break;
                    }
                    if (audioPackets_.empty()) {
                        continue;
                    }
                    const auto generation =
                        audioPackets_.front().generation;
                    lock.unlock();
                    const bool bufferReady =
                        waitForPacketBuffering(generation);
                    const bool mayDecode =
                        bufferReady && decodeRequestValid(generation);
                    const bool stale = generation
                        != presentationGeneration_.load(
                            std::memory_order_acquire);
                    lock.lock();
                    if (stale || mayDecode) {
                        break;
                    }
                    audioPacketChanged_.wait_for(
                        lock,
                        Milliseconds(20));
                }
                if (quitting_.load(std::memory_order_acquire)) {
                    break;
                }
                if (audioPackets_.empty()) {
                    continue;
                }
                queued = std::move(audioPackets_.front());
                audioPackets_.pop_front();
                audioPacketInFlight_ = true;
                if (!queued.diskEntry) {
                    queuedMemoryPacketBytes_.fetch_sub(
                        queued.bytes,
                        std::memory_order_relaxed);
                }
            }
            audioPacketSpace_.notify_all();

            bool ok = materializeDiskPacket(queued);
            if (queued.generation
                    == presentationGeneration_.load(
                    std::memory_order_acquire)) {
                if (ok && queued.end) {
                    flushDecoder(media_.audio, queued.generation);
                } else if (ok && queued.packet) {
                    ok = decodePacket(
                        media_.audio,
                        queued.packet.get(),
                        queued.generation);
                }
            }

            const bool requestValid =
                decodeRequestValid(queued.generation);
            {
                std::lock_guard<std::mutex> lock(audioPacketMutex_);
                if (!ok && requestValid) {
                    audioDecodeFailed_ = true;
                    audioDecodeFailureGeneration_ = queued.generation;
                }
                audioPacketInFlight_ = false;
            }
            audioPacketSpace_.notify_all();
            audioPacketDrained_.notify_all();
            controlChanged_.notify_all();
        }
        {
            std::lock_guard<std::mutex> lock(audioPacketMutex_);
            audioPacketInFlight_ = false;
            audioPackets_.clear();
        }
        audioPacketSpace_.notify_all();
        audioPacketDrained_.notify_all();
    }

    void runVideoDecode()
    {
        while (!quitting_.load(std::memory_order_acquire)) {
            QueuedVideoPacket queued;
            {
                std::unique_lock<std::mutex> lock(videoPacketMutex_);
                while (!quitting_.load(std::memory_order_acquire)) {
                    if (videoPackets_.empty()) {
                        const auto generation =
                            presentationGeneration_.load(
                                std::memory_order_acquire);
                        if (packetBufferingActive(generation)) {
                            lock.unlock();
                            waitForPacketBuffering(generation);
                            lock.lock();
                            continue;
                        }
                        const auto policy = packetBufferPolicy();
                        const bool ready = videoPacketChanged_.wait_for(
                            lock,
                            Milliseconds(
                                policy.underflowDetectionMilliseconds),
                            [this] {
                                return quitting_.load(
                                           std::memory_order_acquire)
                                    || !videoPackets_.empty();
                            });
                        if (!ready
                            && !videoPacketInputEnded_.load(
                                std::memory_order_acquire)) {
                            lock.unlock();
                            if (accurateSeekActive(generation)) {
                                lock.lock();
                                continue;
                            }
                            const auto packetStreams =
                                activePacketStreams();
                            if (packetStreams.second
                                && decodeRequestValid(generation)
                                && beginPacketBuffering(
                                    PacketBufferingReason::Underflow,
                                    generation,
                                    packetStreams.first,
                                    packetStreams.second)) {
                                beginOutputWait(generation, position());
                            }
                            lock.lock();
                            continue;
                        }
                    }
                    if (quitting_.load(std::memory_order_acquire)) {
                        break;
                    }
                    if (videoPackets_.empty()) {
                        continue;
                    }
                    const auto generation =
                        videoPackets_.front().generation;
                    lock.unlock();
                    const bool bufferReady =
                        waitForPacketBuffering(generation);
                    const bool mayDecode =
                        bufferReady && decodeRequestValid(generation);
                    const bool stale = generation
                        != presentationGeneration_.load(
                            std::memory_order_acquire);
                    lock.lock();
                    if (stale || mayDecode) {
                        break;
                    }
                    videoPacketChanged_.wait_for(
                        lock,
                        Milliseconds(20));
                }
                if (quitting_.load(std::memory_order_acquire)) {
                    break;
                }
                if (videoPackets_.empty()) {
                    continue;
                }
                queued = std::move(videoPackets_.front());
                videoPackets_.pop_front();
                videoPacketInFlight_ = true;
                if (!queued.diskEntry) {
                    queuedMemoryPacketBytes_.fetch_sub(
                        queued.bytes,
                        std::memory_order_relaxed);
                }
            }
            videoPacketSpace_.notify_all();

            bool ok = materializeDiskPacket(queued);
            if (queued.generation
                    == presentationGeneration_.load(
                    std::memory_order_acquire)) {
                if (ok && queued.end) {
                    flushDecoder(media_.video, queued.generation);
                    auto decision = finishAccurateVideoAtEnd(
                        queued.generation);
                    if (decision.action
                        == AccurateVideoDecision::Action::DeliverSelected) {
                        VideoFrameScheduler scheduler;
                        {
                            std::lock_guard<std::mutex> lock(mutex_);
                            scheduler = videoFrameScheduler_;
                        }
                        ok = submitVideoFrame(
                            std::move(decision.frame),
                            media_.video.trackIndex,
                            queued.generation,
                            std::move(scheduler),
                            decision.forceImmediate,
                            true,
                            decision.previousTimestamp);
                    }
                } else if (ok && queued.packet) {
                    const bool paceBeforeDecode =
                        isSurfaceOutputHardwareDevice(
                            media_.video.hardwareDeviceType);
                    if (!paceBeforeDecode
                        || waitForSurfacePacketDecodeWindow(
                            media_.video,
                            queued.packet.get(),
                            queued.generation)) {
                        ok = decodePacket(
                            media_.video,
                            queued.packet.get(),
                            queued.generation);
                    }
                }
            }

            const bool requestValid =
                decodeRequestValid(queued.generation);
            {
                std::lock_guard<std::mutex> lock(videoPacketMutex_);
                if (!ok && requestValid) {
                    videoDecodeFailed_ = true;
                    videoDecodeFailureGeneration_ = queued.generation;
                }
                videoPacketInFlight_ = false;
            }
            videoPacketSpace_.notify_all();
            videoPacketDrained_.notify_all();
            controlChanged_.notify_all();
        }
        {
            std::lock_guard<std::mutex> lock(videoPacketMutex_);
            videoPacketInFlight_ = false;
            videoPackets_.clear();
        }
        videoPacketSpace_.notify_all();
        videoPacketDrained_.notify_all();
    }

    bool decodePacket(
        Decoder& decoder,
        const AVPacket* packet,
        std::uint64_t generation)
    {
        if (!decoder.valid()) {
            return true;
        }

        int error = avcodec_send_packet(decoder.context, packet);
        if (error == AVERROR(EAGAIN)) {
            if (!receiveFrames(decoder, generation)) {
                return false;
            }
            error = avcodec_send_packet(decoder.context, packet);
        }
        if (error < 0) {
            publishEvent({
                "decode.error",
                "Could not submit packet to decoder: " + ffmpegError(error),
                error,
            });
            return false;
        }
        return receiveFrames(decoder, generation);
    }

    void flushDecoder(Decoder& decoder, std::uint64_t generation)
    {
        if (!decoder.valid()) {
            return;
        }
        const int error = avcodec_send_packet(decoder.context, nullptr);
        if (error >= 0 || error == AVERROR_EOF) {
            receiveFrames(decoder, generation);
        }
    }

    bool receiveFrames(Decoder& decoder, std::uint64_t generation)
    {
        AVFrame* frame = av_frame_alloc();
        if (!frame) {
            return false;
        }

        bool result = true;
        while (true) {
            const int error = avcodec_receive_frame(decoder.context, frame);
            if (error == AVERROR(EAGAIN) || error == AVERROR_EOF) {
                break;
            }
            if (error < 0) {
                publishEvent({
                    "decode.error",
                    "Could not receive decoded frame: " + ffmpegError(error),
                    error,
                });
                result = false;
                break;
            }
            if (!deliverFrame(decoder, frame, generation)) {
                result = false;
                break;
            }
            av_frame_unref(frame);
        }
        av_frame_free(&frame);
        return result;
    }

    void recordPresentedVideo(
        std::int64_t timestamp,
        std::uint64_t generation,
        bool accurateSelection,
        std::int64_t accuratePreviousTimestamp)
    {
        std::lock_guard<std::mutex> lock(mutex_);
        if (generation
            != presentationGeneration_.load(std::memory_order_acquire)) {
            return;
        }
        if (accurateSelection) {
            previousPresentedVideoTimestamp_ = accuratePreviousTimestamp;
            lastPresentedVideoTimestamp_ = timestamp;
            currentPosition_ = clampPositionLocked(timestamp);
            resetClockLocked(currentPosition_);
            return;
        }
        if (timestamp != lastPresentedVideoTimestamp_) {
            previousPresentedVideoTimestamp_ = lastPresentedVideoTimestamp_;
            lastPresentedVideoTimestamp_ = timestamp;
        }
        currentPosition_ = std::max(currentPosition_, timestamp);
    }

    bool processVideoFrame(
        VideoFrame& video,
        std::uint64_t generation)
    {
        std::string eventCode;
        std::string eventDetail;
        int eventError = 0;
        bool success = true;
        {
            std::lock_guard<std::mutex> processorLock(
                videoProcessorCallMutex_);
            std::shared_ptr<VideoFrameProcessor> configured;
            std::uint64_t serial = 0;
            {
                std::lock_guard<std::mutex> lock(mutex_);
                configured = videoFrameProcessor_;
                serial = videoFrameProcessorSerial_;
            }
            if (activeVideoFrameProcessorSerial_ != serial
                || activeVideoFrameProcessor_ != configured) {
                if (activeVideoFrameProcessor_
                    && videoFrameProcessorOpen_) {
                    activeVideoFrameProcessor_->close();
                }
                activeVideoFrameProcessor_ = std::move(configured);
                activeVideoFrameProcessorSerial_ = serial;
                videoFrameProcessorOpen_ = false;
                videoFrameProcessorBypass_ = false;
                activeVideoProcessorFormat_ = {};
            }
            if (!activeVideoFrameProcessor_) {
                return true;
            }

            const auto format = videoProcessorFormat(video);
            const bool formatChanged = !videoFrameProcessorOpen_
                || activeVideoProcessorFormat_.width != format.width
                || activeVideoProcessorFormat_.height != format.height
                || activeVideoProcessorFormat_.pixelFormat
                    != format.pixelFormat
                || activeVideoProcessorFormat_.hardwareFrame
                    != format.hardwareFrame
                || activeVideoProcessorFormat_.hardwareDevice
                    != format.hardwareDevice;
            if (formatChanged) {
                if (videoFrameProcessorOpen_) {
                    activeVideoFrameProcessor_->close();
                }
                const auto opened = activeVideoFrameProcessor_->open(format);
                videoFrameProcessorOpen_ = opened.success;
                videoFrameProcessorBypass_ =
                    opened.success && opened.bypass;
                activeVideoProcessorFormat_ =
                    opened.success ? format : VideoProcessorFormat {};
                if (!opened.success) {
                    eventCode = "video.processor.open";
                    eventDetail = opened.error.empty()
                        ? "The video frame processor could not be opened"
                        : opened.error;
                    eventError = AVERROR_EXTERNAL;
                    success = false;
                } else if (opened.bypass) {
                    eventCode = "video.processor.bypass";
                    eventDetail =
                        "The video frame processor explicitly bypassed the "
                        "current input format";
                }
            }
            if (success && !videoFrameProcessorBypass_) {
                const auto processed =
                    activeVideoFrameProcessor_->process(video);
                if (!processed.success) {
                    eventCode = "video.processor.process";
                    eventDetail = processed.error.empty()
                        ? "The video frame processor rejected a frame"
                        : processed.error;
                    eventError = AVERROR_EXTERNAL;
                    success = false;
                } else if (!processed.bypass) {
                    if (!processed.frame
                        || processed.frame.timestamp() != video.timestamp()
                        || processed.frame.duration() != video.duration()) {
                        eventCode = "video.processor.contract";
                        eventDetail =
                            "The video frame processor violated the "
                            "one-to-one timestamp contract";
                        eventError = AVERROR_INVALIDDATA;
                        success = false;
                    } else {
                        video = processed.frame;
                    }
                }
            }
            if (!success && activeVideoFrameProcessor_
                && videoFrameProcessorOpen_) {
                activeVideoFrameProcessor_->close();
                videoFrameProcessorOpen_ = false;
                videoFrameProcessorBypass_ = false;
                activeVideoProcessorFormat_ = {};
            }
        }
        if (!eventCode.empty()) {
            publishEvent({
                std::move(eventCode),
                std::move(eventDetail),
                eventError,
            });
        }
        return success
            && generation
                == presentationGeneration_.load(std::memory_order_acquire);
    }

    void resetVideoFrameProcessor()
    {
        std::string error;
        {
            std::lock_guard<std::mutex> processorLock(
                videoProcessorCallMutex_);
            if (!activeVideoFrameProcessor_ || !videoFrameProcessorOpen_) {
                return;
            }
            if (!videoFrameProcessorBypass_
                && !activeVideoFrameProcessor_->reset()) {
                error = "The video frame processor could not be reset";
                activeVideoFrameProcessor_->close();
                videoFrameProcessorOpen_ = false;
                videoFrameProcessorBypass_ = false;
                activeVideoProcessorFormat_ = {};
            }
        }
        if (!error.empty()) {
            publishEvent({
                "video.processor.reset",
                std::move(error),
                AVERROR_EXTERNAL,
            });
        }
    }

    bool drainVideoFrameProcessor()
    {
        bool drained = true;
        {
            std::lock_guard<std::mutex> processorLock(
                videoProcessorCallMutex_);
            if (activeVideoFrameProcessor_ && videoFrameProcessorOpen_
                && !videoFrameProcessorBypass_) {
                drained = activeVideoFrameProcessor_->drain();
            }
        }
        if (!drained) {
            publishEvent({
                "video.processor.drain",
                "The video frame processor could not finish the segment",
                AVERROR_EXTERNAL,
            });
        }
        return drained;
    }

    void closeVideoFrameProcessor() noexcept
    {
        std::lock_guard<std::mutex> processorLock(videoProcessorCallMutex_);
        if (activeVideoFrameProcessor_ && videoFrameProcessorOpen_) {
            activeVideoFrameProcessor_->close();
        }
        activeVideoFrameProcessor_.reset();
        activeVideoFrameProcessorSerial_ = 0;
        videoFrameProcessorOpen_ = false;
        videoFrameProcessorBypass_ = false;
        activeVideoProcessorFormat_ = {};
    }

    bool submitVideoFrame(
        VideoFrame video,
        int track,
        std::uint64_t generation,
        VideoFrameScheduler scheduler,
        bool forceImmediate,
        bool completesAccurateSeek,
        std::int64_t previousTimestamp)
    {
        if (scheduler) {
            const auto current = position();
            float rate = 1.0F;
            {
                std::lock_guard<std::mutex> lock(mutex_);
                rate = playbackRate_;
            }
            const auto lead = forceImmediate
                ? static_cast<std::int64_t>(0)
                : std::max<std::int64_t>(
                    0,
                    video.timestamp() - current);
            const auto deadline = Clock::now()
                + std::chrono::duration_cast<Clock::duration>(
                    std::chrono::duration<double, std::milli>(
                        static_cast<double>(lead)
                        / static_cast<double>(rate)));
            const auto deadlineNanoseconds =
                std::chrono::duration_cast<std::chrono::nanoseconds>(
                    deadline.time_since_epoch())
                    .count();
            if (scheduler(video, track, deadlineNanoseconds)) {
                deliveredVideoFrames_.fetch_add(
                    1,
                    std::memory_order_relaxed);
                recordPresentedVideo(
                    video.timestamp(),
                    generation,
                    completesAccurateSeek,
                    previousTimestamp);
                if (!forceImmediate && !hasActiveAudioDeviceClock()) {
                    resumeClockAfterOutput(generation, video.timestamp());
                }
                if (completesAccurateSeek) {
                    completeAccurateSeek(generation, video.timestamp());
                }
                return true;
            }
        }

        if (!processVideoFrame(video, generation)) {
            if (completesAccurateSeek) {
                failAccurateSeek(generation);
            }
            return false;
        }

        enqueuePresentation(PresentationItem {
            PresentationItem::Type::Video,
            {},
            std::move(video),
            {},
            track,
            generation,
            forceImmediate,
            completesAccurateSeek,
            previousTimestamp,
        });
        return true;
    }

    bool deliverFrame(
        Decoder& decoder,
        const AVFrame* frame,
        std::uint64_t generation)
    {
        if (decoder.hardwareFallbackUsed
            && !decoder.hardwareFallbackReported) {
            decoder.hardwareFallbackReported = true;
            publishEvent({
                "decoder.hardware.fallback",
                "Hardware pixel-format negotiation failed; using software "
                "decode",
                AVERROR(ENOSYS),
            });
        }

        std::int64_t timestampMs = 0;
        if (frame->best_effort_timestamp != AV_NOPTS_VALUE) {
            const auto absoluteUs = av_rescale_q(
                frame->best_effort_timestamp,
                decoder.stream->time_base,
                AV_TIME_BASE_Q);
            timestampMs =
                std::max<std::int64_t>(
                    0,
                    (absoluteUs - decoder.startTimeUs) / 1000);
        } else {
            std::lock_guard<std::mutex> lock(mutex_);
            timestampMs = currentPosition_;
        }

        const auto frameDuration = frame->duration;
        std::int64_t durationMs =
            toMilliseconds(frameDuration, decoder.stream->time_base);
        if (durationMs <= 0 && decoder.type == MediaType::Audio
            && frame->sample_rate > 0) {
            durationMs =
                static_cast<std::int64_t>(frame->nb_samples) * 1000
                / frame->sample_rate;
        }

        if (generation
            != presentationGeneration_.load(std::memory_order_acquire)) {
            return false;
        }

        std::int64_t rangeStart;
        std::int64_t rangeEnd;
        {
            std::lock_guard<std::mutex> lock(mutex_);
            rangeStart = rangeStart_;
            rangeEnd = rangeEnd_;
        }
        if (timestampMs < rangeStart) {
            return true;
        }
        if (rangeEnd != MediaEnd && timestampMs >= rangeEnd) {
            reachedRangeEnd_.store(true, std::memory_order_release);
            return true;
        }

        if (decoder.type == MediaType::Video) {
            VideoFrameScheduler scheduler;
            {
                std::lock_guard<std::mutex> lock(mutex_);
                if (generation
                    == presentationGeneration_.load(
                        std::memory_order_acquire)) {
                    scheduler = videoFrameScheduler_;
                }
            }
            decodedVideoFrames_.fetch_add(1, std::memory_order_relaxed);
            const bool scheduledSurfaceOutput = scheduler
                && isSurfaceOutputHardwareDevice(
                    decoder.hardwareDeviceType);
            if (!scheduledSurfaceOutput
                && !accurateSeekActive(generation)
                && !waitForVideoDecodeWindow(
                    timestampMs,
                    generation,
                    isSurfaceOutputHardwareDevice(
                        decoder.hardwareDeviceType))) {
                return false;
            }
            const auto hardwareDeviceType =
                frame->format == decoder.hardwarePixelFormat
                ? decoder.hardwareDeviceType
                : HardwareDeviceType::Unknown;
            auto video = detail::FrameFactory::video(
                frame,
                timestampMs,
                durationMs,
                hardwareDeviceType,
                hardwareDeviceType
                        != HardwareDeviceType::Unknown
                    ? decoder.hardwareNativeIdentity
                    : 0,
                hardwareDeviceType
                        != HardwareDeviceType::Unknown
                    ? decoder.hardwareSurfaceGeneration
                    : 0,
                hardwareDeviceType
                        != HardwareDeviceType::Unknown
                    ? decoder.contextLifetime
                    : std::shared_ptr<void> {});
            auto decision = classifyAccurateVideoFrame(
                std::move(video),
                generation);
            if (decision.action
                == AccurateVideoDecision::Action::Drop) {
                return true;
            }
            completeVideoPreroll(generation);
            return submitVideoFrame(
                std::move(decision.frame),
                decoder.trackIndex,
                generation,
                std::move(scheduler),
                decision.forceImmediate,
                decision.action
                    == AccurateVideoDecision::Action::DeliverSelected,
                decision.previousTimestamp);
        } else if (decoder.type == MediaType::Audio) {
            if (shouldDropNonVideoForAccurateSeek(
                    timestampMs,
                    generation)) {
                return true;
            }
            auto audio =
                detail::FrameFactory::audio(frame, timestampMs, durationMs);
            return enqueueAudioFrame(
                std::move(audio),
                decoder.trackIndex,
                generation);
        }
        return true;
    }

    enum class PresentationWaitResult {
        Cancelled,
        Ready,
        EarlierItemQueued,
    };

    bool hasEarlierQueuedPresentation(
        std::int64_t timestamp,
        std::uint64_t generation,
        bool includeSubtitles) const
    {
        std::lock_guard<std::mutex> lock(presentationMutex_);
        return std::any_of(
            presentationQueue_.begin(),
            presentationQueue_.end(),
            [timestamp, generation, includeSubtitles](
                const PresentationItem& item) {
                return item.generation == generation
                    && (includeSubtitles
                        || item.type != PresentationItem::Type::Subtitle)
                    && item.timestamp() < timestamp;
            });
    }

    PresentationWaitResult waitUntilPresentation(
        std::int64_t timestampMs,
        std::uint64_t generation,
        bool yieldToEarlierItem = true,
        bool claimOutputPrime = true,
        bool yieldToEarlierSubtitles = true)
    {
        std::unique_lock<std::mutex> lock(mutex_);
        while (true) {
            if (quitting_.load(std::memory_order_acquire)
                || generation
                    != presentationGeneration_.load(std::memory_order_acquire)
                || requestedState_ == State::Stopped || seekRequest_) {
                return PresentationWaitResult::Cancelled;
            }
            lock.unlock();
            const auto audioPosition = audioClockPosition();
            const bool earlierItemQueued =
                yieldToEarlierItem
                && hasEarlierQueuedPresentation(
                    timestampMs,
                    generation,
                    yieldToEarlierSubtitles);
            lock.lock();
            if (quitting_.load(std::memory_order_acquire)
                || generation
                    != presentationGeneration_.load(std::memory_order_acquire)
                || requestedState_ == State::Stopped || seekRequest_) {
                return PresentationWaitResult::Cancelled;
            }
            if (earlierItemQueued) {
                return PresentationWaitResult::EarlierItemQueued;
            }
            const auto current =
                audioPosition ? clampPositionLocked(*audioPosition)
                              : clockPositionLocked();
            if (requestedState_ != State::Playing) {
                controlChanged_.wait(lock, [this, generation] {
                    return quitting_.load(std::memory_order_acquire)
                        || generation
                            != presentationGeneration_.load(
                                std::memory_order_acquire)
                        || requestedState_ != State::Paused;
                });
                continue;
            }
            if (videoPrerollPending_
                && videoPrerollGeneration_ == generation) {
                if (Clock::now() >= videoPrerollDeadline_) {
                    videoPrerollPending_ = false;
                    videoPrerollGeneration_ = 0;
                    videoPrerollFrameCount_ = 0;
                } else {
                    controlChanged_.wait_until(
                        lock,
                        videoPrerollDeadline_);
                    continue;
                }
            }
            if (waitingForOutput_ && !outputWaitPrimed_) {
                if (claimOutputPrime) {
                    outputWaitPrimed_ = true;
                }
                return PresentationWaitResult::Ready;
            }
            const auto delta = timestampMs - current;
            if (delta <= 2) {
                return PresentationWaitResult::Ready;
            }
            const auto waitMs = std::clamp<std::int64_t>(
                static_cast<std::int64_t>(
                    static_cast<double>(delta) / playbackRate_),
                1,
                yieldToEarlierItem ? 10 : 100);
            controlChanged_.wait_for(lock, Milliseconds(waitMs));
        }
    }

    bool waitForVideoDecodeWindow(
        std::int64_t timestampMs,
        std::uint64_t generation,
        bool ownsSurfaceOutput)
    {
        std::unique_lock<std::mutex> lock(mutex_);
        while (true) {
            const bool accurateDecode = accurateSeek_
                && accurateSeek_->generation == generation;
            if (quitting_.load(std::memory_order_acquire)
                || generation
                    != presentationGeneration_.load(std::memory_order_acquire)
                || (!accurateDecode
                    && requestedState_ != State::Playing)
                || seekRequest_) {
                return false;
            }
            if (accurateDecode) {
                return true;
            }
            lock.unlock();
            const auto audioPosition = audioClockPosition();
            lock.lock();
            if (quitting_.load(std::memory_order_acquire)
                || generation
                    != presentationGeneration_.load(std::memory_order_acquire)
                || requestedState_ != State::Playing || seekRequest_) {
                return false;
            }
            if (waitingForOutput_ && !outputWaitPrimed_) {
                return true;
            }
            const auto current =
                audioPosition ? clampPositionLocked(*audioPosition)
                              : clockPositionLocked();
            const auto lead = timestampMs - current;
            const auto maximumLead = ownsSurfaceOutput
                ? kMaximumSurfaceOutputLeadMilliseconds
                : kMaximumVideoDecodeLeadMilliseconds;
            if (lead <= maximumLead) {
                return true;
            }
            controlChanged_.wait_for(
                lock,
                Milliseconds(std::clamp<std::int64_t>(
                    lead - maximumLead,
                    1,
                    50)));
        }
    }

    bool waitForSurfacePacketDecodeWindow(
        const Decoder& decoder,
        const AVPacket* packet,
        std::uint64_t generation)
    {
        if (!packet || !decoder.stream) {
            return true;
        }
        const std::int64_t packetTimestamp =
            packet->dts != AV_NOPTS_VALUE ? packet->dts : packet->pts;
        if (packetTimestamp == AV_NOPTS_VALUE) {
            return true;
        }
        const auto absoluteUs = av_rescale_q(
            packetTimestamp,
            decoder.stream->time_base,
            AV_TIME_BASE_Q);
        const auto timestampMs = std::max<std::int64_t>(
            0,
            (absoluteUs - decoder.startTimeUs) / 1000);

        std::unique_lock<std::mutex> lock(mutex_);
        while (true) {
            const bool accurateDecode = accurateSeek_
                && accurateSeek_->generation == generation;
            if (quitting_.load(std::memory_order_acquire)
                || generation
                    != presentationGeneration_.load(std::memory_order_acquire)
                || (!accurateDecode
                    && requestedState_ != State::Playing)
                || seekRequest_) {
                return false;
            }
            if (accurateDecode) {
                return true;
            }
            lock.unlock();
            const auto audioPosition = audioClockPosition();
            lock.lock();
            if (quitting_.load(std::memory_order_acquire)
                || generation
                    != presentationGeneration_.load(std::memory_order_acquire)
                || requestedState_ != State::Playing || seekRequest_) {
                return false;
            }
            if (waitingForOutput_ && !outputWaitPrimed_) {
                return true;
            }
            const auto current =
                audioPosition ? clampPositionLocked(*audioPosition)
                              : clockPositionLocked();
            const auto lead = timestampMs - current;
            if (lead
                <= kMaximumSurfacePacketDecodeLeadMilliseconds) {
                return true;
            }
            controlChanged_.wait_for(
                lock,
                Milliseconds(std::clamp<std::int64_t>(
                    lead
                        - kMaximumSurfacePacketDecodeLeadMilliseconds,
                    1,
                    20)));
        }
    }

    bool enqueueAudioFrame(
        AudioFrame frame,
        int track,
        std::uint64_t generation)
    {
        const auto duration =
            std::max<std::int64_t>(1, frame.duration());
        while (true) {
            if (quitting_.load(std::memory_order_acquire)
                || generation
                    != presentationGeneration_.load(std::memory_order_acquire)) {
                return false;
            }
            {
                std::lock_guard<std::mutex> lock(mutex_);
                if (requestedState_ != State::Playing || seekRequest_) {
                    return false;
                }
            }

            std::unique_lock<std::mutex> lock(audioQueueMutex_);
            const bool fits =
                audioQueue_.empty()
                || queuedAudioDuration_ + duration
                    <= kMaximumQueuedAudioMilliseconds;
            if (fits) {
                audioQueue_.push_back({
                    std::move(frame),
                    track,
                    generation,
                    duration,
                });
                queuedAudioDuration_ += duration;
                lock.unlock();
                audioQueueChanged_.notify_one();
                return true;
            }
            audioQueueSpace_.wait_for(lock, Milliseconds(20));
        }
    }

    void enqueuePresentation(PresentationItem item)
    {
        if (item.generation
            != presentationGeneration_.load(std::memory_order_acquire)) {
            return;
        }
        const auto livePolicy = livePlaybackPolicy();
        std::lock_guard<std::mutex> lock(presentationMutex_);
        const auto hardMaximumQueuedVideoFrames =
            item.type == PresentationItem::Type::Video
                && item.video.hasHardwareFrame()
            ? kMaximumQueuedHardwareVideoFrames
            : kMaximumQueuedSoftwareVideoFrames;
        const auto maximumQueuedVideoFrames = livePolicy.enabled
            ? std::min<std::size_t>(
                hardMaximumQueuedVideoFrames,
                livePolicy.maximumQueuedVideoFrames)
            : hardMaximumQueuedVideoFrames;
        if (item.type == PresentationItem::Type::Video
            && queuedVideoFrames_ >= maximumQueuedVideoFrames
            && !livePolicy.enabled) {
            // Keep the contiguous near-term presentation window. Evicting its
            // oldest frame lets decode bursts, especially after seek, replace
            // frames that are about to be shown with farther-future frames.
            videoQueueOverflowDrops_.fetch_add(
                1,
                std::memory_order_relaxed);
            return;
        }
        while (item.type == PresentationItem::Type::Video
               && queuedVideoFrames_ >= maximumQueuedVideoFrames) {
            // A latency-sensitive session instead retains the newest bounded
            // window. The loop also applies a smaller runtime queue-depth
            // update without waiting for a generation reset. Do not let a
            // reordered older arrival displace a newer frame already waiting
            // for presentation.
            const auto oldestVideo = std::find_if(
                presentationQueue_.begin(),
                presentationQueue_.end(),
                [](const PresentationItem& candidate) {
                    return candidate.type == PresentationItem::Type::Video;
                });
            videoQueueOverflowDrops_.fetch_add(
                1,
                std::memory_order_relaxed);
            lowLatencyVideoQueueDrops_.fetch_add(
                1,
                std::memory_order_relaxed);
            if (oldestVideo == presentationQueue_.end()
                || item.timestamp() <= oldestVideo->timestamp()) {
                return;
            }
            presentationQueue_.erase(oldestVideo);
            --queuedVideoFrames_;
        }
        if (item.type == PresentationItem::Type::Subtitle
            && queuedSubtitleFrames_ >= kMaximumQueuedSubtitleFrames) {
            return;
        }
        const auto insertion = std::upper_bound(
            presentationQueue_.begin(),
            presentationQueue_.end(),
            item.timestamp(),
            [](std::int64_t timestamp, const PresentationItem& candidate) {
                return timestamp < candidate.timestamp();
            });
        if (item.type == PresentationItem::Type::Video) {
            ++queuedVideoFrames_;
            auto maximum = maximumQueuedVideoFrames_.load(
                std::memory_order_relaxed);
            while (maximum < queuedVideoFrames_
                   && !maximumQueuedVideoFrames_.compare_exchange_weak(
                       maximum,
                       queuedVideoFrames_,
                       std::memory_order_relaxed)) {
            }
        } else if (item.type == PresentationItem::Type::Subtitle) {
            ++queuedSubtitleFrames_;
        }
        presentationQueue_.insert(insertion, std::move(item));
        presentationChanged_.notify_one();
    }

    bool requestedPlaying() const
    {
        std::lock_guard<std::mutex> lock(mutex_);
        return requestedState_ == State::Playing
            && currentState_ == State::Playing && !seekRequest_
            && !seekCompletion_;
    }

    bool hasActiveAudioDeviceClock() const
    {
        std::lock_guard<std::mutex> lock(mutex_);
        return audioSinkOpen_ && audioSinkHasClock_;
    }

    bool needsAudioPresentation() const
    {
        std::lock_guard<std::mutex> lock(mutex_);
        return !audioSinkOpen_ || !audioSinkHasClock_
            || static_cast<bool>(audioFrameCallback_);
    }

    void runAudioOutput()
    {
        while (!quitting_.load(std::memory_order_acquire)) {
            QueuedAudioFrame queued;
            {
                std::unique_lock<std::mutex> lock(audioQueueMutex_);
                audioQueueChanged_.wait(lock, [this] {
                    return quitting_.load(std::memory_order_acquire)
                        || !audioQueue_.empty()
                        || outputClockPollRequested_.load(
                            std::memory_order_acquire);
                });
                if (quitting_.load(std::memory_order_acquire)) {
                    break;
                }
                if (audioQueue_.empty()) {
                    lock.unlock();
                    refreshAudioClockWhileWaiting();
                    lock.lock();
                    audioQueueChanged_.wait_for(
                        lock,
                        Milliseconds(20),
                        [this] {
                            return quitting_.load(
                                       std::memory_order_acquire)
                                || !audioQueue_.empty()
                                || !outputClockPollRequested_.load(
                                    std::memory_order_acquire);
                        });
                    continue;
                }
                if (!requestedPlaying()) {
                    audioQueueChanged_.wait_for(lock, Milliseconds(20));
                    continue;
                }
                queued = std::move(audioQueue_.front());
                audioQueue_.pop_front();
                queuedAudioDuration_ =
                    std::max<std::int64_t>(
                        0,
                        queuedAudioDuration_ - queued.duration);
                audioFrameInFlight_ = true;
            }
            audioQueueSpace_.notify_all();

            bool accepted = queued.generation
                == presentationGeneration_.load(std::memory_order_acquire);
            if (accepted) {
                accepted = waitForVideoPreroll(queued.generation);
            }
            if (accepted) {
                deliverAudioToSink(queued.frame, queued.generation);
                if (!hasActiveAudioDeviceClock()) {
                    accepted =
                        waitUntilPresentation(
                            queued.frame.timestamp(),
                            queued.generation,
                            true,
                            false,
                            false)
                        == PresentationWaitResult::Ready;
                }
            }
            if (accepted
                && queued.generation
                    == presentationGeneration_.load(
                        std::memory_order_acquire)
                && needsAudioPresentation()) {
                enqueuePresentation(PresentationItem {
                    PresentationItem::Type::Audio,
                    std::move(queued.frame),
                    {},
                    {},
                    queued.track,
                    queued.generation,
                });
            }

            {
                std::lock_guard<std::mutex> lock(audioQueueMutex_);
                audioFrameInFlight_ = false;
            }
            audioQueueSpace_.notify_all();
            audioQueueDrained_.notify_all();
        }

        {
            std::lock_guard<std::mutex> lock(audioQueueMutex_);
            audioFrameInFlight_ = false;
        }
        audioQueueDrained_.notify_all();
    }

    bool hasNewerQueuedVideo(
        std::int64_t timestamp,
        std::int64_t current,
        std::uint64_t generation,
        std::int64_t lateThreshold,
        bool requireTimely) const
    {
        std::lock_guard<std::mutex> lock(presentationMutex_);
        return std::any_of(
            presentationQueue_.begin(),
            presentationQueue_.end(),
            [timestamp,
             current,
             generation,
             lateThreshold,
             requireTimely](
                const PresentationItem& item) {
                return item.type == PresentationItem::Type::Video
                    && item.generation == generation
                    && item.timestamp() > timestamp
                    && (!requireTimely
                        || current - item.timestamp() <= lateThreshold);
            });
    }

    void present(PresentationItem& item)
    {
        if (item.generation
            != presentationGeneration_.load(std::memory_order_acquire)) {
            return;
        }

        if (item.type == PresentationItem::Type::Audio) {
            AudioFrameCallback callback;
            {
                std::lock_guard<std::mutex> lock(mutex_);
                if (item.generation
                    != presentationGeneration_.load(
                        std::memory_order_acquire)) {
                    return;
                }
                currentPosition_ =
                    std::max(currentPosition_, item.audio.timestamp());
                callback = audioFrameCallback_;
            }
            if (callback) {
                callback(item.audio, item.track);
            }
            if (!hasActiveAudioDeviceClock()) {
                resumeClockAfterOutput(
                    item.generation,
                    item.audio.timestamp());
            }
            return;
        }

        if (item.type == PresentationItem::Type::Subtitle) {
            SubtitleFrameCallback callback;
            {
                std::lock_guard<std::mutex> lock(mutex_);
                if (item.generation
                    != presentationGeneration_.load(
                        std::memory_order_acquire)) {
                    return;
                }
                callback = subtitleFrameCallback_;
            }
            if (callback) {
                callback(item.subtitle, item.track);
            }
            return;
        }

        VideoFrameCallback frameCallback;
        {
            std::lock_guard<std::mutex> lock(mutex_);
            if (item.generation
                != presentationGeneration_.load(std::memory_order_acquire)) {
                return;
            }
            frameCallback = videoFrameCallback_;
        }
        recordPresentedVideo(
            item.video.timestamp(),
            item.generation,
            item.completesAccurateSeek,
            item.previousVideoTimestamp);

        auto frameSnapshot = std::make_shared<VideoFrameSnapshot>();
        frameSnapshot->frame = item.video;
        frameSnapshot->generation = item.generation;
        frameSnapshot->sequence =
            nextVideoFrameSequence_.fetch_add(1, std::memory_order_relaxed);
        {
            std::lock_guard<std::mutex> lock(videoFrameSnapshotMutex_);
            if (item.generation
                != presentationGeneration_.load(std::memory_order_acquire)) {
                return;
            }
            std::shared_ptr<const VideoFrameSnapshot> immutable =
                std::move(frameSnapshot);
            std::atomic_store_explicit(
                &currentVideoFrameSnapshot_,
                std::move(immutable),
                std::memory_order_release);
        }

        const auto bindings = std::atomic_load_explicit(
            &renderBindings_,
            std::memory_order_acquire);
        const auto renderCallback = bindings
            ? bindings->callback
            : RenderCallback {};
        std::vector<void*> renderKeys;
        if (bindings) {
            const bool scheduleLegacyRenderer = bindings->legacyRenderer
                && bindings->renderAPIs.find(nullptr)
                    == bindings->renderAPIs.end();
            renderKeys.reserve(
                bindings->renderAPIs.size()
                + (scheduleLegacyRenderer ? 1U : 0U));
            if (scheduleLegacyRenderer) {
                renderKeys.push_back(nullptr);
            }
            for (const auto& entry : bindings->renderAPIs) {
                renderKeys.push_back(entry.first);
            }
        }
        if (frameCallback) {
            frameCallback(item.video, item.track);
        }
        deliveredVideoFrames_.fetch_add(1, std::memory_order_relaxed);
        if (!renderCallback
            || item.generation
                != presentationGeneration_.load(std::memory_order_acquire)) {
            if (!item.forceImmediate && !hasActiveAudioDeviceClock()) {
                resumeClockAfterOutput(
                    item.generation,
                    item.video.timestamp());
            }
            if (item.completesAccurateSeek) {
                completeAccurateSeek(
                    item.generation,
                    item.video.timestamp());
            }
            return;
        }
        if (renderKeys.empty()) {
            renderCallback(nullptr);
        } else {
            for (void* key : renderKeys) {
                renderCallback(key);
            }
        }
        if (!item.forceImmediate && !hasActiveAudioDeviceClock()) {
            resumeClockAfterOutput(
                item.generation,
                item.video.timestamp());
        }
        if (item.completesAccurateSeek) {
            completeAccurateSeek(
                item.generation,
                item.video.timestamp());
        }
    }

    void runPresentation()
    {
        while (!quitting_.load(std::memory_order_acquire)) {
            PresentationItem item;
            {
                std::unique_lock<std::mutex> lock(presentationMutex_);
                const auto queueWaitStart = Clock::now();
                presentationChanged_.wait(lock, [this] {
                    return quitting_.load(std::memory_order_acquire)
                        || !presentationQueue_.empty();
                });
                const auto queueWait =
                    std::chrono::duration_cast<Milliseconds>(
                        Clock::now() - queueWaitStart)
                        .count();
                if (queueWait > 60
                    && deliveredVideoFrames_.load(
                        std::memory_order_relaxed) > 0) {
                    videoPresentationStarvations_.fetch_add(
                        1,
                        std::memory_order_relaxed);
                    auto maximum =
                        maximumVideoPresentationStarvationMilliseconds_.load(
                            std::memory_order_relaxed);
                    while (maximum
                               < static_cast<std::uint64_t>(queueWait)
                           && !maximumVideoPresentationStarvationMilliseconds_
                                   .compare_exchange_weak(
                                       maximum,
                                       static_cast<std::uint64_t>(queueWait),
                                       std::memory_order_relaxed)) {
                    }
                }
                if (quitting_.load(std::memory_order_acquire)) {
                    break;
                }
                item = std::move(presentationQueue_.front());
                presentationQueue_.pop_front();
                if (item.type == PresentationItem::Type::Video) {
                    --queuedVideoFrames_;
                } else if (item.type == PresentationItem::Type::Subtitle) {
                    --queuedSubtitleFrames_;
                }
                presentationInFlight_ = true;
            }

            if (item.completesAccurateSeek
                && accurateSelectionShouldPresentImmediately(
                    item.generation)) {
                item.forceImmediate = true;
            }
            const auto waitResult = item.forceImmediate
                ? PresentationWaitResult::Ready
                : waitUntilPresentation(
                    item.timestamp(),
                    item.generation,
                    true,
                    item.type != PresentationItem::Type::Subtitle);
            if (waitResult
                == PresentationWaitResult::EarlierItemQueued) {
                enqueuePresentation(std::move(item));
            } else if (waitResult == PresentationWaitResult::Ready) {
                const auto current = position();
                const auto livePolicy = livePlaybackPolicy();
                const auto lateThreshold = livePolicy.enabled
                    ? livePolicy.lateVideoFrameThresholdMilliseconds
                    : kLateVideoFrameThresholdMilliseconds;
                const bool dropLateVideo =
                    item.type == PresentationItem::Type::Video
                    && !item.completesAccurateSeek
                    && current - item.timestamp()
                        > lateThreshold
                    && hasNewerQueuedVideo(
                        item.timestamp(),
                        current,
                        item.generation,
                        lateThreshold,
                        !livePolicy.enabled);
                if (!dropLateVideo) {
                    present(item);
                } else {
                    lateVideoDrops_.fetch_add(
                        1,
                        std::memory_order_relaxed);
                }
            }

            {
                std::lock_guard<std::mutex> lock(presentationMutex_);
                presentationInFlight_ = false;
            }
            presentationDrained_.notify_all();
        }

        {
            std::lock_guard<std::mutex> lock(presentationMutex_);
            presentationInFlight_ = false;
        }
        presentationDrained_.notify_all();
    }

    void invalidateAudioClock()
    {
        {
            std::lock_guard<std::mutex> lock(audioClockMutex_);
            cachedAudioClock_ = {};
        }
        controlChanged_.notify_all();
        presentationChanged_.notify_all();
    }

    void invalidatePlaybackGeneration()
    {
        {
            std::lock_guard<std::mutex> lock(videoFrameSnapshotMutex_);
            presentationGeneration_.fetch_add(1, std::memory_order_acq_rel);
            clearCurrentVideoFrameSnapshot();
        }
    }

    void notifyPlaybackQueueInvalidation()
    {
        resetPacketBuffering();
        invalidateAudioClock();
        audioQueueChanged_.notify_all();
        audioQueueSpace_.notify_all();
        audioQueueDrained_.notify_all();
        presentationChanged_.notify_all();
        presentationDrained_.notify_all();
        videoPacketChanged_.notify_all();
        videoPacketSpace_.notify_all();
        videoPacketDrained_.notify_all();
        audioPacketChanged_.notify_all();
        audioPacketSpace_.notify_all();
        audioPacketDrained_.notify_all();
        packetBufferChanged_.notify_all();
    }

    void invalidatePlaybackQueues()
    {
        invalidatePlaybackGeneration();
        notifyPlaybackQueueInvalidation();
    }

    void resetPlaybackQueues()
    {
        invalidatePlaybackQueues();
        resetVideoPacketQueue();
        resetAudioPacketQueue();
        resetVideoFrameProcessor();
        queuedMemoryPacketBytes_.store(0, std::memory_order_relaxed);
        packetDiskStore_->clear();
        diskCacheUnavailable_.store(false, std::memory_order_release);
        audioPacketInputEnded_.store(false, std::memory_order_release);
        videoPacketInputEnded_.store(false, std::memory_order_release);
        reachedRangeEnd_.store(false, std::memory_order_release);
        outputClockPollRequested_.store(
            false,
            std::memory_order_release);
        {
            std::lock_guard<std::mutex> lock(audioQueueMutex_);
            audioQueue_.clear();
            queuedAudioDuration_ = 0;
        }
        {
            std::lock_guard<std::mutex> lock(presentationMutex_);
            presentationQueue_.clear();
            queuedVideoFrames_ = 0;
            queuedSubtitleFrames_ = 0;
        }
        {
            std::lock_guard<std::mutex> lock(mutex_);
            waitingForOutput_ = false;
            outputWaitPrimed_ = false;
            outputWaitGeneration_ = 0;
            outputWaitRequiresDeviceClock_ = false;
            videoPrerollPending_ = false;
            videoPrerollGeneration_ = 0;
            videoPrerollFrameCount_ = 0;
        }
        audioQueueChanged_.notify_all();
        audioQueueSpace_.notify_all();
        audioQueueDrained_.notify_all();
        presentationChanged_.notify_all();
        presentationDrained_.notify_all();
    }

    void resetPlaybackStatistics() noexcept
    {
        decodedVideoFrames_.store(0, std::memory_order_relaxed);
        videoQueueOverflowDrops_.store(0, std::memory_order_relaxed);
        lateVideoDrops_.store(0, std::memory_order_relaxed);
        deliveredVideoFrames_.store(0, std::memory_order_relaxed);
        maximumQueuedVideoFrames_.store(0, std::memory_order_relaxed);
        videoPresentationStarvations_.store(0, std::memory_order_relaxed);
        maximumVideoPresentationStarvationMilliseconds_.store(
            0,
            std::memory_order_relaxed);
        lowLatencyVideoQueueDrops_.store(0, std::memory_order_relaxed);
        networkRecoveryAttempts_.store(0, std::memory_order_relaxed);
        successfulNetworkRecoveries_.store(0, std::memory_order_relaxed);
        failedNetworkRecoveries_.store(0, std::memory_order_relaxed);
    }

    void beginOutputWait(
        std::uint64_t generation,
        std::int64_t position)
    {
        bool publishBuffering = false;
        {
            std::lock_guard<std::mutex> lock(mutex_);
            if (generation
                    != presentationGeneration_.load(
                        std::memory_order_acquire)
                || currentState_ != State::Playing
                || requestedState_ != State::Playing
                || seekRequest_) {
                return;
            }
            primeOutputWaitLocked(
                position,
                generation,
                false,
                audioSinkOpen_ && audioSinkHasClock_);
            publishBuffering = status_ == MediaStatus::Loaded;
        }
        if (publishBuffering) {
            publishStatus(MediaStatus::Buffering);
        }
        controlChanged_.notify_all();
        presentationChanged_.notify_all();
    }

    void primeOutputWaitLocked(
        std::int64_t position,
        std::uint64_t generation,
        bool waitForVideo,
        bool requireDeviceClock)
    {
        currentPosition_ = clampPositionLocked(position);
        resetClockLocked(currentPosition_);
        waitingForOutput_ = true;
        outputWaitPrimed_ = false;
        outputWaitGeneration_ = generation;
        outputWaitRequiresDeviceClock_ = requireDeviceClock;
        outputClockPollRequested_.store(
            requireDeviceClock,
            std::memory_order_release);
        if (requireDeviceClock) {
            audioQueueChanged_.notify_all();
        }
        videoPrerollPending_ = waitForVideo;
        videoPrerollGeneration_ = waitForVideo ? generation : 0;
        videoPrerollFrameCount_ = 0;
        videoPrerollDeadline_ =
            Clock::now()
            + Milliseconds(kMaximumVideoPrerollWaitMilliseconds);
    }

    bool waitForVideoPreroll(std::uint64_t generation)
    {
        std::unique_lock<std::mutex> lock(mutex_);
        while (videoPrerollPending_
               && videoPrerollGeneration_ == generation) {
            if (quitting_.load(std::memory_order_acquire)
                || generation
                    != presentationGeneration_.load(
                        std::memory_order_acquire)
                || requestedState_ != State::Playing
                || seekRequest_) {
                return false;
            }
            if (Clock::now() >= videoPrerollDeadline_) {
                videoPrerollPending_ = false;
                videoPrerollGeneration_ = 0;
                videoPrerollFrameCount_ = 0;
                return true;
            }
            controlChanged_.wait_until(lock, videoPrerollDeadline_);
        }
        return generation
            == presentationGeneration_.load(std::memory_order_acquire)
            && requestedState_ == State::Playing && !seekRequest_;
    }

    void completeVideoPreroll(std::uint64_t generation)
    {
        {
            std::lock_guard<std::mutex> lock(mutex_);
            if (!videoPrerollPending_
                || videoPrerollGeneration_ != generation) {
                return;
            }
            ++videoPrerollFrameCount_;
            if (videoPrerollFrameCount_
                < kMinimumVideoPrerollFrames) {
                return;
            }
            videoPrerollPending_ = false;
            videoPrerollGeneration_ = 0;
            videoPrerollFrameCount_ = 0;
        }
        controlChanged_.notify_all();
        audioQueueChanged_.notify_all();
    }

    void resumeClockAfterOutput(
        std::uint64_t generation,
        std::int64_t position)
    {
        bool publishLoaded = false;
        {
            std::lock_guard<std::mutex> lock(mutex_);
            if (!waitingForOutput_
                || outputWaitGeneration_ != generation
                || seekRequest_) {
                return;
            }
            currentPosition_ = std::max(
                currentPosition_,
                clampPositionLocked(position));
            waitingForOutput_ = false;
            outputWaitPrimed_ = false;
            outputWaitGeneration_ = 0;
            outputWaitRequiresDeviceClock_ = false;
            outputClockPollRequested_.store(
                false,
                std::memory_order_release);
            videoPrerollPending_ = false;
            videoPrerollGeneration_ = 0;
            videoPrerollFrameCount_ = 0;
            resetClockLocked(currentPosition_);
            publishLoaded = status_ == MediaStatus::Buffering;
        }
        if (publishLoaded) {
            publishStatus(MediaStatus::Loaded);
        }
    }

    bool waitForAudioQueueDrained(std::uint64_t generation)
    {
        while (!quitting_.load(std::memory_order_acquire)
               && generation
                   == presentationGeneration_.load(std::memory_order_acquire)) {
            {
                std::lock_guard<std::mutex> lock(audioQueueMutex_);
                if (audioQueue_.empty() && !audioFrameInFlight_) {
                    return true;
                }
            }
            if (!requestedPlaying()) {
                return false;
            }
            std::unique_lock<std::mutex> lock(audioQueueMutex_);
            audioQueueDrained_.wait_for(lock, Milliseconds(20));
        }
        return false;
    }

    bool waitForPresentationDrained(std::uint64_t generation)
    {
        while (!quitting_.load(std::memory_order_acquire)
               && generation
                   == presentationGeneration_.load(std::memory_order_acquire)) {
            {
                std::lock_guard<std::mutex> lock(presentationMutex_);
                if (presentationQueue_.empty() && !presentationInFlight_) {
                    return true;
                }
            }
            if (!requestedPlaying()) {
                return false;
            }
            std::unique_lock<std::mutex> lock(presentationMutex_);
            presentationDrained_.wait_for(lock, Milliseconds(20));
        }
        return false;
    }

    void cacheAudioClockSample(
        const AudioSinkClock& value,
        std::uint64_t serial,
        std::int64_t submittedUntil,
        std::uint64_t generation,
        double playbackRate,
        std::optional<std::int64_t> submittedStart)
    {
        if (generation
            != presentationGeneration_.load(std::memory_order_acquire)) {
            return;
        }
        std::optional<std::int64_t> validPosition;
        {
            std::lock_guard<std::mutex> lock(audioClockMutex_);
            if (cachedAudioClock_.sinkSerial != serial) {
                cachedAudioClock_ = {};
                cachedAudioClock_.sinkSerial = serial;
            }
            if (!cachedAudioClock_.hasRateAnchor && submittedStart) {
                cachedAudioClock_.rawAnchor = *submittedStart;
                cachedAudioClock_.mediaAnchor = *submittedStart;
                cachedAudioClock_.playbackRate = playbackRate;
                cachedAudioClock_.hasRateAnchor = true;
            }
            cachedAudioClock_.submittedUntil = std::max(
                cachedAudioClock_.submittedUntil,
                submittedUntil);
            if (value.valid) {
                const auto now = Clock::now();
                if (!cachedAudioClock_.hasRateAnchor) {
                    cachedAudioClock_.rawAnchor =
                        value.positionMilliseconds;
                    cachedAudioClock_.mediaAnchor =
                        value.positionMilliseconds;
                    cachedAudioClock_.playbackRate = playbackRate;
                    cachedAudioClock_.hasRateAnchor = true;
                }
                const auto rawDelta = value.positionMilliseconds
                    - cachedAudioClock_.rawAnchor;
                auto position = std::max<std::int64_t>(
                    0,
                    cachedAudioClock_.mediaAnchor
                        + static_cast<std::int64_t>(std::llround(
                            static_cast<double>(rawDelta)
                            * cachedAudioClock_.playbackRate)));
                if (cachedAudioClock_.valid) {
                    const auto elapsed = std::max<std::int64_t>(
                        0,
                        std::chrono::duration_cast<Milliseconds>(
                            now - cachedAudioClock_.sampledAt)
                            .count());
                    auto extrapolated = cachedAudioClock_.position
                        + static_cast<std::int64_t>(std::llround(
                            static_cast<double>(elapsed)
                            * cachedAudioClock_.playbackRate));
                    if (cachedAudioClock_.submittedUntil > 0) {
                        extrapolated = std::min(
                            extrapolated,
                            cachedAudioClock_.submittedUntil);
                    }
                    position = std::max(position, extrapolated);
                }
                if (cachedAudioClock_.submittedUntil > 0) {
                    position = std::min(
                        position,
                        cachedAudioClock_.submittedUntil);
                }
                cachedAudioClock_.valid = true;
                cachedAudioClock_.position = position;
                cachedAudioClock_.submittedUntil = std::max(
                    cachedAudioClock_.submittedUntil,
                    cachedAudioClock_.position);
                cachedAudioClock_.sampledAt = now;
                validPosition = cachedAudioClock_.position;
            }
        }
        if (validPosition) {
            resumeClockAfterOutput(generation, *validPosition);
        } else if (submittedUntil > 0) {
            bool useFallbackClock = false;
            {
                std::lock_guard<std::mutex> lock(mutex_);
                useFallbackClock = waitingForOutput_
                    && outputWaitGeneration_ == generation
                    && !outputWaitRequiresDeviceClock_;
            }
            if (useFallbackClock) {
                resumeClockAfterOutput(generation, submittedUntil);
            }
        }
        controlChanged_.notify_all();
        presentationChanged_.notify_all();
    }

    void completeCachedAudioClock()
    {
        std::optional<std::int64_t> completedPosition;
        {
            std::lock_guard<std::mutex> lock(audioClockMutex_);
            if (cachedAudioClock_.valid) {
                cachedAudioClock_.position = std::max(
                    cachedAudioClock_.position,
                    cachedAudioClock_.submittedUntil);
                cachedAudioClock_.sampledAt = Clock::now();
                completedPosition = cachedAudioClock_.position;
            }
        }
        std::uint64_t generation = 0;
        std::int64_t fallbackPosition = 0;
        {
            std::lock_guard<std::mutex> lock(mutex_);
            // Once all decoded audio has been submitted, the device clock
            // must no longer cap trailing video or frame callbacks at the
            // last audio timestamp. Continue on the monotonic playback clock
            // while the remaining presentation queue drains.
            audioSinkHasClock_ = false;
            generation = presentationGeneration_.load(
                std::memory_order_acquire);
            fallbackPosition = currentPosition_;
        }
        resumeClockAfterOutput(
            generation,
            completedPosition.value_or(fallbackPosition));
        controlChanged_.notify_all();
        presentationChanged_.notify_all();
    }

    void refreshAudioClockWhileWaiting()
    {
        std::shared_ptr<AudioSink> sink;
        std::uint64_t serial = 0;
        std::uint64_t generation = 0;
        double playbackRate = 1.0;
        {
            std::lock_guard<std::mutex> lock(mutex_);
            if (!waitingForOutput_ || !outputWaitRequiresDeviceClock_
                || !audioSinkOpen_ || !audioSinkHasClock_) {
                outputClockPollRequested_.store(
                    false,
                    std::memory_order_release);
                return;
            }
            sink = activeAudioSink_;
            serial = appliedAudioSinkSerial_;
            generation = outputWaitGeneration_;
            playbackRate = playbackRate_;
        }
        if (!sink) {
            outputClockPollRequested_.store(
                false,
                std::memory_order_release);
            return;
        }
        AudioSinkClock clock;
        {
            std::lock_guard<std::mutex> sinkLock(audioSinkCallMutex_);
            clock = sink->clock();
        }
        cacheAudioClockSample(
            clock,
            serial,
            0,
            generation,
            playbackRate,
            std::nullopt);
    }

    void replaceAudioSink(
        const std::shared_ptr<AudioSink>& sink,
        const std::shared_ptr<AudioFrameConverter>& converter,
        const std::shared_ptr<AudioTimeStretcher>& stretcher,
        const std::shared_ptr<AudioFrameProcessor>& processor,
        std::uint64_t serial)
    {
        invalidateAudioClock();
        std::shared_ptr<AudioSink> previous;
        std::shared_ptr<AudioFrameConverter> previousConverter;
        std::shared_ptr<AudioTimeStretcher> previousStretcher;
        std::shared_ptr<AudioFrameProcessor> previousProcessor;
        bool previousOpen = false;
        bool previousConverterOpen = false;
        bool previousStretcherOpen = false;
        bool previousProcessorOpen = false;
        {
            std::lock_guard<std::mutex> lock(mutex_);
            previous = activeAudioSink_;
            previousConverter = activeAudioFrameConverter_;
            previousStretcher = activeAudioTimeStretcher_;
            previousProcessor = activeAudioFrameProcessor_;
            previousOpen = audioSinkOpen_;
            previousConverterOpen = audioFrameConverterOpen_;
            previousStretcherOpen = audioTimeStretcherOpen_;
            previousProcessorOpen = audioFrameProcessorOpen_;
            activeAudioSink_.reset();
            activeAudioFrameConverter_.reset();
            activeAudioTimeStretcher_.reset();
            activeAudioFrameProcessor_.reset();
            audioSinkOpen_ = false;
            audioSinkOpenAttempted_ = false;
            audioSinkHasClock_ = false;
            audioFrameConverterOpen_ = false;
            audioTimeStretcherOpen_ = false;
            audioFrameProcessorOpen_ = false;
        }

        if (previous || previousConverter || previousStretcher
            || previousProcessor) {
            std::lock_guard<std::mutex> sinkLock(audioSinkCallMutex_);
            if (previous) {
                previous->setEventCallback({});
                if (previousOpen) {
                    previous->flush();
                }
            }
            if (previousStretcher && previousStretcherOpen) {
                previousStretcher->reset();
                previousStretcher->close();
            }
            if (previousProcessor && previousProcessorOpen) {
                previousProcessor->reset();
                previousProcessor->close();
            }
            audioProcessorInputSamples_ = 0;
            audioProcessorOutputSamples_ = 0;
            audioProcessorLastTimestamp_ = 0;
            audioProcessorHasTimestamp_ = false;
            audioProcessorFormat_ = {};
            if (previousConverter && previousConverterOpen) {
                previousConverter->reset();
                previousConverter->close();
            }
            if (previous && previousOpen) {
                previous->close();
            }
        }

        {
            std::lock_guard<std::mutex> lock(mutex_);
            activeAudioSink_ = sink;
            activeAudioFrameConverter_ = converter;
            activeAudioTimeStretcher_ = stretcher;
            activeAudioFrameProcessor_ = processor;
            appliedAudioSinkSerial_ = serial;
        }
        if (sink) {
            sink->setEventCallback(
                [bridge = audioSinkCallbackBridge_,
                 weakSink = std::weak_ptr<AudioSink>(sink),
                 serial](const AudioSinkEvent& event) {
                    if (const auto source = weakSink.lock()) {
                        std::lock_guard<std::mutex> lock(bridge->mutex);
                        if (bridge->owner) {
                            bridge->owner->publishAudioSinkEvent(
                                source.get(),
                                serial,
                                event);
                        }
                    }
                });
        }
    }

    void closeAudioSink(bool flush)
    {
        invalidateAudioClock();
        std::shared_ptr<AudioSink> sink;
        std::shared_ptr<AudioFrameConverter> converter;
        std::shared_ptr<AudioTimeStretcher> stretcher;
        std::shared_ptr<AudioFrameProcessor> processor;
        bool wasOpen = false;
        bool converterWasOpen = false;
        bool stretcherWasOpen = false;
        bool processorWasOpen = false;
        {
            std::lock_guard<std::mutex> lock(mutex_);
            sink = activeAudioSink_;
            converter = activeAudioFrameConverter_;
            stretcher = activeAudioTimeStretcher_;
            processor = activeAudioFrameProcessor_;
            wasOpen = audioSinkOpen_;
            converterWasOpen = audioFrameConverterOpen_;
            stretcherWasOpen = audioTimeStretcherOpen_;
            processorWasOpen = audioFrameProcessorOpen_;
            audioSinkOpen_ = false;
            audioSinkOpenAttempted_ = false;
            audioSinkHasClock_ = false;
            audioFrameConverterOpen_ = false;
            audioTimeStretcherOpen_ = false;
            audioFrameProcessorOpen_ = false;
        }
        if ((!sink || !wasOpen) && (!converter || !converterWasOpen)
            && (!stretcher || !stretcherWasOpen)
            && (!processor || !processorWasOpen)) {
            return;
        }
        std::lock_guard<std::mutex> sinkLock(audioSinkCallMutex_);
        if (sink && wasOpen && flush) {
            sink->flush();
        }
        if (stretcher && stretcherWasOpen) {
            if (flush) {
                stretcher->reset();
            }
            stretcher->close();
        }
        if (processor && processorWasOpen) {
            if (flush) {
                processor->reset();
            }
            processor->close();
        }
        audioProcessorInputSamples_ = 0;
        audioProcessorOutputSamples_ = 0;
        audioProcessorLastTimestamp_ = 0;
        audioProcessorHasTimestamp_ = false;
        audioProcessorFormat_ = {};
        if (converter && converterWasOpen) {
            if (flush) {
                converter->reset();
            }
            converter->close();
        }
        if (sink && wasOpen) {
            sink->close();
        }
    }

    void flushAudioSink()
    {
        invalidateAudioClock();
        std::shared_ptr<AudioSink> sink;
        std::shared_ptr<AudioFrameConverter> converter;
        std::shared_ptr<AudioTimeStretcher> stretcher;
        std::shared_ptr<AudioFrameProcessor> processor;
        bool converterOpen = false;
        bool stretcherOpen = false;
        bool processorOpen = false;
        {
            std::lock_guard<std::mutex> lock(mutex_);
            if (!audioSinkOpen_) {
                return;
            }
            sink = activeAudioSink_;
            converter = activeAudioFrameConverter_;
            stretcher = activeAudioTimeStretcher_;
            processor = activeAudioFrameProcessor_;
            converterOpen = audioFrameConverterOpen_;
            stretcherOpen = audioTimeStretcherOpen_;
            processorOpen = audioFrameProcessorOpen_;
        }
        bool reset = true;
        if (sink) {
            std::lock_guard<std::mutex> sinkLock(audioSinkCallMutex_);
            sink->flush();
            if (stretcher && stretcherOpen) {
                reset = stretcher->reset();
            }
            if (processor && processorOpen) {
                reset = processor->reset() && reset;
                audioProcessorInputSamples_ = 0;
                audioProcessorOutputSamples_ = 0;
                audioProcessorLastTimestamp_ = 0;
                audioProcessorHasTimestamp_ = false;
            }
            if (converter && converterOpen) {
                reset = converter->reset() && reset;
            }
        }
        if (!reset) {
            closeAudioSink(false);
            publishEvent({
                "audio.output.reset",
                "The audio converter, time stretcher, or frame processor "
                "could not be reset",
                AVERROR_EXTERNAL,
            });
        }
    }

    void setAudioSinkPaused(bool paused)
    {
        if (paused) {
            invalidateAudioClock();
        }
        std::shared_ptr<AudioSink> sink;
        {
            std::lock_guard<std::mutex> lock(mutex_);
            if (!audioSinkOpen_) {
                return;
            }
            sink = activeAudioSink_;
        }
        if (sink) {
            std::lock_guard<std::mutex> sinkLock(audioSinkCallMutex_);
            sink->pause(paused);
        }
    }

    void deliverAudioToSink(
        const AudioFrame& frame,
        std::uint64_t generation)
    {
        std::shared_ptr<AudioSink> sink;
        std::shared_ptr<AudioFrameConverter> converter;
        std::shared_ptr<AudioTimeStretcher> stretcher;
        std::shared_ptr<AudioFrameProcessor> processor;
        std::uint64_t serial = 0;
        bool open = false;
        bool attempted = false;
        bool converterOpen = false;
        bool stretcherOpen = false;
        bool processorOpen = false;
        bool hasClock = false;
        double playbackRate = 1.0;
        {
            std::lock_guard<std::mutex> lock(mutex_);
            if (requestedState_ != State::Playing) {
                return;
            }
            sink = activeAudioSink_;
            converter = activeAudioFrameConverter_;
            stretcher = activeAudioTimeStretcher_;
            processor = activeAudioFrameProcessor_;
            serial = appliedAudioSinkSerial_;
            if (serial != audioSinkSerial_) {
                return;
            }
            open = audioSinkOpen_;
            attempted = audioSinkOpenAttempted_;
            converterOpen = audioFrameConverterOpen_;
            stretcherOpen = audioTimeStretcherOpen_;
            processorOpen = audioFrameProcessorOpen_;
            hasClock = audioSinkHasClock_;
            playbackRate = playbackRate_;
        }
        if (!sink) {
            return;
        }

        const auto decodedFormat = audioFormat(frame);
        if (!open && !attempted) {
            AudioSinkCapabilities capabilities;
            AudioSinkOpenResult result;
            AudioConverterOpenResult converterResult;
            AudioTimeStretchOpenResult stretcherResult;
            AudioProcessorOpenResult processorResult;
            bool conversionNeeded = false;
            bool converterOpened = false;
            const bool stretchNeeded =
                std::abs(playbackRate - 1.0) > 0.000001;
            bool stretcherOpened = false;
            bool processorOpened = false;
            {
                std::lock_guard<std::mutex> sinkLock(audioSinkCallMutex_);
                capabilities = sink->capabilities();
                result = sink->open(decodedFormat);
                conversionNeeded = result.success
                    && result.deviceFormat.isValid()
                    && !sameAudioFormat(
                        decodedFormat,
                        result.deviceFormat);
                if (conversionNeeded && converter) {
                    converterResult =
                        converter->open(decodedFormat, result.deviceFormat);
                    converterOpened = converterResult.success;
                }
                if (result.success && result.deviceFormat.isValid()
                    && (!conversionNeeded || converterOpened)
                    && stretchNeeded && stretcher) {
                    stretcherResult = stretcher->open(
                        result.deviceFormat,
                        playbackRate);
                    stretcherOpened = stretcherResult.success;
                }
                if (result.success && result.deviceFormat.isValid()
                    && (!conversionNeeded || converterOpened)
                    && (!stretchNeeded || stretcherOpened)
                    && processor) {
                    processorResult = processor->open(result.deviceFormat);
                    processorOpened = processorResult.success;
                    if (processorOpened) {
                        audioProcessorFormat_ = result.deviceFormat;
                        audioProcessorInputSamples_ = 0;
                        audioProcessorOutputSamples_ = 0;
                        audioProcessorLastTimestamp_ = 0;
                        audioProcessorHasTimestamp_ = false;
                    }
                }
            }

            bool formatSupported = result.success
                && result.deviceFormat.isValid()
                && (!conversionNeeded || converterOpened)
                && (!stretchNeeded || stretcherOpened)
                && (!processor || processorOpened);
            bool stillActive = false;
            {
                std::lock_guard<std::mutex> lock(mutex_);
                stillActive = activeAudioSink_ == sink
                    && activeAudioFrameConverter_ == converter
                    && activeAudioTimeStretcher_ == stretcher
                    && activeAudioFrameProcessor_ == processor
                    && appliedAudioSinkSerial_ == serial
                    && audioSinkSerial_ == serial
                    && requestedState_ == State::Playing
                    && generation
                        == presentationGeneration_.load(
                            std::memory_order_acquire);
                if (stillActive) {
                    audioSinkOpenAttempted_ = true;
                    audioSinkOpen_ = formatSupported;
                    audioSinkHasClock_ =
                        formatSupported && capabilities.hasDeviceClock;
                    audioFrameConverterOpen_ =
                        formatSupported && converterOpened;
                    audioTimeStretcherOpen_ =
                        formatSupported && stretcherOpened;
                    audioFrameProcessorOpen_ =
                        formatSupported && processorOpened;
                }
            }

            if (!stillActive || (result.success && !formatSupported)) {
                std::lock_guard<std::mutex> sinkLock(audioSinkCallMutex_);
                if (stretcher && stretcherOpened) {
                    stretcher->close();
                }
                if (processor && processorOpened) {
                    processor->close();
                }
                if (converter && converterOpened) {
                    converter->close();
                }
                if (result.success) {
                    sink->close();
                }
            }
            if (!stillActive) {
                return;
            }
            if (!result.success) {
                publishEvent({
                    "audio.sink.open",
                    result.error.empty()
                        ? "The audio sink could not be opened"
                        : result.error,
                    AVERROR_EXTERNAL,
                });
                return;
            }
            if (!formatSupported) {
                if (!result.deviceFormat.isValid()) {
                    publishEvent({
                        "audio.sink.format",
                        "The audio sink returned an invalid device format",
                        AVERROR(EINVAL),
                    });
                } else if (conversionNeeded && !converter) {
                    publishEvent({
                        "audio.sink.format",
                        "The audio sink requires PCM conversion, but no "
                        "audio converter is connected",
                        AVERROR(ENOSYS),
                    });
                } else if (conversionNeeded && !converterOpened) {
                    publishEvent({
                        "audio.converter.open",
                        converterResult.error.empty()
                            ? "The audio converter could not be opened"
                            : converterResult.error,
                        AVERROR_EXTERNAL,
                    });
                } else if (stretchNeeded && !stretcher) {
                    publishEvent({
                        "audio.time_stretch.unavailable",
                        "Playback-rate audio requires an injected time "
                        "stretcher; device output is disabled",
                        AVERROR(ENOSYS),
                    });
                } else if (stretchNeeded && !stretcherOpened) {
                    publishEvent({
                        "audio.time_stretch.open",
                        stretcherResult.error.empty()
                            ? "The audio time stretcher could not be opened"
                            : stretcherResult.error,
                        AVERROR_EXTERNAL,
                    });
                } else if (processor && !processorOpened) {
                    publishEvent({
                        "audio.processor.open",
                        processorResult.error.empty()
                            ? "The audio frame processor could not be opened"
                            : processorResult.error,
                        AVERROR_EXTERNAL,
                    });
                }
                return;
            }

            {
                std::lock_guard<std::mutex> sinkLock(audioSinkCallMutex_);
                sink->pause(false);
            }
            open = true;
            converterOpen = converterOpened;
            stretcherOpen = stretcherOpened;
            processorOpen = processorOpened;
            hasClock = capabilities.hasDeviceClock;
        }

        if (!open) {
            return;
        }

        AudioConversionResult conversion {
            true,
            audioBufferView(frame),
            {},
        };
        AudioTimeStretchResult stretched { true, {}, {} };
        AudioProcessingResult processed { true, {}, {} };
        std::string processingError;
        bool written = true;
        AudioSinkClock clockBefore;
        AudioSinkClock clockAfter;
        std::int64_t submittedUntil = 0;
        std::int64_t submittedStart = 0;
        bool submitted = false;
        {
            std::lock_guard<std::mutex> sinkLock(audioSinkCallMutex_);
            if (hasClock) {
                clockBefore = sink->clock();
            }
            if (converter && converterOpen) {
                conversion = converter->convert(frame);
            }
            if (conversion.success && conversion.buffer.isValid()) {
                if (stretcher && stretcherOpen) {
                    stretched = stretcher->process(conversion.buffer);
                } else {
                    stretched.buffers.push_back(conversion.buffer);
                }
            }
            if (conversion.success && stretched.success) {
                for (const auto& buffer : stretched.buffers) {
                    if (!buffer.isValid()) {
                        continue;
                    }
                    processed = { true, {}, {} };
                    if (processor && processorOpen) {
                        audioProcessorInputSamples_ +=
                            buffer.samplesPerChannel;
                        processed = processor->process(buffer);
                    } else {
                        processed.buffers.push_back(buffer);
                    }
                    if (!processed.success
                        || processed.buffers.size() > 32) {
                        processingError = processed.error;
                        if (processingError.empty()
                            && processed.buffers.size() > 32) {
                            processingError =
                                "The audio frame processor produced more "
                                "than 32 buffers for one input";
                        }
                        break;
                    }
                    for (const auto& output : processed.buffers) {
                        if (!output.isValid()
                            || !sameAudioFormat(
                                output.format,
                                buffer.format)
                            || output.duration < 0
                            || (audioProcessorHasTimestamp_
                                && output.timestamp
                                    < audioProcessorLastTimestamp_)) {
                            processed.success = false;
                            processingError =
                                "The audio frame processor violated its "
                                "format or timestamp contract";
                            break;
                        }
                        if (processor && processorOpen) {
                            audioProcessorOutputSamples_ +=
                                output.samplesPerChannel;
                            audioProcessorLastTimestamp_ = output.timestamp;
                            audioProcessorHasTimestamp_ = true;
                        }
                        if (!sink->write(output)) {
                            written = false;
                            break;
                        }
                        if (!submitted) {
                            submittedStart = output.timestamp;
                            submitted = true;
                        }
                        submittedUntil = std::max(
                            submittedUntil,
                            output.timestamp
                                + std::max<std::int64_t>(
                                    0,
                                    output.duration));
                    }
                    if (!processed.success || !written) {
                        break;
                    }
                }
            }
            if (hasClock) {
                clockAfter = sink->clock();
            }
        }
        if (hasClock) {
            cacheAudioClockSample(
                clockBefore,
                serial,
                0,
                generation,
                playbackRate,
                std::nullopt);
            cacheAudioClockSample(
                clockAfter,
                serial,
                written && submitted ? submittedUntil : 0,
                generation,
                playbackRate,
                written && submitted
                    ? std::optional<std::int64_t>(submittedStart)
                    : std::nullopt);
        }
        if (!conversion.success) {
            closeAudioSink(false);
            {
                std::lock_guard<std::mutex> lock(mutex_);
                if (activeAudioSink_ == sink
                    && appliedAudioSinkSerial_ == serial) {
                    audioSinkOpenAttempted_ = true;
                }
            }
            publishEvent({
                "audio.converter.convert",
                conversion.error.empty()
                    ? "The audio converter rejected a decoded frame"
                    : conversion.error,
                AVERROR_EXTERNAL,
            });
            return;
        }
        if (!stretched.success) {
            closeAudioSink(false);
            {
                std::lock_guard<std::mutex> lock(mutex_);
                if (activeAudioSink_ == sink
                    && appliedAudioSinkSerial_ == serial) {
                    audioSinkOpenAttempted_ = true;
                }
            }
            publishEvent({
                "audio.time_stretch.process",
                stretched.error.empty()
                    ? "The audio time stretcher rejected a PCM buffer"
                    : stretched.error,
                AVERROR_EXTERNAL,
            });
            return;
        }
        if (!processed.success) {
            closeAudioSink(false);
            {
                std::lock_guard<std::mutex> lock(mutex_);
                if (activeAudioSink_ == sink
                    && appliedAudioSinkSerial_ == serial) {
                    audioSinkOpenAttempted_ = true;
                }
            }
            publishEvent({
                "audio.processor.process",
                processingError.empty()
                    ? "The audio frame processor rejected a PCM buffer"
                    : processingError,
                AVERROR_EXTERNAL,
            });
            return;
        }
        if (!conversion.buffer.isValid() || !submitted) {
            return;
        }
        if (!written) {
            closeAudioSink(false);
            {
                std::lock_guard<std::mutex> lock(mutex_);
                if (activeAudioSink_ == sink
                    && appliedAudioSinkSerial_ == serial) {
                    audioSinkOpenAttempted_ = true;
                }
            }
            publishEvent({
                "audio.sink.write",
                "The audio sink rejected a decoded audio buffer",
                AVERROR_EXTERNAL,
            });
        }
    }

    bool drainAudioPipeline()
    {
        std::shared_ptr<AudioSink> sink;
        std::shared_ptr<AudioFrameConverter> converter;
        std::shared_ptr<AudioTimeStretcher> stretcher;
        std::shared_ptr<AudioFrameProcessor> processor;
        std::uint64_t serial = 0;
        bool hasClock = false;
        bool converterOpen = false;
        bool stretcherOpen = false;
        bool processorOpen = false;
        double playbackRate = 1.0;
        {
            std::lock_guard<std::mutex> lock(mutex_);
            if (!audioSinkOpen_) {
                return true;
            }
            sink = activeAudioSink_;
            converter = activeAudioFrameConverter_;
            stretcher = activeAudioTimeStretcher_;
            processor = activeAudioFrameProcessor_;
            serial = appliedAudioSinkSerial_;
            hasClock = audioSinkHasClock_;
            converterOpen = audioFrameConverterOpen_;
            stretcherOpen = audioTimeStretcherOpen_;
            processorOpen = audioFrameProcessorOpen_;
            playbackRate = playbackRate_;
        }
        if (!sink) {
            return true;
        }

        struct AudioSubmission {
            bool written = true;
            bool submitted = false;
            std::int64_t submittedStart = 0;
            std::int64_t submittedUntil = 0;
            AudioSinkClock clock;
        };
        auto submit = [this, &sink, hasClock, processorOpen](
                          const std::vector<AudioBufferView>& buffers) {
            AudioSubmission submission;
            for (const auto& buffer : buffers) {
                if (!buffer.isValid()
                    || (processorOpen
                        && (!sameAudioFormat(
                                buffer.format,
                                audioProcessorFormat_)
                            || buffer.duration < 0
                            || (audioProcessorHasTimestamp_
                                && buffer.timestamp
                                    < audioProcessorLastTimestamp_)))) {
                    submission.written = false;
                    break;
                }
                if (processorOpen) {
                    audioProcessorOutputSamples_ +=
                        buffer.samplesPerChannel;
                    audioProcessorLastTimestamp_ = buffer.timestamp;
                    audioProcessorHasTimestamp_ = true;
                }
                if (!sink->write(buffer)) {
                    submission.written = false;
                    break;
                }
                if (!submission.submitted) {
                    submission.submittedStart = buffer.timestamp;
                    submission.submitted = true;
                }
                submission.submittedUntil = std::max(
                    submission.submittedUntil,
                    buffer.timestamp
                        + std::max<std::int64_t>(0, buffer.duration));
            }
            if (hasClock) {
                submission.clock = sink->clock();
            }
            return submission;
        };
        struct ProcessedSubmission {
            bool success = true;
            std::string error;
            AudioSubmission submission;
        };
        auto processAndSubmit = [this,
                                 &processor,
                                 processorOpen,
                                 &submit](
                                    const std::vector<AudioBufferView>& inputs) {
            ProcessedSubmission result;
            for (const auto& input : inputs) {
                AudioProcessingResult processed { true, {}, {} };
                if (processor && processorOpen) {
                    if (!input.isValid()
                        || !sameAudioFormat(
                            input.format,
                            audioProcessorFormat_)) {
                        result.success = false;
                        result.error =
                            "The audio frame processor received an invalid "
                            "or renegotiated format";
                        break;
                    }
                    audioProcessorInputSamples_ += input.samplesPerChannel;
                    processed = processor->process(input);
                } else {
                    processed.buffers.push_back(input);
                }
                if (!processed.success || processed.buffers.size() > 32) {
                    result.success = false;
                    result.error = processed.error;
                    if (result.error.empty()
                        && processed.buffers.size() > 32) {
                        result.error =
                            "The audio frame processor produced more than 32 "
                            "buffers for one input";
                    }
                    break;
                }
                const auto submitted = submit(processed.buffers);
                result.submission.written =
                    result.submission.written && submitted.written;
                if (submitted.submitted) {
                    if (!result.submission.submitted) {
                        result.submission.submittedStart =
                            submitted.submittedStart;
                        result.submission.submitted = true;
                    }
                    result.submission.submittedUntil = std::max(
                        result.submission.submittedUntil,
                        submitted.submittedUntil);
                }
                result.submission.clock = submitted.clock;
                if (!submitted.written) {
                    break;
                }
            }
            return result;
        };
        auto cacheSubmission = [this,
                                hasClock,
                                serial,
                                playbackRate](
                                   const AudioSubmission& submission,
                                   std::uint64_t generation) {
            if (hasClock) {
                cacheAudioClockSample(
                    submission.clock,
                    serial,
                    submission.written && submission.submitted
                        ? submission.submittedUntil
                        : 0,
                    generation,
                    playbackRate,
                    submission.written && submission.submitted
                        ? std::optional<std::int64_t>(
                              submission.submittedStart)
                        : std::nullopt);
            }
        };

        if (converter && converterOpen) {
            bool converterFinished = false;
            for (int iteration = 0; iteration < 32; ++iteration) {
                AudioConversionResult converted;
                AudioTimeStretchResult stretched { true, {}, {} };
                ProcessedSubmission processed;
                const auto generation = presentationGeneration_.load(
                    std::memory_order_acquire);
                {
                    std::lock_guard<std::mutex> sinkLock(
                        audioSinkCallMutex_);
                    converted = converter->drain();
                    if (converted.success
                        && converted.buffer.isValid()) {
                        if (stretcher && stretcherOpen) {
                            stretched = stretcher->process(
                                converted.buffer);
                        } else {
                            stretched.buffers.push_back(converted.buffer);
                        }
                    }
                    if (converted.success && stretched.success) {
                        processed = processAndSubmit(stretched.buffers);
                    }
                }
                cacheSubmission(processed.submission, generation);
                if (!converted.success) {
                    publishEvent({
                        "audio.converter.drain",
                        converted.error.empty()
                            ? "The audio converter could not be drained"
                            : converted.error,
                        AVERROR_EXTERNAL,
                    });
                    return false;
                }
                if (!stretched.success) {
                    publishEvent({
                        "audio.time_stretch.process",
                        stretched.error.empty()
                            ? "The audio time stretcher rejected drained PCM"
                            : stretched.error,
                        AVERROR_EXTERNAL,
                    });
                    return false;
                }
                if (!processed.success) {
                    publishEvent({
                        "audio.processor.process",
                        processed.error.empty()
                            ? "The audio frame processor rejected drained PCM"
                            : processed.error,
                        AVERROR_EXTERNAL,
                    });
                    return false;
                }
                if (!processed.submission.written) {
                    publishEvent({
                        "audio.sink.write",
                        "The audio sink rejected a drained audio buffer",
                        AVERROR_EXTERNAL,
                    });
                    return false;
                }
                if (!converted.buffer.isValid()) {
                    converterFinished = true;
                    break;
                }
            }
            if (!converterFinished) {
                publishEvent({
                    "audio.converter.drain",
                    "The audio converter did not finish draining",
                    AVERROR_EXTERNAL,
                });
                return false;
            }
        }

        if (stretcher && stretcherOpen) {
            bool stretcherFinished = false;
            for (int iteration = 0; iteration < 32; ++iteration) {
                AudioTimeStretchResult result;
                ProcessedSubmission processed;
                const auto generation = presentationGeneration_.load(
                    std::memory_order_acquire);
                {
                    std::lock_guard<std::mutex> sinkLock(
                        audioSinkCallMutex_);
                    result = stretcher->drain();
                    if (result.success) {
                        processed = processAndSubmit(result.buffers);
                    }
                }
                cacheSubmission(processed.submission, generation);
                if (!result.success) {
                    publishEvent({
                        "audio.time_stretch.drain",
                        result.error.empty()
                            ? "The audio time stretcher could not be drained"
                            : result.error,
                        AVERROR_EXTERNAL,
                    });
                    return false;
                }
                if (!processed.success) {
                    publishEvent({
                        "audio.processor.process",
                        processed.error.empty()
                            ? "The audio frame processor rejected stretched "
                              "drain output"
                            : processed.error,
                        AVERROR_EXTERNAL,
                    });
                    return false;
                }
                if (!processed.submission.written) {
                    publishEvent({
                        "audio.sink.write",
                        "The audio sink rejected stretched drain output",
                        AVERROR_EXTERNAL,
                    });
                    return false;
                }
                if (result.buffers.empty()) {
                    stretcherFinished = true;
                    break;
                }
            }
            if (!stretcherFinished) {
                publishEvent({
                    "audio.time_stretch.drain",
                    "The audio time stretcher did not finish draining",
                    AVERROR_EXTERNAL,
                });
                return false;
            }
        }

        if (processor && processorOpen) {
            bool processorFinished = false;
            for (int iteration = 0; iteration < 32; ++iteration) {
                AudioProcessingResult result;
                AudioSubmission submission;
                const auto generation = presentationGeneration_.load(
                    std::memory_order_acquire);
                {
                    std::lock_guard<std::mutex> sinkLock(
                        audioSinkCallMutex_);
                    result = processor->drain();
                    if (result.success && result.buffers.size() <= 32) {
                        submission = submit(result.buffers);
                    }
                }
                cacheSubmission(submission, generation);
                if (!result.success || result.buffers.size() > 32) {
                    publishEvent({
                        "audio.processor.drain",
                        result.error.empty()
                            ? "The audio frame processor could not be drained"
                            : result.error,
                        AVERROR_EXTERNAL,
                    });
                    return false;
                }
                if (!submission.written) {
                    publishEvent({
                        "audio.sink.write",
                        "The audio sink rejected processor drain output",
                        AVERROR_EXTERNAL,
                    });
                    return false;
                }
                if (result.buffers.empty()) {
                    processorFinished = true;
                    break;
                }
            }
            if (!processorFinished) {
                publishEvent({
                    "audio.processor.drain",
                    "The audio frame processor did not finish draining",
                    AVERROR_EXTERNAL,
                });
                return false;
            }
            if (audioProcessorInputSamples_
                != audioProcessorOutputSamples_) {
                publishEvent({
                    "audio.processor.samples",
                    "The audio frame processor changed the completed "
                    "segment sample count",
                    AVERROR_INVALIDDATA,
                });
                return false;
            }
        }
        return true;
    }

    bool drainAudioSink()
    {
        std::shared_ptr<AudioSink> sink;
        {
            std::lock_guard<std::mutex> lock(mutex_);
            if (!audioSinkOpen_) {
                return true;
            }
            sink = activeAudioSink_;
        }
        if (!sink) {
            return true;
        }

        bool drained = false;
        {
            std::lock_guard<std::mutex> sinkLock(audioSinkCallMutex_);
            drained = sink->drain();
        }
        completeCachedAudioClock();
        if (!drained) {
            publishEvent({
                "audio.sink.drain",
                "The audio sink could not drain its queued buffers",
                AVERROR_EXTERNAL,
            });
        }
        return drained;
    }

    std::optional<std::int64_t> audioClockPosition() const
    {
        CachedAudioClock cached;
        {
            std::lock_guard<std::mutex> lock(audioClockMutex_);
            cached = cachedAudioClock_;
        }
        if (!cached.valid) {
            return std::nullopt;
        }

        {
            std::lock_guard<std::mutex> lock(mutex_);
            if (!audioSinkOpen_ || !audioSinkHasClock_
                || currentState_ != State::Playing
                || waitingForOutput_
                || cached.sinkSerial != appliedAudioSinkSerial_) {
                return std::nullopt;
            }
        }
        const auto elapsed = std::max<std::int64_t>(
            0,
            std::chrono::duration_cast<Milliseconds>(
                Clock::now() - cached.sampledAt)
                .count());
        auto position = cached.position
            + static_cast<std::int64_t>(std::llround(
                static_cast<double>(elapsed) * cached.playbackRate));
        if (cached.submittedUntil > 0) {
            position = std::min(position, cached.submittedUntil);
        }
        return std::max<std::int64_t>(0, position);
    }

    void publishAudioSinkEvent(
        const AudioSink* source,
        std::uint64_t serial,
        const AudioSinkEvent& event)
    {
        {
            std::lock_guard<std::mutex> lock(mutex_);
            if (activeAudioSink_.get() != source
                || appliedAudioSinkSerial_ != serial) {
                return;
            }
        }

        std::string code;
        switch (event.type) {
        case AudioSinkEventType::Underrun:
            code = "audio.sink.underrun";
            break;
        case AudioSinkEventType::DeviceLost:
            code = "audio.sink.device_lost";
            break;
        case AudioSinkEventType::Error:
            code = "audio.sink.error";
            break;
        }
        if (event.type == AudioSinkEventType::Underrun) {
            const auto generation =
                presentationGeneration_.load(std::memory_order_acquire);
            beginOutputWait(generation, position());
        }
        invalidateAudioClock();
        publishEvent({
            std::move(code),
            event.detail,
            event.type == AudioSinkEventType::Underrun ? 0 : AVERROR_EXTERNAL,
        });
    }

    void handlePlaybackEnd()
    {
        const auto generation =
            presentationGeneration_.load(std::memory_order_acquire);
        const auto controlPending = [this] {
            return seekRequest_ || seekCompletion_ || prepareRequest_
                || !trackSwitchRequests_.empty()
                || loadedSerial_ != mediaSerial_
                || requestedState_ != State::Playing;
        };
        {
            std::lock_guard<std::mutex> lock(mutex_);
            if (controlPending()) {
                return;
            }
        }
        if (!waitForAudioQueueDrained(generation)) {
            return;
        }
        if (!drainAudioPipeline() || !drainAudioSink()
            || !drainVideoFrameProcessor()) {
            stopPlayback(false, true);
            return;
        }
        if (!waitForPresentationDrained(generation)
            || generation
                != presentationGeneration_.load(std::memory_order_acquire)) {
            return;
        }
        {
            std::lock_guard<std::mutex> lock(mutex_);
            if (controlPending()) {
                return;
            }
        }

        int loopCount;
        int loopsCompleted;
        std::int64_t loopStart;
        {
            std::lock_guard<std::mutex> lock(mutex_);
            loopCount = loopCount_;
            loopsCompleted = loopsCompleted_;
            loopStart = rangeStart_;
        }
        if (loopCount < 0 || loopsCompleted < loopCount) {
            {
                std::lock_guard<std::mutex> lock(mutex_);
                ++loopsCompleted_;
            }
            const int error = seekMedia(loopStart, SeekFlag::KeyFrame);
            if (error >= 0) {
                return;
            }
            publishEvent({
                "loop.error",
                "Could not seek to the loop start: " + ffmpegError(error),
                error,
            });
        }
        stopPlayback(true);
    }

    void stopPlayback(bool naturalEnd, bool invalid = false)
    {
        resetPlaybackQueues();
        {
            std::lock_guard<std::mutex> lock(mutex_);
            if (naturalEnd
                && (seekRequest_ || seekCompletion_ || prepareRequest_
                    || !trackSwitchRequests_.empty()
                    || loadedSerial_ != mediaSerial_
                    || requestedState_ != State::Playing)) {
                return;
            }
            // Commit the natural stop while holding the same mutex used by
            // public control requests. A seek that arrived while queues were
            // draining wins above; one arriving after this point observes the
            // closed state and is rejected instead of losing its callback.
            media_.reset();
            hasOpenMedia_ = false;
            loadedSerial_ = 0;
            requestedState_ = State::Stopped;
            currentPosition_ =
                naturalEnd && mediaInfo_.duration > 0 ? mediaInfo_.duration : 0;
            resetClockLocked(currentPosition_);
        }
        closeAudioSink(false);
        closeVideoFrameProcessor();
        publishState(State::Stopped);
        publishStatus(invalid
                ? MediaStatus::Invalid
                : naturalEnd ? MediaStatus::EndOfMedia
                             : MediaStatus::NoMedia);
    }

    void failOpen(int error, std::string detail)
    {
        publishEvent({
            "media.error",
            std::move(detail) + ": " + ffmpegError(error),
            error,
        });
        {
            std::lock_guard<std::mutex> lock(mutex_);
            hasOpenMedia_ = false;
            loadedSerial_ = 0;
            requestedState_ = State::Stopped;
        }
        publishState(State::Stopped);
        publishStatus(MediaStatus::Invalid);

        std::optional<PrepareRequest> prepare;
        {
            std::lock_guard<std::mutex> lock(mutex_);
            prepare = std::move(prepareRequest_);
            prepareRequest_.reset();
        }
        if (prepare && prepare->callback) {
            bool boost = false;
            prepare->callback(-1, &boost);
        }
    }

    bool wasCanceled(std::uint64_t serial) const
    {
        std::lock_guard<std::mutex> lock(mutex_);
        return quitting_.load(std::memory_order_acquire)
            || serial != mediaSerial_ || requestedState_ == State::Stopped;
    }

    void publishState(State value)
    {
        StateCallback callback;
        {
            std::lock_guard<std::mutex> lock(mutex_);
            if (currentState_ == value) {
                stateChanged_.notify_all();
                return;
            }
            currentState_ = value;
            callback = stateCallback_;
            stateChanged_.notify_all();
        }
        if (callback) {
            callback(value);
        }
    }

    void publishStatus(MediaStatus value)
    {
        StatusCallback callback;
        MediaStatus oldValue;
        {
            std::lock_guard<std::mutex> lock(mutex_);
            if (status_ == value) {
                return;
            }
            oldValue = status_;
            status_ = value;
            callback = statusCallback_;
        }
        if (callback) {
            callback(oldValue, value);
        }
    }

    void publishEvent(MediaEvent event)
    {
        EventCallback callback;
        {
            std::lock_guard<std::mutex> lock(mutex_);
            callback = eventCallback_;
        }
        if (callback) {
            callback(event);
        }
    }

    void publishNetworkRecoveryStatus(NetworkRecoveryStatus status)
    {
        NetworkRecoveryStatusCallback callback;
        {
            std::lock_guard<std::mutex> lock(mutex_);
            networkRecoveryStatus_ = status;
            callback = networkRecoveryStatusCallback_;
        }
        if (callback) {
            callback(status);
        }
    }

    std::int64_t clockPositionLocked() const
    {
        if (currentState_ != State::Playing || waitingForOutput_) {
            return currentPosition_;
        }
        const auto elapsed = std::chrono::duration_cast<Milliseconds>(
                                 Clock::now() - clockWallBase_)
                                 .count();
        const auto value = clockMediaBase_
            + static_cast<std::int64_t>(
                               static_cast<double>(elapsed) * playbackRate_);
        if (mediaInfo_.duration > 0) {
            return std::clamp<std::int64_t>(value, 0, mediaInfo_.duration);
        }
        return std::max<std::int64_t>(0, value);
    }

    std::int64_t clampPosition(std::int64_t position) const
    {
        std::lock_guard<std::mutex> lock(mutex_);
        return clampPositionLocked(position);
    }

    std::int64_t clampPositionLocked(std::int64_t position) const
    {
        if (mediaInfo_.duration > 0) {
            return std::clamp<std::int64_t>(
                position,
                0,
                mediaInfo_.duration);
        }
        return std::max<std::int64_t>(0, position);
    }

    void resetClockLocked(std::int64_t position)
    {
        clockMediaBase_ = std::max<std::int64_t>(0, position);
        clockWallBase_ = Clock::now();
    }

    std::int64_t currentPosition() const
    {
        std::lock_guard<std::mutex> lock(mutex_);
        return currentPosition_;
    }

    mutable std::mutex mutex_;
    mutable std::mutex renderBindingsMutex_;
    mutable std::mutex videoFrameSnapshotMutex_;
    mutable std::mutex audioSinkCallMutex_;
    mutable std::mutex videoProcessorCallMutex_;
    mutable std::mutex audioClockMutex_;
    mutable std::mutex audioQueueMutex_;
    mutable std::mutex audioPacketMutex_;
    mutable std::mutex packetBufferMutex_;
    mutable std::mutex presentationMutex_;
    mutable std::mutex videoPacketMutex_;
    std::shared_ptr<AudioSinkCallbackBridge> audioSinkCallbackBridge_ =
        std::make_shared<AudioSinkCallbackBridge>();
    std::shared_ptr<PacketDiskStore> packetDiskStore_ =
        std::make_shared<PacketDiskStore>();
    std::condition_variable controlChanged_;
    std::condition_variable stateChanged_;
    std::condition_variable audioQueueChanged_;
    std::condition_variable audioQueueSpace_;
    std::condition_variable audioQueueDrained_;
    std::condition_variable audioPacketChanged_;
    std::condition_variable audioPacketSpace_;
    std::condition_variable audioPacketDrained_;
    std::condition_variable packetBufferChanged_;
    std::condition_variable presentationChanged_;
    std::condition_variable presentationDrained_;
    std::condition_variable videoPacketChanged_;
    std::condition_variable videoPacketSpace_;
    std::condition_variable videoPacketDrained_;
    std::thread worker_;
    std::thread audioDecodeWorker_;
    std::thread audioWorker_;
    std::thread presentationWorker_;
    std::thread videoDecodeWorker_;
    std::atomic<bool> quitting_ { false };
    std::atomic<std::uint64_t> interruptEpoch_ { 1 };
    std::atomic<std::uint64_t> presentationGeneration_ { 1 };
    std::atomic<std::uint64_t> decodedVideoFrames_ { 0 };
    std::atomic<std::uint64_t> videoQueueOverflowDrops_ { 0 };
    std::atomic<std::uint64_t> lateVideoDrops_ { 0 };
    std::atomic<std::uint64_t> deliveredVideoFrames_ { 0 };
    std::atomic<std::uint64_t> maximumQueuedVideoFrames_ { 0 };
    std::atomic<std::uint64_t> videoPresentationStarvations_ { 0 };
    std::atomic<std::uint64_t>
        maximumVideoPresentationStarvationMilliseconds_ { 0 };
    std::atomic<std::uint64_t> lowLatencyVideoQueueDrops_ { 0 };
    std::atomic<std::uint64_t> networkRecoveryAttempts_ { 0 };
    std::atomic<std::uint64_t> successfulNetworkRecoveries_ { 0 };
    std::atomic<std::uint64_t> failedNetworkRecoveries_ { 0 };
    std::atomic<bool> outputClockPollRequested_ { false };
    std::atomic<bool> audioPacketInputEnded_ { false };
    std::atomic<bool> videoPacketInputEnded_ { false };
    std::atomic<std::uint64_t> queuedMemoryPacketBytes_ { 0 };
    std::atomic<bool> diskCacheUnavailable_ { false };
    InterruptContext interrupt_;

    std::deque<QueuedAudioFrame> audioQueue_;
    std::int64_t queuedAudioDuration_ = 0;
    bool audioFrameInFlight_ = false;
    std::deque<QueuedAudioPacket> audioPackets_;
    bool audioPacketInFlight_ = false;
    bool audioDecodeFailed_ = false;
    std::uint64_t audioDecodeFailureGeneration_ = 0;
    std::deque<PresentationItem> presentationQueue_;
    std::size_t queuedVideoFrames_ = 0;
    std::size_t queuedSubtitleFrames_ = 0;
    bool presentationInFlight_ = false;
    std::deque<QueuedVideoPacket> videoPackets_;
    bool videoPacketInFlight_ = false;
    bool videoDecodeFailed_ = false;
    std::uint64_t videoDecodeFailureGeneration_ = 0;
    PacketBufferState packetBufferState_;
    std::optional<PacketBufferStatus> lastPublishedPacketBufferStatus_;
    CachedAudioClock cachedAudioClock_;
    MediaContext media_;
    ReadRecoveryBudget primaryReadRecoveryBudget_;
    ReadRecoveryBudget externalAudioReadRecoveryBudget_;
    ReadRecoveryBudget externalSubtitleReadRecoveryBudget_;
    std::int64_t primaryDemuxProgress_ =
        std::numeric_limits<std::int64_t>::min();
    std::int64_t externalAudioDemuxProgress_ =
        std::numeric_limits<std::int64_t>::min();
    std::int64_t externalSubtitleDemuxProgress_ =
        std::numeric_limits<std::int64_t>::min();
    bool hasOpenMedia_ = false;
    std::atomic<bool> reachedRangeEnd_ { false };
    std::string url_;
    std::string externalAudioUrl_;
    std::string externalSubtitleUrl_;
    std::uint64_t mediaSerial_ = 1;
    std::uint64_t loadedSerial_ = 0;
    std::uint64_t requestSerial_ = 0;
    std::optional<PrepareRequest> prepareRequest_;
    std::optional<SeekRequest> seekRequest_;
    std::optional<AccurateSeekState> accurateSeek_;
    std::optional<SeekCompletion> seekCompletion_;
    std::uint64_t accuratePresentationFloorGeneration_ = 0;
    std::int64_t accuratePresentationFloor_ = 0;
    std::deque<TrackSwitchRequest> trackSwitchRequests_;

    State requestedState_ = State::Stopped;
    State currentState_ = State::Stopped;
    MediaStatus status_ = MediaStatus::NoMedia;
    MediaInfo mediaInfo_;
    std::int64_t currentPosition_ = 0;
    std::int64_t lastPresentedVideoTimestamp_ = -1;
    std::int64_t previousPresentedVideoTimestamp_ = -1;
    bool waitingForOutput_ = false;
    bool outputWaitPrimed_ = false;
    std::uint64_t outputWaitGeneration_ = 0;
    bool outputWaitRequiresDeviceClock_ = false;
    bool videoPrerollPending_ = false;
    std::uint64_t videoPrerollGeneration_ = 0;
    std::size_t videoPrerollFrameCount_ = 0;
    Clock::time_point videoPrerollDeadline_ = Clock::now();
    std::int64_t clockMediaBase_ = 0;
    Clock::time_point clockWallBase_ = Clock::now();
    float playbackRate_ = 1.0F;
    int loopCount_ = 0;
    int loopsCompleted_ = 0;
    std::int64_t rangeStart_ = 0;
    std::int64_t rangeEnd_ = MediaEnd;
    HardwareDecodeConfig hardwareDecodeConfig_;
    LivePlaybackPolicy livePlaybackPolicy_;
    NetworkRecoveryPolicy networkRecoveryPolicy_;
    NetworkRecoveryStatus networkRecoveryStatus_;
    PacketBufferPolicy packetBufferPolicy_;

    std::unordered_map<std::string, std::string> properties_;
    StateCallback stateCallback_;
    StatusCallback statusCallback_;
    PacketBufferStatusCallback packetBufferStatusCallback_;
    NetworkRecoveryStatusCallback networkRecoveryStatusCallback_;
    EventCallback eventCallback_;
    VideoFrameCallback videoFrameCallback_;
    AudioFrameCallback audioFrameCallback_;
    SubtitleFrameCallback subtitleFrameCallback_;
    VideoFrameScheduler videoFrameScheduler_;
    std::shared_ptr<AudioSink> audioSink_;
    std::shared_ptr<AudioSink> activeAudioSink_;
    std::shared_ptr<AudioFrameConverter> audioFrameConverter_;
    std::shared_ptr<AudioFrameConverter> activeAudioFrameConverter_;
    std::shared_ptr<AudioTimeStretcher> audioTimeStretcher_;
    std::shared_ptr<AudioTimeStretcher> activeAudioTimeStretcher_;
    std::shared_ptr<AudioFrameProcessor> audioFrameProcessor_;
    std::shared_ptr<AudioFrameProcessor> activeAudioFrameProcessor_;
    std::uint64_t audioSinkSerial_ = 1;
    std::uint64_t appliedAudioSinkSerial_ = 0;
    bool audioSinkOpen_ = false;
    bool audioSinkOpenAttempted_ = false;
    bool audioSinkHasClock_ = false;
    bool audioFrameConverterOpen_ = false;
    bool audioTimeStretcherOpen_ = false;
    bool audioFrameProcessorOpen_ = false;
    AudioFormat audioProcessorFormat_;
    std::int64_t audioProcessorInputSamples_ = 0;
    std::int64_t audioProcessorOutputSamples_ = 0;
    std::int64_t audioProcessorLastTimestamp_ = 0;
    bool audioProcessorHasTimestamp_ = false;
    std::shared_ptr<VideoFrameProcessor> videoFrameProcessor_;
    std::shared_ptr<VideoFrameProcessor> activeVideoFrameProcessor_;
    std::uint64_t videoFrameProcessorSerial_ = 1;
    std::uint64_t activeVideoFrameProcessorSerial_ = 0;
    VideoProcessorFormat activeVideoProcessorFormat_;
    bool videoFrameProcessorOpen_ = false;
    bool videoFrameProcessorBypass_ = false;
    std::shared_ptr<const RenderBindingsSnapshot> renderBindings_ =
        std::make_shared<const RenderBindingsSnapshot>();
    std::shared_ptr<const VideoFrameSnapshot> currentVideoFrameSnapshot_;
    std::atomic<std::uint64_t> nextVideoFrameSequence_ { 1 };
};

Player::Player()
    : impl_(std::make_unique<Impl>())
{
}

Player::~Player() = default;
Player::Player(Player&&) noexcept = default;
Player& Player::operator=(Player&&) noexcept = default;

void Player::setMedia(std::string url)
{
    impl_->setMedia(std::move(url));
}

std::string Player::url() const
{
    return impl_->url();
}

bool Player::setExternalMedia(MediaType type, std::string url)
{
    return impl_->setExternalMedia(type, std::move(url));
}

std::string Player::externalMedia(MediaType type) const
{
    return impl_->externalMedia(type);
}

void Player::prepare(
    std::int64_t startPosition,
    PrepareCallback callback,
    SeekFlag flags)
{
    impl_->prepare(startPosition, std::move(callback), flags);
}

bool Player::seek(
    std::int64_t position,
    SeekFlag flags,
    SeekCallback callback)
{
    return impl_->seek(position, flags, std::move(callback));
}

bool Player::stepForward(SeekCallback callback)
{
    return impl_->stepForward(std::move(callback));
}

bool Player::stepBackward(SeekCallback callback)
{
    return impl_->stepBackward(std::move(callback));
}

void Player::setState(State state)
{
    impl_->setState(state);
}

State Player::state() const
{
    return impl_->state();
}

bool Player::waitFor(State state, long timeoutMs)
{
    return impl_->waitFor(state, timeoutMs);
}

MediaStatus Player::mediaStatus() const
{
    return impl_->mediaStatus();
}

MediaInfo Player::mediaInfo() const
{
    return impl_->mediaInfo();
}

bool Player::setActiveTrack(MediaType type, int track)
{
    return impl_->setActiveTrack(type, track);
}

std::int64_t Player::position() const
{
    return impl_->position();
}

PlaybackStatistics Player::playbackStatistics() const noexcept
{
    return impl_->playbackStatistics();
}

Player& Player::setLivePlaybackPolicy(LivePlaybackPolicy policy)
{
    impl_->setLivePlaybackPolicy(policy);
    return *this;
}

LivePlaybackPolicy Player::livePlaybackPolicy() const
{
    return impl_->livePlaybackPolicy();
}

Player& Player::setNetworkRecoveryPolicy(NetworkRecoveryPolicy policy)
{
    impl_->setNetworkRecoveryPolicy(policy);
    return *this;
}

NetworkRecoveryPolicy Player::networkRecoveryPolicy() const
{
    return impl_->networkRecoveryPolicy();
}

NetworkRecoveryStatus Player::networkRecoveryStatus() const
{
    return impl_->networkRecoveryStatus();
}

Player& Player::setPacketBufferPolicy(PacketBufferPolicy policy)
{
    impl_->setPacketBufferPolicy(policy);
    return *this;
}

PacketBufferPolicy Player::packetBufferPolicy() const
{
    return impl_->packetBufferPolicy();
}

PacketBufferStatus Player::packetBufferStatus() const
{
    return impl_->packetBufferStatus();
}

std::string Player::packetDiskCachePath() const
{
    return impl_->packetDiskCachePath();
}

bool Player::clearPacketDiskCache()
{
    return impl_->clearPacketDiskCache();
}

Player& Player::onStateChanged(StateCallback callback)
{
    impl_->onStateChanged(std::move(callback));
    return *this;
}

Player& Player::onMediaStatus(StatusCallback callback)
{
    impl_->onMediaStatus(std::move(callback));
    return *this;
}

Player& Player::onPacketBufferStatus(PacketBufferStatusCallback callback)
{
    impl_->onPacketBufferStatus(std::move(callback));
    return *this;
}

Player& Player::onNetworkRecoveryStatus(
    NetworkRecoveryStatusCallback callback)
{
    impl_->onNetworkRecoveryStatus(std::move(callback));
    return *this;
}

Player& Player::onEvent(EventCallback callback)
{
    impl_->onEvent(std::move(callback));
    return *this;
}

Player& Player::onVideoFrame(VideoFrameCallback callback)
{
    impl_->onVideoFrame(std::move(callback));
    return *this;
}

Player& Player::onAudioFrame(AudioFrameCallback callback)
{
    impl_->onAudioFrame(std::move(callback));
    return *this;
}

Player& Player::onSubtitleFrame(SubtitleFrameCallback callback)
{
    impl_->onSubtitleFrame(std::move(callback));
    return *this;
}

Player& Player::setAudioSink(std::shared_ptr<AudioSink> sink)
{
    impl_->setAudioSink(std::move(sink));
    return *this;
}

Player& Player::setAudioFrameConverter(
    std::shared_ptr<AudioFrameConverter> converter)
{
    impl_->setAudioFrameConverter(std::move(converter));
    return *this;
}

Player& Player::setAudioTimeStretcher(
    std::shared_ptr<AudioTimeStretcher> stretcher)
{
    impl_->setAudioTimeStretcher(std::move(stretcher));
    return *this;
}

Player& Player::setAudioFrameProcessor(
    std::shared_ptr<AudioFrameProcessor> processor)
{
    impl_->setAudioFrameProcessor(std::move(processor));
    return *this;
}

Player& Player::setVideoFrameProcessor(
    std::shared_ptr<VideoFrameProcessor> processor)
{
    impl_->setVideoFrameProcessor(std::move(processor));
    return *this;
}

Player& Player::setHardwareDecodeConfig(HardwareDecodeConfig config)
{
    impl_->setHardwareDecodeConfig(config);
    return *this;
}

HardwareDecodeConfig Player::hardwareDecodeConfig() const
{
    return impl_->hardwareDecodeConfig();
}

Player& Player::setVideoFrameScheduler(VideoFrameScheduler scheduler)
{
    impl_->setVideoFrameScheduler(std::move(scheduler));
    return *this;
}

Player& Player::setRenderCallback(RenderCallback callback)
{
    impl_->setRenderCallback(std::move(callback));
    return *this;
}

Player& Player::setVideoRenderer(VideoRenderer renderer)
{
    impl_->setVideoRenderer(std::move(renderer));
    return *this;
}

Player& Player::setVideoRenderAPI(
    std::shared_ptr<VideoRenderAPI> renderer,
    void* opaque)
{
    impl_->setVideoRenderAPI(std::move(renderer), opaque);
    return *this;
}

VideoRenderResult Player::renderVideoDetailed(void* opaque)
{
    return impl_->renderVideoDetailed(opaque);
}

double Player::renderVideo(void* opaque)
{
    return impl_->renderVideo(opaque);
}

void Player::setPlaybackRate(float value)
{
    impl_->setPlaybackRate(value);
}

float Player::playbackRate() const
{
    return impl_->playbackRate();
}

void Player::setLoop(int count)
{
    impl_->setLoop(count);
}

void Player::setRange(std::int64_t start, std::int64_t end)
{
    impl_->setRange(start, end);
}

void Player::setProperty(std::string key, std::string value)
{
    impl_->setProperty(std::move(key), std::move(value));
}

std::string Player::property(
    const std::string& key,
    std::string defaultValue) const
{
    return impl_->property(key, std::move(defaultValue));
}

} // namespace qtav
