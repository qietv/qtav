// SPDX-License-Identifier: LGPL-2.1-or-later
#pragma once

#include <qtav/d3d11_device_access.h>

namespace qtav::detail {

// Build-tree-only bridge for native APIs, such as FFmpeg, which require
// separate C lock/unlock callbacks instead of an RAII guard.
class QTAV_PLATFORM_WINDOWS_EXPORT D3D11DeviceAccessPrivate {
public:
    static void lock(D3D11DeviceAccess& access) noexcept;
    static void unlock(D3D11DeviceAccess& access) noexcept;
};

} // namespace qtav::detail
