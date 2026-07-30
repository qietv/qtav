// SPDX-License-Identifier: LGPL-2.1-or-later
#pragma once

#include <cstdint>

#include <qtav/hardware_decoder.h>

extern "C" {
struct AVBufferRef;
}

namespace qtav::detail {

// Private bridge for in-tree hardware backends. This header is deliberately
// not installed so AVBufferRef never enters the public QtAVCore API.
class QTAV_CORE_EXPORT HardwareDecodeDevicePrivate {
public:
    // Stores an av_buffer_ref() of context. The caller retains ownership of
    // the reference it passes in.
    static HardwareDecodeDevice create(
        HardwareDeviceType type,
        std::uintptr_t nativeIdentity,
        AVBufferRef* context) noexcept;

    // Returns a new referenced FFmpeg device context owned by the caller.
    static AVBufferRef* contextRef(
        const HardwareDecodeDevice& device) noexcept;
};

} // namespace qtav::detail
