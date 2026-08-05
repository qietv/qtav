// SPDX-License-Identifier: GPL-3.0-or-later
#include "pch.h"

#include "MainWindow.xaml.h"

#if __has_include("MainWindow.g.cpp")
#  include "MainWindow.g.cpp"
#endif

#include "DebugWindow.xaml.h"

#include <microsoft.ui.xaml.media.dxinterop.h>
#include <microsoft.ui.xaml.window.h>

#include <shobjidl_core.h>

#include <qtav/d3d11_video_output.h>
#include <qtav/player.h>
#include <qtav/swresample_audio_converter.h>
#include <qtav/wasapi_audio_sink.h>

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cmath>
#include <cstdint>
#include <deque>
#include <functional>
#include <iomanip>
#include <memory>
#include <sstream>
#include <string>
#include <utility>

namespace winrt::QtAVWinUI3::implementation {

namespace {

const wchar_t* stateName(qtav::State state)
{
    switch (state) {
    case qtav::State::Stopped:
        return L"stopped";
    case qtav::State::Playing:
        return L"playing";
    case qtav::State::Paused:
        return L"paused";
    }
    return L"unknown";
}

const wchar_t* statusName(qtav::MediaStatus status)
{
    switch (status) {
    case qtav::MediaStatus::NoMedia:
        return L"no-media";
    case qtav::MediaStatus::Loading:
        return L"loading";
    case qtav::MediaStatus::Loaded:
        return L"loaded";
    case qtav::MediaStatus::Buffering:
        return L"buffering";
    case qtav::MediaStatus::EndOfMedia:
        return L"end-of-media";
    case qtav::MediaStatus::Invalid:
        return L"invalid";
    }
    return L"unknown";
}

const wchar_t* renderEventName(qtav::D3D11VideoOutputEventType type)
{
    switch (type) {
    case qtav::D3D11VideoOutputEventType::SurfaceLost:
        return L"surface-lost";
    case qtav::D3D11VideoOutputEventType::Error:
        return L"error";
    }
    return L"unknown";
}

std::wstring formatTime(std::int64_t milliseconds)
{
    milliseconds = std::max<std::int64_t>(milliseconds, 0);
    const auto totalSeconds = milliseconds / 1000;
    const auto seconds = totalSeconds % 60;
    const auto totalMinutes = totalSeconds / 60;
    const auto minutes = totalMinutes % 60;
    const auto hours = totalMinutes / 60;

    std::wostringstream output;
    output << std::setfill(L'0');
    if (hours > 0) {
        output << hours << L':' << std::setw(2) << minutes;
    } else {
        output << std::setw(2) << minutes;
    }
    output << L':' << std::setw(2) << seconds;
    return output.str();
}

std::wstring trim(std::wstring value)
{
    const auto first = value.find_first_not_of(L" \t\r\n");
    if (first == std::wstring::npos) {
        return {};
    }
    const auto last = value.find_last_not_of(L" \t\r\n");
    return value.substr(first, last - first + 1);
}

std::wstring timestamp()
{
    SYSTEMTIME time {};
    GetLocalTime(&time);

    std::wostringstream output;
    output << std::setfill(L'0')
           << L'['
           << std::setw(2) << time.wHour << L':'
           << std::setw(2) << time.wMinute << L':'
           << std::setw(2) << time.wSecond << L'.'
           << std::setw(3) << time.wMilliseconds
           << L"] ";
    return output.str();
}

std::int64_t steadyMicroseconds()
{
    return std::chrono::duration_cast<std::chrono::microseconds>(
               std::chrono::steady_clock::now().time_since_epoch())
        .count();
}

void updateMaximum(
    std::atomic<std::int64_t>& maximum,
    std::int64_t value)
{
    auto current = maximum.load(std::memory_order_relaxed);
    while (value > current
           && !maximum.compare_exchange_weak(
               current,
               value,
               std::memory_order_relaxed)) {
    }
}

void recordCadence(
    std::atomic<std::uint64_t>& count,
    std::atomic<std::int64_t>& previousMicroseconds,
    std::atomic<std::int64_t>& maximumGapMicroseconds,
    std::atomic<std::uint64_t>& longGaps)
{
    constexpr std::int64_t longGapMicroseconds = 80'000;
    count.fetch_add(1, std::memory_order_relaxed);
    const auto now = steadyMicroseconds();
    const auto previous =
        previousMicroseconds.exchange(now, std::memory_order_relaxed);
    if (previous <= 0) {
        return;
    }
    const auto gap = now - previous;
    updateMaximum(maximumGapMicroseconds, gap);
    if (gap > longGapMicroseconds) {
        longGaps.fetch_add(1, std::memory_order_relaxed);
    }
}

} // namespace

struct MainWindowPrivate;

struct UiBridge final : std::enable_shared_from_this<UiBridge> {
    explicit UiBridge(
        Microsoft::UI::Dispatching::DispatcherQueue queue)
        : dispatcher(std::move(queue))
    {
    }

    bool post(std::function<void(MainWindowPrivate&)> action)
    {
        auto self = shared_from_this();
        return dispatcher.TryEnqueue(
            [self = std::move(self), action = std::move(action)] {
                auto* current =
                    self->target.load(std::memory_order_acquire);
                if (current) {
                    action(*current);
                }
            });
    }

