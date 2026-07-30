// SPDX-License-Identifier: LGPL-2.1-or-later
#pragma once

#if defined(QTAV_PLATFORM_WINDOWS_STATIC)
#  define QTAV_PLATFORM_WINDOWS_EXPORT
#elif defined(QTAV_PLATFORM_WINDOWS_BUILDING_LIBRARY)
#  define QTAV_PLATFORM_WINDOWS_EXPORT __declspec(dllexport)
#else
#  define QTAV_PLATFORM_WINDOWS_EXPORT __declspec(dllimport)
#endif
