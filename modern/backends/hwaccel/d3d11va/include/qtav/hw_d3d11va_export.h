// SPDX-License-Identifier: LGPL-2.1-or-later
#pragma once

#if defined(QTAV_HW_D3D11VA_STATIC)
#  define QTAV_HW_D3D11VA_EXPORT
#elif defined(QTAV_HW_D3D11VA_BUILDING_LIBRARY)
#  define QTAV_HW_D3D11VA_EXPORT __declspec(dllexport)
#else
#  define QTAV_HW_D3D11VA_EXPORT __declspec(dllimport)
#endif