    Microsoft::UI::Dispatching::DispatcherQueue dispatcher { nullptr };
    std::atomic<MainWindowPrivate*> target { nullptr };
};

struct CallbackState final {
    std::atomic<bool> firstVideoFrame { false };
    std::atomic<bool> firstAudioFrame { false };
    std::atomic<bool> firstPresentedFrame { false };
    std::atomic<std::uint64_t> videoCallbacks { 0 };
    std::atomic<std::uint64_t> audioCallbacks { 0 };
    std::atomic<std::int64_t> previousVideoMicroseconds { 0 };
    std::atomic<std::int64_t> maximumVideoGapMicroseconds { 0 };
    std::atomic<std::uint64_t> longVideoGaps { 0 };

    void reset()
    {
        firstVideoFrame.store(false);
        firstAudioFrame.store(false);
        firstPresentedFrame.store(false);
        videoCallbacks.store(0);
        audioCallbacks.store(0);
        previousVideoMicroseconds.store(0);
        maximumVideoGapMicroseconds.store(0);
        longVideoGaps.store(0);
    }
};

struct MainWindowPrivate final {
    explicit MainWindowPrivate(MainWindow& owner)
        : owner_(owner),
          uiBridge_(std::make_shared<UiBridge>(
              owner.DispatcherQueue())),
          callbackState_(std::make_shared<CallbackState>())
    {
        uiBridge_->target.store(this, std::memory_order_release);

        TitleAndSize();
        InitializePlayer();
        InitializeTimers();
        RegisterPanelEvents();
        AppendLog(L"WinUI 3 player initialized");
    }

    ~MainWindowPrivate()
    {
        Shutdown();
    }

    MainWindowPrivate(const MainWindowPrivate&) = delete;
    MainWindowPrivate& operator=(const MainWindowPrivate&) = delete;

    void TitleAndSize()
    {
        owner_.Title(L"QtAVCore WinUI 3 Player");

        Microsoft::UI::Xaml::Window window = owner_;
        check_hresult(
            window.as<IWindowNative>()->get_WindowHandle(&windowHandle_));

        const UINT dpi = GetDpiForWindow(windowHandle_);
        const float scale = static_cast<float>(dpi) / 96.0F;
        SetWindowPos(
            windowHandle_,
            nullptr,
            0,
            0,
            static_cast<int>(1180.0F * scale),
            static_cast<int>(760.0F * scale),
            SWP_NOMOVE | SWP_NOZORDER);
    }

    void InitializePlayer()
    {
        player_ = std::make_unique<qtav::Player>();
        player_
            ->setAudioFrameConverter(
                std::make_shared<qtav::SwresampleAudioConverter>())
            .setAudioSink(
                std::make_shared<qtav::WasapiAudioSink>());

        auto bridge = uiBridge_;
        auto callbackState = callbackState_;
        player_
            ->onStateChanged(
                [bridge](qtav::State state) {
                    bridge->post(
                        [state](MainWindowPrivate& window) {
                            window.HandleStateChanged(state);
                        });
                })
            .onMediaStatus(
                [bridge](
                    qtav::MediaStatus oldStatus,
                    qtav::MediaStatus newStatus) {
                    bridge->post(
                        [oldStatus, newStatus](
                            MainWindowPrivate& window) {
                            window.HandleMediaStatus(
                                oldStatus,
                                newStatus);
                        });
                    return false;
                })
            .onEvent(
                [bridge](const qtav::MediaEvent& event) {
                    const std::string category = event.category;
                    const std::string detail = event.detail;
                    const int error = event.error;
                    bridge->post(
                        [category, detail, error](
                            MainWindowPrivate& window) {
                            window.HandleMediaEvent(
                                category,
                                detail,
                                error);
                        });
                    return false;
                })
            .onVideoFrame(
                [bridge, callbackState](
                    const qtav::VideoFrame& frame,
                    int track) {
                    recordCadence(
                        callbackState->videoCallbacks,
                        callbackState->previousVideoMicroseconds,
                        callbackState->maximumVideoGapMicroseconds,
                        callbackState->longVideoGaps);
                    if (callbackState->firstVideoFrame.exchange(true)) {
                        return;
                    }
                    const int width = frame.width();
                    const int height = frame.height();
                    const std::string format = frame.formatName();
                    const std::string colorSpace = frame.colorSpace();
                    const bool hardware = frame.hasHardwareFrame();
                    bridge->post(
                        [width,
                         height,
                         format,
                         colorSpace,
                         hardware,
                         track](MainWindowPrivate& window) {
                            std::wostringstream message;
                            message
                                << L"video track " << track << L": "
                                << width << L'x' << height << L", "
                                << to_hstring(format).c_str()
                                << L", color="
                                << to_hstring(colorSpace).c_str()
                                << L", decode="
                                << (hardware ? L"D3D11VA" : L"software");
                            window.AppendLog(message.str());
                        });
                })
            .onAudioFrame(
                [bridge, callbackState](
                    const qtav::AudioFrame& frame,
                    int track) {
                    callbackState->audioCallbacks.fetch_add(
                        1,
                        std::memory_order_relaxed);
                    if (callbackState->firstAudioFrame.exchange(true)) {
                        return;
                    }
                    const int sampleRate = frame.sampleRate();
                    const int channels = frame.channels();
                    const std::string format = frame.formatName();
                    bridge->post(
                        [sampleRate, channels, format, track](
                            MainWindowPrivate& window) {
                            std::wostringstream message;
                            message
                                << L"audio track " << track << L": "
                                << sampleRate << L" Hz, "
                                << channels << L" ch, "
                                << to_hstring(format).c_str();
                            window.AppendLog(message.str());
                        });
                });
    }

