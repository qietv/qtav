// SPDX-License-Identifier: LGPL-2.1-or-later
#pragma once

#include <cstdint>
#include <memory>

#include <qtav/export.h>
#include <qtav/hardware_frame.h>

namespace qtav {

namespace detail {
class HardwareDecodeDevicePrivate;
}

// A reference-counted, backend-created hardware device that can be supplied
// to the decoder without exposing FFmpeg or platform SDK types through core.
// Copies identify the same device token and keep its native resources alive.
class QTAV_CORE_EXPORT HardwareDecodeDevice {
public:
    HardwareDecodeDevice() noexcept;
    HardwareDecodeDevice(const HardwareDecodeDevice&) noexcept;
    HardwareDecodeDevice(HardwareDecodeDevice&&) noexcept;
    HardwareDecodeDevice& operator=(
        const HardwareDecodeDevice&) noexcept;
    HardwareDecodeDevice& operator=(HardwareDecodeDevice&&) noexcept;
    ~HardwareDecodeDevice();

    explicit operator bool() const noexcept;
    bool isValid() const noexcept;
    HardwareDeviceType deviceType() const noexcept;
    std::uintptr_t nativeIdentity() const noexcept;

    bool operator==(const HardwareDecodeDevice& other) const noexcept;
    bool operator!=(const HardwareDecodeDevice& other) const noexcept;

private:
    class Impl;
    explicit HardwareDecodeDevice(std::shared_ptr<const Impl> impl) noexcept;

    std::shared_ptr<const Impl> impl_;

    friend class detail::HardwareDecodeDevicePrivate;
};

// Selects an optional FFmpeg hardware-device decode path without exposing
// FFmpeg or platform SDK types through the core API. Backend-specific targets
// provide convenient constructors and strong native-frame accessors.
struct QTAV_CORE_EXPORT HardwareDecodeConfig {
    HardwareDeviceType deviceType = HardwareDeviceType::Unknown;
    bool allowSoftwareFallback = true;
    HardwareDecodeDevice device;
    // Additional decoder-owned hardware surfaces. Values are clamped to the
    // core-supported range before AVCodecContext is opened.
    int extraHardwareFrames = 0;
    // Backend helpers which promise an application-selected device set this
    // so a failed token creation cannot silently select another native device.
    bool requireSuppliedDevice = false;

    bool isValid() const noexcept
    {
        return deviceType != HardwareDeviceType::Unknown;
    }
};

} // namespace qtav
