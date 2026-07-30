// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

#include "DebugWindow.g.h"

namespace winrt::QtAVWinUI3::implementation {

struct DebugWindow : DebugWindowT<DebugWindow> {
    DebugWindow();

    void SetLog(hstring const& text);
    void AppendLine(hstring const& line);
};

} // namespace winrt::QtAVWinUI3::implementation

namespace winrt::QtAVWinUI3::factory_implementation {

struct DebugWindow
    : DebugWindowT<DebugWindow, implementation::DebugWindow> {
};

} // namespace winrt::QtAVWinUI3::factory_implementation