    void InitializeTimers()
    {
        progressTimer_ = owner_.DispatcherQueue().CreateTimer();
        progressTimer_.Interval(std::chrono::milliseconds(250));
        progressTimer_.IsRepeating(true);
        auto bridge = uiBridge_;
        progressTimer_.Tick(
            [bridge](auto const&, auto const&) {
                bridge->post(
                    [](MainWindowPrivate& window) {
                        window.UpdateProgress();
                    });
            });
        progressTimer_.Start();

        seekTimer_ = owner_.DispatcherQueue().CreateTimer();
        seekTimer_.Interval(std::chrono::milliseconds(180));
        seekTimer_.IsRepeating(false);
        seekTimer_.Tick(
            [bridge](auto const&, auto const&) {
                bridge->post(
                    [](MainWindowPrivate& window) {
                        window.CommitSeek();
                    });
            });
    }

    void RegisterPanelEvents()
    {
        auto bridge = uiBridge_;
        auto progressSlider = owner_.ProgressSlider();
        progressSlider.AddHandler(
            Microsoft::UI::Xaml::UIElement::PointerPressedEvent(),
            box_value<Microsoft::UI::Xaml::Input::PointerEventHandler>(
                Microsoft::UI::Xaml::Input::PointerEventHandler {
                    [bridge](
                        IInspectable const&,
                        Microsoft::UI::Xaml::Input::
                            PointerRoutedEventArgs const&) {
                        bridge->post(
                            [](MainWindowPrivate& window) {
                                window.BeginProgressInteraction();
                            });
                    },
                }
            ),
            true);
        progressSlider.AddHandler(
            Microsoft::UI::Xaml::UIElement::PointerReleasedEvent(),
            box_value<Microsoft::UI::Xaml::Input::PointerEventHandler>(
                Microsoft::UI::Xaml::Input::PointerEventHandler {
                    [bridge](
                        IInspectable const&,
                        Microsoft::UI::Xaml::Input::
                            PointerRoutedEventArgs const&) {
                        bridge->post(
                            [](MainWindowPrivate& window) {
                                window.EndProgressInteraction();
                            });
                    },
                }
            ),
            true);
        progressSlider.AddHandler(
            Microsoft::UI::Xaml::UIElement::PointerCaptureLostEvent(),
            box_value<Microsoft::UI::Xaml::Input::PointerEventHandler>(
                Microsoft::UI::Xaml::Input::PointerEventHandler {
                    [bridge](
                        IInspectable const&,
                        Microsoft::UI::Xaml::Input::
                            PointerRoutedEventArgs const&) {
                        bridge->post(
                            [](MainWindowPrivate& window) {
                                window.EndProgressInteraction();
                            });
                    },
                }
            ),
            true);
        owner_.VideoPanel().SizeChanged(
            [bridge](auto const&, auto const&) {
                bridge->post(
                    [](MainWindowPrivate& window) {
                        window.ResizeVideoOutput();
                    });
            });
        owner_.VideoPanel().CompositionScaleChanged(
            [bridge](auto const&, auto const&) {
                bridge->post(
                    [](MainWindowPrivate& window) {
                        window.ResizeVideoOutput();
                    });
            });
    }

    Windows::Foundation::IAsyncAction PickAndOpenFile()
    {
        try {
            Windows::Storage::Pickers::FileOpenPicker picker;
            picker.ViewMode(
                Windows::Storage::Pickers::PickerViewMode::Thumbnail);
            picker.SuggestedStartLocation(
                Windows::Storage::Pickers::PickerLocationId::
                    VideosLibrary);
            picker.FileTypeFilter().Append(L"*");

            check_hresult(
                picker.as<IInitializeWithWindow>()->Initialize(
                    windowHandle_));

            const auto file = co_await picker.PickSingleFileAsync();
            if (!file) {
                co_return;
            }
            const std::wstring path = file.Path().c_str();
            if (path.empty()) {
                SetStatus(L"所选文件没有可供 FFmpeg 打开的本地路径");
                AppendLog(L"file picker returned an empty local path");
                co_return;
            }

            owner_.UrlTextBox().Text(path);
            OpenMedia(path);
        } catch (hresult_error const& error) {
            SetStatus(L"打开文件失败");
            AppendLog(
                L"file picker error: "
                + std::wstring(error.message().c_str()));
        }
    }

    void OpenUrlInput()
    {
        OpenMedia(trim(owner_.UrlTextBox().Text().c_str()));
    }

