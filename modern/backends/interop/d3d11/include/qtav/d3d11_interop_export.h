// SPDX-License-Identifier: LGPL-2.1-or-later
#pragma once

#if defined(QTAV_INTEROP_D3D11_STATIC)
#  define QTAV_INTEROP_D3D11_EXPORT
#elif defined(QTAV_INTEROP_D3D11_BUILDING_LIBRARY)
#  define QTAV_INTEROP_D3D11_EXPORT __declspec(dllexport)
#else
#  define QTAV_INTEROP_D3D11_EXPORT __declspec(dllimport)
#endif
