// SPDX-License-Identifier: LGPL-2.1-or-later

#if defined(NDEBUG)
#  undef NDEBUG
#endif

#include <qtav/qtav.h>

#include <cassert>
#include <utility>

int main()
{
    qtav::Player player;
    assert(player.state() == qtav::State::Stopped);
    assert(player.mediaStatus() == qtav::MediaStatus::NoMedia);
    assert(player.position() == 0);
    const auto defaultBufferPolicy = player.packetBufferPolicy();
    assert(defaultBufferPolicy.enabled);
    assert(defaultBufferPolicy.initialBufferMilliseconds == 500);
    assert(defaultBufferPolicy.rebufferMilliseconds == 750);
    assert(defaultBufferPolicy.maximumBufferMilliseconds == 5'000);
    assert(
        defaultBufferPolicy.maximumBufferBytes
        == 32U * 1024U * 1024U);
    assert(!defaultBufferPolicy.diskCache.enabled);
    assert(
        defaultBufferPolicy.diskCache.maximumCacheMilliseconds
        == 60'000);
    assert(
        defaultBufferPolicy.diskCache.maximumCacheBytes
        == 256U * 1024U * 1024U);
    assert(player.packetBufferStatus().progress == 1.0);
    assert(player.packetDiskCachePath().empty());
    assert(player.clearPacketDiskCache());
    qtav::PacketBufferPolicy normalizedBufferPolicy;
    normalizedBufferPolicy.initialBufferMilliseconds = -1;
    normalizedBufferPolicy.rebufferMilliseconds = 2'000;
    normalizedBufferPolicy.maximumBufferMilliseconds = 100;
    normalizedBufferPolicy.maximumBufferBytes = 0;
    normalizedBufferPolicy.underflowDetectionMilliseconds = -1;
    player.setPacketBufferPolicy(normalizedBufferPolicy);
    normalizedBufferPolicy = player.packetBufferPolicy();
    assert(normalizedBufferPolicy.initialBufferMilliseconds == 0);
    assert(normalizedBufferPolicy.rebufferMilliseconds == 2'000);
    assert(normalizedBufferPolicy.maximumBufferMilliseconds == 2'000);
    assert(normalizedBufferPolicy.maximumBufferBytes > 0);
    assert(normalizedBufferPolicy.underflowDetectionMilliseconds == 0);
    normalizedBufferPolicy.diskCache.enabled = true;
    normalizedBufferPolicy.maximumBufferMilliseconds = 100;
    normalizedBufferPolicy.initialBufferMilliseconds = 3'000;
    normalizedBufferPolicy.diskCache.maximumCacheMilliseconds = -1;
    normalizedBufferPolicy.diskCache.maximumCacheBytes = 0;
    player.setPacketBufferPolicy(normalizedBufferPolicy);
    normalizedBufferPolicy = player.packetBufferPolicy();
    assert(normalizedBufferPolicy.maximumBufferMilliseconds == 100);
    assert(
        normalizedBufferPolicy.diskCache.maximumCacheMilliseconds
        == 2'900);
    assert(normalizedBufferPolicy.diskCache.maximumCacheBytes > 0);
    assert(!player.setExternalMedia(qtav::MediaType::Video, "video.mp4"));
    assert(player.setExternalMedia(qtav::MediaType::Audio, "audio.opus"));
    assert(player.setExternalMedia(qtav::MediaType::Subtitle, "subtitle.srt"));
    assert(player.externalMedia(qtav::MediaType::Audio) == "audio.opus");
    assert(player.externalMedia(qtav::MediaType::Subtitle) == "subtitle.srt");

    qtav::VideoColorSpace color;
    assert(!color.isSpecified());
    assert(!color.isHdr());
    color.transfer = qtav::ColorTransfer::PQ;
    assert(color.isSpecified());
    assert(color.isHdr());

    player.setProperty("custom.value", "42");
    assert(player.property("custom.value") == "42");
    assert(player.property("missing", "fallback") == "fallback");

    player.setPlaybackRate(1.5F);
    assert(player.playbackRate() == 1.5F);
    player.setRange(100, 200);
    player.setLoop(2);

    qtav::Player moved(std::move(player));
    assert(moved.state() == qtav::State::Stopped);
    return 0;
}