    void OpenMedia(std::wstring const& input)
    {
        if (input.empty()) {
            SetStatus(L"请输入媒体 URL 或选择本地文件");
            return;
        }

        if (!videoOutput_ || !videoOutput_->isOpen()) {
            if (!InitializeVideoOutput()) {
                return;
            }
        }

        callbackState_->reset();
        static_cast<void>(videoOutput_->takeStatistics());
        cadenceReportAt_ = std::chrono::steady_clock::now();
        seekTimer_.Stop();
        scrubbing_ = false;
        seekPending_ = false;
        ++seekSerial_;
        durationMilliseconds_ = 0;
        seekable_ = false;
        updatingProgress_ = true;
        owner_.ProgressSlider().Maximum(1.0);
        owner_.ProgressSlider().Value(0.0);
        owner_.ProgressSlider().IsEnabled(false);
        owner_.CurrentTimeText().Text(L"00:00");
        owner_.DurationText().Text(L"--:--");
        updatingProgress_ = false;

        owner_.MediaNameText().Text(input);
        SetStatus(L"正在打开媒体…");
        AppendLog(L"open: " + input);

        player_->setMedia(to_string(hstring(input)));
        player_->setState(qtav::State::Playing);
    }

    void TogglePlayPause()
    {
        if (!player_) {
            return;
        }

        const auto state = player_->state();
        if (state == qtav::State::Playing) {
            player_->setState(qtav::State::Paused);
            return;
        }

        if (player_->url().empty()) {
            OpenUrlInput();
            return;
        }
        player_->setState(qtav::State::Playing);
    }

    void Stop()
    {
        seekTimer_.Stop();
        scrubbing_ = false;
        seekPending_ = false;
        ++seekSerial_;
        if (player_) {
            player_->setState(qtav::State::Stopped);
        }
    }

    void OnProgressValueChanged(double value)
    {
        if (updatingProgress_ || !seekable_ || !player_) {
            return;
        }
        pendingSeekMilliseconds_ =
            static_cast<std::int64_t>(std::llround(value));
        owner_.CurrentTimeText().Text(
            formatTime(pendingSeekMilliseconds_));
        if (scrubbing_) {
            return;
        }
        seekTimer_.Stop();
        seekTimer_.Start();
    }

    void BeginProgressInteraction()
    {
        if (!seekable_ || !player_) {
            return;
        }
        scrubbing_ = true;
        seekTimer_.Stop();
    }

    void EndProgressInteraction()
    {
        if (!scrubbing_) {
            return;
        }
        scrubbing_ = false;
        pendingSeekMilliseconds_ = static_cast<std::int64_t>(
            std::llround(owner_.ProgressSlider().Value()));
        seekTimer_.Stop();
        CommitSeek();
    }

    void CommitSeek()
    {
        if (!seekable_ || !player_) {
            return;
        }
        const auto target = std::clamp<std::int64_t>(
            pendingSeekMilliseconds_,
            0,
            durationMilliseconds_);
        const auto serial = ++seekSerial_;
        seekPending_ = true;
        updatingProgress_ = true;
        owner_.ProgressSlider().Value(static_cast<double>(target));
        owner_.CurrentTimeText().Text(formatTime(target));
        updatingProgress_ = false;

        auto bridge = uiBridge_;
        if (player_->seek(
                target,
                qtav::SeekFlag::FromStart,
                [bridge, serial](std::int64_t result) {
                    bridge->post(
                        [serial, result](MainWindowPrivate& window) {
                            window.HandleSeekCompleted(serial, result);
                        });
                })) {
            AppendLog(L"seek: " + formatTime(target));
        } else {
            seekPending_ = false;
            AppendLog(L"seek request was rejected");
        }
    }

    void HandleSeekCompleted(
        std::uint64_t serial,
        std::int64_t result)
    {
        if (serial != seekSerial_) {
            return;
        }
        seekPending_ = false;
        if (result < 0) {
            AppendLog(L"seek failed");
        }
        UpdateProgress();
    }

    void HandleStateChanged(qtav::State state)
    {
        owner_.PlayPauseButton().Content(
            box_value(
                state == qtav::State::Playing
                    ? L"暂停"
                    : L"播放"));

        std::wostringstream message;
        message << L"state: " << stateName(state);
        AppendLog(message.str());
    }

    void HandleMediaStatus(
        qtav::MediaStatus oldStatus,
        qtav::MediaStatus newStatus)
    {
        std::wostringstream message;
        message
            << L"status: " << statusName(oldStatus)
            << L" -> " << statusName(newStatus);
        AppendLog(message.str());

        switch (newStatus) {
        case qtav::MediaStatus::NoMedia:
            SetStatus(L"尚未打开媒体");
            break;
        case qtav::MediaStatus::Loading:
            SetStatus(L"正在加载…");
            break;
        case qtav::MediaStatus::Loaded:
            if (durationMilliseconds_ <= 0) {
                UpdateMediaInfo();
            }
            SetStatus(L"已加载");
            break;
        case qtav::MediaStatus::Buffering:
            SetStatus(L"缓冲中…");
            break;
        case qtav::MediaStatus::EndOfMedia:
            seekPending_ = false;
            SetStatus(L"播放结束");
            break;
        case qtav::MediaStatus::Invalid:
            seekPending_ = false;
            SetStatus(L"媒体无效；请查看 Debug 窗口");
            break;
        }
    }

