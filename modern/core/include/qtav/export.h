// SPDX-License-Identifier: LGPL-2.1-or-later
#pragma once

#if defined(_WIN32)
#  if defined(QTAV_CORE_STATIC)
#    define QTAV_CORE_EXPORT
#  elif defined(QTAV_CORE_BUILDING_LIBRARY)
#    define QTAV_CORE_EXPORT __declspec(dllexport)
#  else
#    define QTAV_CORE_EXPORT __declspec(dllimport)
#  endif
#else
#  define QTAV_CORE_EXPORT __attribute__((visibility("default")))
#endif
