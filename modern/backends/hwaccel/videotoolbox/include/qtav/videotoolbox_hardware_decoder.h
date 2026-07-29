// SPDX-License-Identifier: LGPL-2.1-or-later
#pragma once

#if !defined(__APPLE__)
#  error "qtav/videotoolbox_hardware_decoder.h is available only on Apple platforms"
#endif

#include <CoreVideo/CVPixelBuffer.h>

#include <qtav/hardware_decoder.h>
#include <qtav/hw_videotoolbox_export.h>

namespace qtav {

struct QTAV_HW_VIDEOTOOLBOX_EXPORT VideoToolboxHardwareDecodeConfig {
    // If VideoToolbox device creation or pixel-format negotiation fails,
    // reopen or continue with the ordinary FFmpeg software decoder.
    bool allowSoftwareFallback = true;
};

QTAV_HW_VIDEOTOOLBOX_EXPORT HardwareDecodeConfig
videoToolboxHardwareDecodeConfig(
    VideoToolboxHardwareDecodeConfig config = {}) noexcept;

// Returns the decoded CVPixelBuffer without retaining it. The result remains
// valid while a copy of frame is alive. Call CVPixelBufferRetain() when native
// code needs a lifetime independent of the HardwareFrame.
QTAV_HW_VIDEOTOOLBOX_EXPORT CVPixelBufferRef
videoToolboxPixelBuffer(const HardwareFrame& frame) noexcept;

} // namespace qtav