    void HandleMediaEvent(
        std::string const& category,
        std::string const& detail,
        int error)
    {
        std::wostringstream message;
        message
            << L"event " << to_hstring(category).c_str();
        if (error != 0) {
            message << L" (" << error << L')';
        }
        if (!detail.empty()) {
            message << L": " << to_hstring(detail).c_str();
        }
        AppendLog(message.str());

        if (category.find("error") != std::string::npos
            || category == "open") {
            SetStatus(L"播放错误；请查看 Debug 窗口");
        }
    }

    void HandleRenderEvent(
        qtav::D3D11VideoOutputEventType type,
        std::string const& detail)
    {
        std::wostringstream message;
        message << L"renderer " << renderEventName(type);
        if (!detail.empty()) {
            message << L": " << to_hstring(detail).c_str();
        }
        AppendLog(message.str());

        if (type == qtav::D3D11VideoOutputEventType::SurfaceLost) {
            SetStatus(L"视频表面已丢失；请查看 Debug 窗口");
        } else {
            SetStatus(L"视频渲染失败；请查看 Debug 窗口");
        }
    }

    void UpdateMediaInfo()
    {
        if (!player_) {
            return;
        }
        const auto info = player_->mediaInfo();
        durationMilliseconds_ =
            std::max<std::int64_t>(info.duration, 0);
        seekable_ =
            info.seekable && durationMilliseconds_ > 0;

        updatingProgress_ = true;
        owner_.ProgressSlider().Maximum(
            durationMilliseconds_ > 0
                ? static_cast<double>(durationMilliseconds_)
                : 1.0);
        owner_.ProgressSlider().Value(0.0);
        owner_.ProgressSlider().IsEnabled(seekable_);
        owner_.DurationText().Text(
            durationMilliseconds_ > 0
                ? formatTime(durationMilliseconds_)
                : L"--:--");
        updatingProgress_ = false;

        std::wostringstream message;
        message
            << L"media: duration="
            << durationMilliseconds_ << L" ms, seekable="
            << (info.seekable ? L"yes" : L"no")
            << L", tracks=" << info.tracks.size();
        AppendLog(message.str());
    }

    void UpdateProgress()
    {
        if (!player_) {
            return;
        }
        ReportCadenceMetrics();
        if (scrubbing_ || seekPending_) {
            return;
        }
        const auto position = std::max<std::int64_t>(
            player_->position(),
            0);
        owner_.CurrentTimeText().Text(formatTime(position));

        if (durationMilliseconds_ <= 0) {
            return;
        }
        updatingProgress_ = true;
        owner_.ProgressSlider().Value(
            static_cast<double>(
                std::min(position, durationMilliseconds_)));
        updatingProgress_ = false;
    }

