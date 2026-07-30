// SPDX-License-Identifier: LGPL-2.1-or-later
#pragma once

#if defined(_WIN32)
#  if defined(QTAV_OUTPUT_D3D11_STATIC)
#    define QTAV_OUTPUT_D3D11_EXPORT
#  elif defined(QTAV_OUTPUT_D3D11_BUILDING_LIBRARY)
#    define QTAV_OUTPUT_D3D11_EXPORT __declspec(dllexport)
#  else
#    define QTAV_OUTPUT_D3D11_EXPORT __declspec(dllimport)
#  endif
#else
#  define QTAV_OUTPUT_D3D11_EXPORT
#endif
