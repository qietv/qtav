// SPDX-License-Identifier: LGPL-2.1-or-later
#pragma once

#if defined(_WIN32)
#  if defined(QTAV_AUDIO_TIMESTRETCH_STATIC)
#    define QTAV_AUDIO_TIMESTRETCH_EXPORT
#  elif defined(QTAV_AUDIO_TIMESTRETCH_BUILDING_LIBRARY)
#    define QTAV_AUDIO_TIMESTRETCH_EXPORT __declspec(dllexport)
#  else
#    define QTAV_AUDIO_TIMESTRETCH_EXPORT __declspec(dllimport)
#  endif
#else
#  define QTAV_AUDIO_TIMESTRETCH_EXPORT \
      __attribute__((visibility("default")))
#endif
