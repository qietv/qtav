// SPDX-License-Identifier: LGPL-2.1-or-later
#pragma once

#if !defined(_WIN32)
#  error "qtav/d3d11va_hardware_decoder.h is available only on Windows"
#endif

#if !defined(NOMINMAX)
#  define NOMINMAX
#endif

#include <d3d11.h>

#include <memory>

#include <qtav/d3d11_device_access.h>
#include <qtav/hardware_decoder.h>
#include <qtav/hw_d3d11va_export.h>

namespace qtav {

struct QTAV_HW_D3D11VA_EXPORT D3D11VAHardwareDecodeOptions {
    // If selected-device setup, codec capability, or decoder initialization
    // fails, continue through the ordinary FFmpeg software decoder.
    bool allowSoftwareFallback = true;
    // Extra fixed-pool surfaces reserved for QtAVCore's current render frame
    // and a bounded number of application-retained frames.
    int extraHardwareFrames = 4;
};

// Creates an FFmpeg D3D11VA device on the application-selected device and
// installs lock callbacks backed by deviceAccess->contextGuard().
// A failed device creation returns a requested D3D11 config with a required
// but empty device token, allowing Player's explicit fallback policy to run.
QTAV_HW_D3D11VA_EXPORT HardwareDecodeConfig
d3d11vaHardwareDecodeConfig(
    std::shared_ptr<D3D11DeviceAccess> deviceAccess,
    D3D11VAHardwareDecodeOptions options = {}) noexcept;

class D3D11VAFrame;

// Validates and retains a D3D11 hardware frame. Invalid device type, software
// format, texture, array slice, dimensions, or texture format returns empty.
QTAV_HW_D3D11VA_EXPORT D3D11VAFrame
d3d11vaFrame(const HardwareFrame& frame) noexcept;

// Strong retained view of one FFmpeg D3D11VA decoder texture-array slice.
// Native pointers are borrowed and remain valid while this object or a copy
// of sourceFrame() is alive.
class QTAV_HW_D3D11VA_EXPORT D3D11VAFrame final {
public:
    D3D11VAFrame() noexcept;

    explicit operator bool() const noexcept;
    bool isValid() const noexcept;
    ID3D11Texture2D* texture() const noexcept;
    UINT arraySlice() const noexcept;
    ID3D11Device* device() const noexcept;
    int width() const noexcept;
    int height() const noexcept;
    PixelFormat softwareFormat() const noexcept;
    const HardwareFrame& sourceFrame() const noexcept;

private:
    D3D11VAFrame(
        HardwareFrame source,
        ID3D11Texture2D* texture,
        UINT arraySlice,
        ID3D11Device* device) noexcept;

    HardwareFrame source_;
    ID3D11Texture2D* texture_ = nullptr;
    UINT arraySlice_ = 0;
    ID3D11Device* device_ = nullptr;

    friend QTAV_HW_D3D11VA_EXPORT D3D11VAFrame d3d11vaFrame(
        const HardwareFrame& frame) noexcept;
};

} // namespace qtav
