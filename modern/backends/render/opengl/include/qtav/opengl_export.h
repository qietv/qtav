// SPDX-License-Identifier: LGPL-2.1-or-later
#pragma once

#if defined(_WIN32)
#  if defined(QTAV_RENDER_OPENGL_STATIC)
#    define QTAV_RENDER_OPENGL_EXPORT
#  elif defined(QTAV_RENDER_OPENGL_BUILDING_LIBRARY)
#    define QTAV_RENDER_OPENGL_EXPORT __declspec(dllexport)
#  else
#    define QTAV_RENDER_OPENGL_EXPORT __declspec(dllimport)
#  endif
#else
#  define QTAV_RENDER_OPENGL_EXPORT __attribute__((visibility("default")))
#endif
