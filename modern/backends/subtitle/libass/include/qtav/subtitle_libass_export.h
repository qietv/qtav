// SPDX-License-Identifier: LGPL-2.1-or-later
#pragma once

#if defined(_WIN32)
#  if defined(QTAV_SUBTITLE_LIBASS_STATIC)
#    define QTAV_SUBTITLE_LIBASS_EXPORT
#  elif defined(QTAV_SUBTITLE_LIBASS_BUILDING_LIBRARY)
#    define QTAV_SUBTITLE_LIBASS_EXPORT __declspec(dllexport)
#  else
#    define QTAV_SUBTITLE_LIBASS_EXPORT __declspec(dllimport)
#  endif
#else
#  define QTAV_SUBTITLE_LIBASS_EXPORT \
      __attribute__((visibility("default")))
#endif
