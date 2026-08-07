// SPDX-License-Identifier: LGPL-2.1-or-later
#pragma once

#if !defined(_WIN32)
#  error "qtav/d3d11_statistics.h is available only on Windows"
#endif

namespace qtav {

// Controls optional D3D11 playback diagnostics. Retry behavior and resource
// lifetime never depend on this setting.
enum class D3D11StatisticsMode {
    Off,
    Counters,
    Timing,
};

} // namespace qtav
