// SPDX-License-Identifier: LGPL-2.1-or-later

#include <qtav/videotoolbox_hardware_decoder.h>

namespace qtav {

HardwareDecodeConfig videoToolboxHardwareDecodeConfig(
    VideoToolboxHardwareDecodeConfig config) noexcept
{
    return {
        HardwareDeviceType::VideoToolbox,
        config.allowSoftwareFallback,
    };
}

CVPixelBufferRef videoToolboxPixelBuffer(
    const HardwareFrame& frame) noexcept
{
    if (frame.deviceType() != HardwareDeviceType::VideoToolbox) {
        return nullptr;
    }
    const auto handle = frame.nativeHandle(HardwareHandleType::Frame);
    return reinterpret_cast<CVPixelBufferRef>(handle.value);
}

} // namespace qtav
