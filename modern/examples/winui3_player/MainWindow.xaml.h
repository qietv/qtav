// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

#include "MainWindow.g.h"

#include <memory>

namespace winrt::QtAVWinUI3::implementation {

struct MainWindowPrivate;

struct MainWindow : MainWindowT<MainWindow> {
    MainWindow();
    ~MainWindow();

    fire_and_forget OpenFile_Click(
        IInspectable const&,
        Microsoft::UI::Xaml::RoutedEventArgs const&);
    void OpenUrl_Click(
        IInspectable const&,
        Microsoft::UI::Xaml::RoutedEventArgs const&);
    void PlayPause_Click(
        IInspectable const&,
        Microsoft::UI::Xaml::RoutedEventArgs const&);
    void Stop_Click(
        IInspectable const&,
        Microsoft::UI::Xaml::RoutedEventArgs const&);
    void UrlTextBox_KeyDown(
        IInspectable const&,
        Microsoft::UI::Xaml::Input::KeyRoutedEventArgs const&);
    void ProgressSlider_ValueChanged(
        IInspectable const&,
        Microsoft::UI::Xaml::Controls::Primitives::
            RangeBaseValueChangedEventArgs const&);
    void DebugToggle_Checked(
        IInspectable const&,
        Microsoft::UI::Xaml::RoutedEventArgs const&);
    void DebugToggle_Unchecked(
        IInspectable const&,
        Microsoft::UI::Xaml::RoutedEventArgs const&);
    void VideoPanel_Loaded(
        IInspectable const&,
        Microsoft::UI::Xaml::RoutedEventArgs const&);
    void Window_Closed(
        IInspectable const&,
        Microsoft::UI::Xaml::WindowEventArgs const&);

private:
    std::unique_ptr<MainWindowPrivate> impl_;
};

} // namespace winrt::QtAVWinUI3::implementation

namespace winrt::QtAVWinUI3::factory_implementation {

struct MainWindow
    : MainWindowT<MainWindow, implementation::MainWindow> {
};

} // namespace winrt::QtAVWinUI3::factory_implementation
