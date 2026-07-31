// SPDX-License-Identifier: LGPL-2.1-or-later
#pragma once

#if defined(QTAV_HW_MEDIACODEC_STATIC)
#  define QTAV_HW_MEDIACODEC_EXPORT
#else
#  define QTAV_HW_MEDIACODEC_EXPORT \
    __attribute__((visibility("default")))
#endif
