// SPDX-License-Identifier: LGPL-2.1-or-later
#pragma once

#include <qtav/export.h>
#include <qtav/hardware_frame.h>

namespace qtav {

// Selects an optional FFmpeg hardware-device decode path without exposing
// FFmpeg or platform SDK types through the core API. Backend-specific targets
// provide convenient constructors and strong native-frame accessors.
struct QTAV_CORE_EXPORT HardwareDecodeConfig {
    HardwareDeviceType deviceType = HardwareDeviceType::Unknown;
    bool allowSoftwareFallback = true;

    bool isValid() const noexcept
    {
        return deviceType != HardwareDeviceType::Unknown;
    }
};

} // namespace qtav
