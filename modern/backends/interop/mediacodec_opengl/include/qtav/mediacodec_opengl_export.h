// SPDX-License-Identifier: LGPL-2.1-or-later
#pragma once

#if defined(QTAV_INTEROP_MEDIACODEC_OPENGL_STATIC)
#  define QTAV_INTEROP_MEDIACODEC_OPENGL_EXPORT
#else
#  define QTAV_INTEROP_MEDIACODEC_OPENGL_EXPORT \
    __attribute__((visibility("default")))
#endif