    void ReportCadenceMetrics()
    {
        const auto now = std::chrono::steady_clock::now();
        const auto elapsed =
            std::chrono::duration<double>(now - cadenceReportAt_).count();
        if (elapsed < 5.0) {
            return;
        }
        cadenceReportAt_ = now;

        const auto video = callbackState_->videoCallbacks.exchange(0);
        const auto audio = callbackState_->audioCallbacks.exchange(0);
        const auto outputStatistics =
            videoOutput_
            ? videoOutput_->takeStatistics()
            : qtav::D3D11VideoOutputStatistics {};
        const auto requests = outputStatistics.renderRequests;
        const auto coalesced =
            outputStatistics.coalescedRenderRequests;
        const auto passes = outputStatistics.renderPasses;
        const auto rendered = outputStatistics.presentedFrames;
        const auto busyPresents = outputStatistics.busyPresents;
        const auto skippedRenders = outputStatistics.skippedRenders;
        const auto noFrameRenders =
            outputStatistics.noFrameRenderAttempts;
        const auto playerBusyRenders =
            outputStatistics.playerBusyRenderAttempts;
        const auto rendererBusyRenders =
            outputStatistics.rendererBusyRenderAttempts;
        const auto retryWakeups = outputStatistics.retryWakeups;
        const auto supersededRenderFrames =
            outputStatistics.supersededRenderFrames;
        const auto terminalRenderDrops =
            outputStatistics.terminalRenderDrops;
        const auto rendererStateBusyRenders =
            outputStatistics.rendererStateBusyRenderAttempts;
        const auto rendererSerializationBusyRenders =
            outputStatistics.rendererSerializationBusyRenderAttempts;
        const auto rendererContextBusyRenders =
            outputStatistics.rendererDeviceContextBusyRenderAttempts;
        const auto rendererReservationAwareContextBusyRenders =
            outputStatistics
                .rendererReservationAwareContextBusyRenderAttempts;
        const auto rendererUnreservedContextBusyRenders =
            outputStatistics.rendererUnreservedContextBusyRenderAttempts;
        const auto contextHandoffWaits =
            outputStatistics.contextHandoffWaits;
        const auto contextHandoffTimeouts =
            outputStatistics.contextHandoffTimeouts;
        const auto rendererInFlightBusyRenders =
            outputStatistics.rendererInFlightBusyRenderAttempts;
        const auto decoderSurfaceCopies =
            outputStatistics.decoderSurfaceCopies;
        const auto videoLongGaps =
            callbackState_->longVideoGaps.exchange(0);
        const auto renderLongGaps = outputStatistics.longRenderGaps;
        const auto maximumVideoGap =
            callbackState_->maximumVideoGapMicroseconds.exchange(0);
        const auto maximumRenderGap =
            outputStatistics.maximumRenderGapMicroseconds;
        const auto maximumRender =
            outputStatistics.maximumRenderMicroseconds;
        const auto maximumPresent =
            outputStatistics.maximumPresentMicroseconds;
        const auto maximumColorSetup =
            outputStatistics.maximumColorSetupMicroseconds;
        const auto maximumInterop =
            outputStatistics.maximumInteropMicroseconds;
        const auto maximumBufferUpdate =
            outputStatistics.maximumBufferUpdateMicroseconds;
        const auto maximumDraw =
            outputStatistics.maximumDrawMicroseconds;
        if (video == 0 && audio == 0 && requests == 0
            && passes == 0 && rendered == 0) {
            return;
        }

        std::wostringstream message;
        message
            << std::fixed << std::setprecision(1)
            << L"cadence " << elapsed << L"s: scheduled-video="
            << static_cast<double>(video) / elapsed
            << L" fps, audio=" << static_cast<double>(audio) / elapsed
            << L" fps, render-requests=" << requests
            << L", passes=" << passes
            << L", rendered=" << static_cast<double>(rendered) / elapsed
            << L" fps, coalesced=" << coalesced
            << L", present-busy=" << busyPresents
            << L", render-skipped=" << skippedRenders
            << L" (no-frame/player-busy/renderer-busy="
            << noFrameRenders << L'/' << playerBusyRenders << L'/'
            << rendererBusyRenders << L')'
            << L", retry/superseded/terminal=" << retryWakeups << L'/'
            << supersededRenderFrames << L'/' << terminalRenderDrops
            << L", renderer-busy(state/serialize/context/in-flight)="
            << rendererStateBusyRenders << L'/'
            << rendererSerializationBusyRenders << L'/'
            << rendererContextBusyRenders << L'/'
            << rendererInFlightBusyRenders
            << L", context-owner(reservation-aware/unreserved)="
            << rendererReservationAwareContextBusyRenders << L'/'
            << rendererUnreservedContextBusyRenders
            << L", handoff(wait/timeout)=" << contextHandoffWaits << L'/'
            << contextHandoffTimeouts
            << L", decoder-copies=" << decoderSurfaceCopies
            << L", >80ms gaps(video/render)="
            << videoLongGaps << L'/' << renderLongGaps
            << L", max-gap-ms="
            << static_cast<double>(maximumVideoGap) / 1'000.0
            << L'/'
            << static_cast<double>(maximumRenderGap) / 1'000.0
            << L", max-render/present-ms="
            << static_cast<double>(maximumRender) / 1'000.0
            << L'/'
            << static_cast<double>(maximumPresent) / 1'000.0
            << L", max-stage-ms(color/interop/buffer/draw)="
            << static_cast<double>(maximumColorSetup) / 1'000.0
            << L'/'
            << static_cast<double>(maximumInterop) / 1'000.0
            << L'/'
            << static_cast<double>(maximumBufferUpdate) / 1'000.0
            << L'/'
            << static_cast<double>(maximumDraw) / 1'000.0;
        AppendLog(message.str());
    }

    void SetStatus(std::wstring const& value)
    {
        owner_.StatusText().Text(value);
    }

    void ShowDebugWindow()
    {
        if (debugWindow_) {
            debugWindow_.Activate();
            return;
        }

        debugWindow_ =
            winrt::make<
                winrt::QtAVWinUI3::implementation::DebugWindow>();
        debugWindow_.SetLog(AllDebugText());

        auto bridge = uiBridge_;
        debugWindow_.Closed(
            [bridge](auto const&, auto const&) {
                bridge->post(
                    [](MainWindowPrivate& window) {
                        window.DebugWindowClosed();
                    });
            });
        debugWindow_.Activate();
        AppendLog(L"Debug window opened");
    }

    void CloseDebugWindow()
    {
        if (!debugWindow_) {
            return;
        }
        auto window = debugWindow_;
        debugWindow_ = nullptr;
        window.Close();
    }

    void DebugWindowClosed()
    {
        debugWindow_ = nullptr;
        owner_.DebugToggle().IsChecked(
            box_value(false).as<
                Windows::Foundation::IReference<bool>>());
    }

    void AppendLog(std::wstring message)
    {
        message.insert(0, timestamp());
        debugLines_.push_back(message);
        bool trimmed = false;
        while (debugLines_.size() > maximumDebugLines_) {
            debugLines_.pop_front();
            trimmed = true;
        }

        if (debugWindow_) {
            if (trimmed) {
                debugWindow_.SetLog(AllDebugText());
            } else {
                debugWindow_.AppendLine(message);
            }
        }
    }

