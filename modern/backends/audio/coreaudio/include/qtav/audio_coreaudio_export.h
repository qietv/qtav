// SPDX-License-Identifier: LGPL-2.1-or-later
#pragma once

#if defined(QTAV_AUDIO_COREAUDIO_STATIC)
#  define QTAV_AUDIO_COREAUDIO_EXPORT
#else
#  define QTAV_AUDIO_COREAUDIO_EXPORT \
    __attribute__((visibility("default")))
#endif
