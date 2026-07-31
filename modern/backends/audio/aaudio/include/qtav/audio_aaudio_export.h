// SPDX-License-Identifier: LGPL-2.1-or-later
#pragma once

#if defined(QTAV_AUDIO_AAUDIO_STATIC)
#  define QTAV_AUDIO_AAUDIO_EXPORT
#else
#  define QTAV_AUDIO_AAUDIO_EXPORT \
    __attribute__((visibility("default")))
#endif
