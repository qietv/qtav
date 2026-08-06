// SPDX-License-Identifier: LGPL-2.1-or-later
#pragma once

#if defined(QTAV_INTEROP_OHCODEC_OPENGL_STATIC)
#  define QTAV_INTEROP_OHCODEC_OPENGL_EXPORT
#elif defined(QTAV_INTEROP_OHCODEC_OPENGL_BUILDING_LIBRARY)
#  define QTAV_INTEROP_OHCODEC_OPENGL_EXPORT \
       __attribute__((visibility("default")))
#else
#  define QTAV_INTEROP_OHCODEC_OPENGL_EXPORT \
       __attribute__((visibility("default")))
#endif
