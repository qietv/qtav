// SPDX-License-Identifier: LGPL-2.1-or-later
#pragma once

#if defined(QTAV_RENDER_METAL_STATIC)
#  define QTAV_RENDER_METAL_EXPORT
#else
#  define QTAV_RENDER_METAL_EXPORT __attribute__((visibility("default")))
#endif
