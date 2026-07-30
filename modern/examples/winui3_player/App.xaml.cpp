// SPDX-License-Identifier: GPL-3.0-or-later
#include "pch.h"

#include "App.xaml.h"
#include "MainWindow.xaml.h"

namespace winrt::QtAVWinUI3::implementation {

App::App()
{
    InitializeComponent();

#if defined(_DEBUG) \
    && !defined(DISABLE_XAML_GENERATED_BREAK_ON_UNHANDLED_EXCEPTION)
    UnhandledException(
        [](
            IInspectable const&,
            Microsoft::UI::Xaml::UnhandledExceptionEventArgs const&
                event) {
            if (IsDebuggerPresent()) {
                const auto message = event.Message();
                (void)message;
                __debugbreak();
            }
        });
#endif
}

void App::OnLaunched(
    Microsoft::UI::Xaml::LaunchActivatedEventArgs const&)
{
    try {
        window_ = winrt::make<MainWindow>();
        window_.Activate();
    } catch (hresult_error const& error) {
        const auto message = error.message();
        MessageBoxW(
            nullptr,
            message.c_str(),
            L"QtAVWinUI3 startup failure",
            MB_OK | MB_ICONERROR);
    }
}

} // namespace winrt::QtAVWinUI3::implementation
