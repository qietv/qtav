// SPDX-License-Identifier: GPL-3.0-or-later
#include "pch.h"

#include "DebugWindow.xaml.h"

#if __has_include("DebugWindow.g.cpp")
#  include "DebugWindow.g.cpp"
#endif

#include <microsoft.ui.xaml.window.h>

namespace winrt::QtAVWinUI3::implementation {

DebugWindow::DebugWindow()
{
    InitializeComponent();
    Title(L"QtAVCore Debug");

    HWND windowHandle = nullptr;
    Microsoft::UI::Xaml::Window window = *this;
    check_hresult(
        window.as<IWindowNative>()->get_WindowHandle(&windowHandle));

    const UINT dpi = GetDpiForWindow(windowHandle);
    const float scale = static_cast<float>(dpi) / 96.0F;
    SetWindowPos(
        windowHandle,
        nullptr,
        0,
        0,
        static_cast<int>(720.0F * scale),
        static_cast<int>(520.0F * scale),
        SWP_NOMOVE | SWP_NOZORDER);
}

void DebugWindow::SetLog(hstring const& text)
{
    LogTextBox().Text(text);
    LogTextBox().Select(static_cast<int>(text.size()), 0);
}

void DebugWindow::AppendLine(hstring const& line)
{
    std::wstring text = LogTextBox().Text().c_str();
    if (!text.empty()) {
        text.append(L"\r\n");
    }
    text.append(line.c_str());
    LogTextBox().Text(text);
    LogTextBox().Select(static_cast<int>(text.size()), 0);
}

} // namespace winrt::QtAVWinUI3::implementation
