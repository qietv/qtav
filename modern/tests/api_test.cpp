// SPDX-License-Identifier: LGPL-2.1-or-later

#include <qtav/qtav.h>

#include <cassert>
#include <utility>

int main()
{
    qtav::Player player;
    assert(player.state() == qtav::State::Stopped);
    assert(player.mediaStatus() == qtav::MediaStatus::NoMedia);
    assert(player.position() == 0);

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
