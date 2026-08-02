// SPDX-License-Identifier: GPL-3.0-or-later
// Historical QtAVCore console-player integration. This file is archived and
// is not part of any build target.

#include <qtav/coreaudio_audio_sink.h>
#include <qtav/player.h>
#include <qtav/swresample_audio_converter.h>

#include <memory>

void configureArchivedCoreAudio(qtav::Player& player)
{
    player
        .setAudioFrameConverter(
            std::make_shared<qtav::SwresampleAudioConverter>())
        .setAudioSink(
            std::make_shared<qtav::CoreAudioAudioSink>());
}
