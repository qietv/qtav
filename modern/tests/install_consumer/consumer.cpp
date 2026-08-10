// SPDX-License-Identifier: LGPL-2.1-or-later

#include <qtav/qtav.h>

static_assert(QTAV_CORE_VERSION_MAJOR == 2);
static_assert(QTAV_CORE_VERSION_MINOR == 0);
static_assert(QTAV_CORE_VERSION_PATCH == 0);
static_assert(qtav::coreVersion == qtav::Version { 2, 0, 0 });
static_assert(qtav::coreVersionString == "2.0.0");

int main()
{
    qtav::Player player;
    return player.state() == qtav::State::Stopped ? 0 : 1;
}
