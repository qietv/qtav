// SPDX-License-Identifier: LGPL-2.1-or-later
#pragma once

#if defined(QTAV_RENDER_VULKAN_ANDROID_STATIC)
#  define QTAV_RENDER_VULKAN_ANDROID_EXPORT
#else
#  define QTAV_RENDER_VULKAN_ANDROID_EXPORT \
    __attribute__((visibility("default")))
#endif
