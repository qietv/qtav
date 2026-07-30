// SPDX-License-Identifier: LGPL-2.1-or-later
#pragma once

#if defined(QTAV_RENDER_VULKAN_STATIC)
#  define QTAV_RENDER_VULKAN_EXPORT
#elif defined(_WIN32)
#  if defined(QTAV_RENDER_VULKAN_BUILDING_LIBRARY)
#    define QTAV_RENDER_VULKAN_EXPORT __declspec(dllexport)
#  else
#    define QTAV_RENDER_VULKAN_EXPORT __declspec(dllimport)
#  endif
#else
#  define QTAV_RENDER_VULKAN_EXPORT __attribute__((visibility("default")))
#endif