    hstring AllDebugText() const
    {
        std::wstring result;
        for (const auto& line : debugLines_) {
            if (!result.empty()) {
                result.append(L"\r\n");
            }
            result.append(line);
        }
        return hstring(result);
    }

    std::pair<UINT, UINT> PanelPixelSize() const
    {
        const auto panel = owner_.VideoPanel();
        const auto scaleX =
            std::max(panel.CompositionScaleX(), 0.01F);
        const auto scaleY =
            std::max(panel.CompositionScaleY(), 0.01F);
        const auto width = static_cast<UINT>(std::max(
            1LL,
            std::llround(panel.ActualWidth() * scaleX)));
        const auto height = static_cast<UINT>(std::max(
            1LL,
            std::llround(panel.ActualHeight() * scaleY)));
        return { width, height };
    }

    bool InitializeVideoOutput()
    {
        if ((videoOutput_ && videoOutput_->isOpen()) || shuttingDown_) {
            return videoOutput_ && videoOutput_->isOpen();
        }

        const auto panel = owner_.VideoPanel();
        const auto panelNative = panel.as<ISwapChainPanelNative>();
        const auto [width, height] = PanelPixelSize();

        qtav::D3D11CompositionSurface surface;
        surface.size = {
            static_cast<int>(width),
            static_cast<int>(height),
        };
        surface.compositionScaleX =
            std::max(panel.CompositionScaleX(), 0.01F);
        surface.compositionScaleY =
            std::max(panel.CompositionScaleY(), 0.01F);
        surface.bindSwapChain =
            [panelNative](IDXGISwapChain1* swapChain) {
                return panelNative->SetSwapChain(swapChain);
            };
        surface.window = windowHandle_;

        auto output = std::make_unique<qtav::D3D11VideoOutput>();
        auto bridge = uiBridge_;
        auto callbackState = callbackState_;
        output
            ->setEventCallback(
                [bridge](const qtav::D3D11VideoOutputEvent& event) {
                    const auto type = event.type;
                    const std::string detail = event.detail;
                    bridge->post(
                        [type, detail](MainWindowPrivate& window) {
                            window.HandleRenderEvent(type, detail);
                        });
                })
            .setFramePresentedCallback(
                [bridge, callbackState](double timestamp) {
                    if (callbackState->firstPresentedFrame.exchange(
                            true)) {
                        return;
                    }
                    const auto milliseconds =
                        static_cast<std::int64_t>(
                            std::llround(timestamp * 1000.0));
                    bridge->post(
                        [milliseconds](MainWindowPrivate& window) {
                            window.AppendLog(
                                L"first video frame presented at "
                                + formatTime(milliseconds));
                            if (!window.videoOutput_) {
                                return;
                            }
                            const auto color =
                                window.videoOutput_->colorInfo();
                            std::wostringstream message;
                            message
                                << L"output color: "
                                << (color.isHdrOutput()
                                        ? L"HDR active"
                                        : L"SDR")
                                << L", "
                                << (color.colorSpace
                                            == qtav::
                                                D3D11PresentationColorSpace::
                                                    ScRGB
                                        ? L"FP16 scRGB"
                                        : color.colorSpace
                                                == qtav::
                                                    D3D11PresentationColorSpace::
                                                        HDR10
                                        ? L"RGB10/PQ"
                                        : L"BGRA8")
                                << L", SDR white "
                                << std::fixed << std::setprecision(0)
                                << color.sdrWhiteLevelNits
                                << L" nits, display peak "
                                << color.maximumLuminanceNits
                                << L" nits";
                            window.AppendLog(message.str());
                        });
                });

        qtav::D3D11VideoOutputOptions outputOptions;
        // The player surface is opaque. Prefer native RGB10/PQ so ordinary
        // HDR10 and Dolby Vision both avoid the extra scRGB/DWM conversion and
        // follow the same presentation model as dedicated video renderers.
        outputOptions.hdrPresentationMode =
            qtav::D3D11HdrPresentationMode::HDR10;
        outputOptions.alphaMode = DXGI_ALPHA_MODE_IGNORE;
        if (!output->open(std::move(surface), outputOptions)
            || !output->attach(*player_)) {
            SetStatus(L"D3D11 输出初始化失败；请查看 Debug 窗口");
            AppendLog(
                L"video output initialization failed: "
                + std::wstring(
                    to_hstring(output->lastError()).c_str()));
            return false;
        }

        AppendLog(
            L"graphics: "
            + std::wstring(
                to_hstring(output->deviceDescription()).c_str())
            + L", library-owned HDR-aware RGB10/PQ "
              L"composition output");
        AppendLog(
            L"video path: library-owned render thread, "
            L"D3D11VA/libplacebo raw-plane path preferred, "
            L"software fallback");
        videoOutput_ = std::move(output);
        return true;
    }

