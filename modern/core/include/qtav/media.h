// SPDX-License-Identifier: LGPL-2.1-or-later
#pragma once

#include <cstdint>
#include <limits>
#include <string>
#include <vector>

#include <qtav/export.h>

namespace qtav {

enum class MediaType {
    Unknown = -1,
    Audio,
    Video,
    Subtitle,
};

enum class State {
    Stopped,
    Playing,
    Paused,
};

using PlaybackState = State;

enum class MediaStatus {
    NoMedia,
    Loading,
    Loaded,
    Buffering,
    EndOfMedia,
    Invalid,
};

enum class SeekFlag : std::uint32_t {
    FromStart = 0,
    FromNow = 1U << 0U,
    KeyFrame = 1U << 1U,
    AnyFrame = 1U << 2U,
};

constexpr SeekFlag operator|(SeekFlag lhs, SeekFlag rhs) noexcept
{
    return static_cast<SeekFlag>(
        static_cast<std::uint32_t>(lhs) | static_cast<std::uint32_t>(rhs));
}

constexpr SeekFlag operator&(SeekFlag lhs, SeekFlag rhs) noexcept
{
    return static_cast<SeekFlag>(
        static_cast<std::uint32_t>(lhs) & static_cast<std::uint32_t>(rhs));
}

constexpr bool hasFlag(SeekFlag value, SeekFlag flag) noexcept
{
    return (value & flag) != SeekFlag::FromStart;
}

struct QTAV_CORE_EXPORT TrackInfo {
    // Stable selector within the current MediaInfo snapshot. Main-input
    // tracks retain their FFmpeg stream index; external tracks are assigned
    // non-overlapping values after the main input's stream range.
    int index = -1;
    // Stream index within sourceUrl. Use index, not streamIndex, with
    // Player::setActiveTrack().
    int streamIndex = -1;
    MediaType type = MediaType::Unknown;
    std::string sourceUrl;
    bool external = false;
    std::string codec;
    std::string codecDescription;
    std::string language;
    std::string title;
    std::int64_t bitRate = 0;
    int width = 0;
    int height = 0;
    int sampleRate = 0;
    int channels = 0;
};

struct QTAV_CORE_EXPORT MediaInfo {
    std::string url;
    std::int64_t startTime = 0;
    std::int64_t duration = 0;
    bool seekable = false;
    std::vector<TrackInfo> tracks;
    int activeVideoTrack = -1;
    int activeAudioTrack = -1;
    int activeSubtitleTrack = -1;
};

struct QTAV_CORE_EXPORT MediaEvent {
    std::string category;
    std::string detail;
    int error = 0;
};

inline constexpr std::int64_t MediaEnd =
    std::numeric_limits<std::int64_t>::max();

} // namespace qtav
