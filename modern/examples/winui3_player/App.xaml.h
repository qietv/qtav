// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

#include "App.xaml.g.h"

namespace winrt::QtAVWinUI3::implementation {

struct App : AppT<App> {
    App();

    void OnLaunched(
        Microsoft::UI::Xaml::LaunchActivatedEventArgs const&);

private:
    Microsoft::UI::Xaml::Window window_ { nullptr };
};

} // namespace winrt::QtAVWinUI3::implementation