    void ResizeVideoOutput()
    {
        if (shuttingDown_) {
            return;
        }
        if (!videoOutput_ || !videoOutput_->isOpen()) {
            InitializeVideoOutput();
            return;
        }

        const auto panel = owner_.VideoPanel();
        const auto [width, height] = PanelPixelSize();
        if (!videoOutput_->resize(
                {
                    static_cast<int>(width),
                    static_cast<int>(height),
                },
                std::max(panel.CompositionScaleX(), 0.01F),
                std::max(panel.CompositionScaleY(), 0.01F))) {
            SetStatus(L"视频表面调整失败；请查看 Debug 窗口");
            AppendLog(
                L"video output resize failed: "
                + std::wstring(
                    to_hstring(videoOutput_->lastError()).c_str()));
            return;
        }

        std::wostringstream message;
        message << L"surface resized: " << width << L'x' << height;
        AppendLog(message.str());
    }

    void ReleaseVideoOutput() noexcept
    {
        if (!videoOutput_) {
            return;
        }
        videoOutput_->detach();
        videoOutput_->close();
        videoOutput_.reset();
    }

    void Shutdown()
    {
        if (shuttingDown_) {
            return;
        }
        shuttingDown_ = true;
        uiBridge_->target.store(nullptr, std::memory_order_release);
        progressTimer_.Stop();
        seekTimer_.Stop();

        if (debugWindow_) {
            auto debug = debugWindow_;
            debugWindow_ = nullptr;
            debug.Close();
        }

        if (player_) {
            player_->setState(qtav::State::Stopped);
        }
        ReleaseVideoOutput();
        player_.reset();
    }

    MainWindow& owner_;
    HWND windowHandle_ = nullptr;
    std::shared_ptr<UiBridge> uiBridge_;
    std::shared_ptr<CallbackState> callbackState_;
    std::unique_ptr<qtav::Player> player_;
    std::unique_ptr<qtav::D3D11VideoOutput> videoOutput_;

    Microsoft::UI::Dispatching::DispatcherQueueTimer
        progressTimer_ { nullptr };
    Microsoft::UI::Dispatching::DispatcherQueueTimer
        seekTimer_ { nullptr };

    winrt::QtAVWinUI3::DebugWindow
        debugWindow_ { nullptr };
    std::deque<std::wstring> debugLines_;
    static constexpr std::size_t maximumDebugLines_ = 1000;

    std::int64_t durationMilliseconds_ = 0;
    std::int64_t pendingSeekMilliseconds_ = 0;
    std::uint64_t seekSerial_ = 0;
    bool seekable_ = false;
    bool updatingProgress_ = false;
    bool scrubbing_ = false;
    bool seekPending_ = false;
    std::chrono::steady_clock::time_point cadenceReportAt_ =
        std::chrono::steady_clock::now();
    bool shuttingDown_ = false;
};

MainWindow::MainWindow()
{
    InitializeComponent();
    impl_ = std::make_unique<MainWindowPrivate>(*this);
}

MainWindow::~MainWindow()
{
    if (impl_) {
        impl_->Shutdown();
    }
}

fire_and_forget MainWindow::OpenFile_Click(
    IInspectable const&,
    Microsoft::UI::Xaml::RoutedEventArgs const&)
{
    auto lifetime = get_strong();
    co_await impl_->PickAndOpenFile();
}

void MainWindow::OpenUrl_Click(
    IInspectable const&,
    Microsoft::UI::Xaml::RoutedEventArgs const&)
{
    impl_->OpenUrlInput();
}

void MainWindow::PlayPause_Click(
    IInspectable const&,
    Microsoft::UI::Xaml::RoutedEventArgs const&)
{
    impl_->TogglePlayPause();
}

void MainWindow::Stop_Click(
    IInspectable const&,
    Microsoft::UI::Xaml::RoutedEventArgs const&)
{
    impl_->Stop();
}

void MainWindow::UrlTextBox_KeyDown(
    IInspectable const&,
    Microsoft::UI::Xaml::Input::KeyRoutedEventArgs const& event)
{
    if (event.Key() == Windows::System::VirtualKey::Enter) {
        event.Handled(true);
        impl_->OpenUrlInput();
    }
}

void MainWindow::ProgressSlider_ValueChanged(
    IInspectable const&,
    Microsoft::UI::Xaml::Controls::Primitives::
        RangeBaseValueChangedEventArgs const& event)
{
    if (impl_) {
        impl_->OnProgressValueChanged(event.NewValue());
    }
}

void MainWindow::DebugToggle_Checked(
    IInspectable const&,
    Microsoft::UI::Xaml::RoutedEventArgs const&)
{
    impl_->ShowDebugWindow();
}

void MainWindow::DebugToggle_Unchecked(
    IInspectable const&,
    Microsoft::UI::Xaml::RoutedEventArgs const&)
{
    impl_->CloseDebugWindow();
}

void MainWindow::VideoPanel_Loaded(
    IInspectable const&,
    Microsoft::UI::Xaml::RoutedEventArgs const&)
{
    impl_->InitializeVideoOutput();
    impl_->ResizeVideoOutput();
}

void MainWindow::Window_Closed(
    IInspectable const&,
    Microsoft::UI::Xaml::WindowEventArgs const&)
{
    impl_->Shutdown();
}

} // namespace winrt::QtAVWinUI3::implementation
