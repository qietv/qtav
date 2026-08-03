// SPDX-License-Identifier: LGPL-2.1-or-later
#pragma once

#include <d3d11.h>
#include <dxgi.h>

#include <qtav/video_render_api.h>

namespace qtav::detail {

inline bool d3d11ShouldCopyDecoderSurface(
    bool intelDevice,
    bool hasDolbyVisionMetadata,
    bool rawYuv) noexcept
{
    return intelDevice && hasDolbyVisionMetadata && rawYuv;
}

inline VideoRenderEventType d3d11FailureEvent(HRESULT result) noexcept
{
    switch (result) {
    case DXGI_ERROR_DEVICE_HUNG:
    case DXGI_ERROR_DEVICE_REMOVED:
    case DXGI_ERROR_DEVICE_RESET:
    case DXGI_ERROR_DRIVER_INTERNAL_ERROR:
        return VideoRenderEventType::SurfaceLost;
    default:
        return VideoRenderEventType::Error;
    }
}

} // namespace qtav::detail
