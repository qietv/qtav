// SPDX-License-Identifier: LGPL-2.1-or-later
#pragma once

#if defined(_WIN32) || defined(__CYGWIN__)
#  if defined(QTAV_AUDIO_FILTER_STATIC)
#    define QTAV_AUDIO_FILTER_EXPORT
#  elif defined(QTAV_AUDIO_FILTER_BUILDING_LIBRARY)
#    define QTAV_AUDIO_FILTER_EXPORT __declspec(dllexport)
#  else
#    define QTAV_AUDIO_FILTER_EXPORT __declspec(dllimport)
#  endif
#elif defined(__GNUC__) && __GNUC__ >= 4
#  define QTAV_AUDIO_FILTER_EXPORT __attribute__((visibility("default")))
#else
#  define QTAV_AUDIO_FILTER_EXPORT
#endif
