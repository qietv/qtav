// SPDX-License-Identifier: LGPL-2.1-or-later
#pragma once

#if defined(QTAV_INTEROP_CVMETAL_STATIC)
#  define QTAV_INTEROP_CVMETAL_EXPORT
#else
#  define QTAV_INTEROP_CVMETAL_EXPORT \
    __attribute__((visibility("default")))
#endif
