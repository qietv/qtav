// SPDX-License-Identifier: GPL-3.0-or-later
#include "pch.h"

#include "MainWindow.xaml.h"

#if __has_include("MainWindow.g.cpp")
#  include "MainWindow.g.cpp"
#endif

#include "DebugWindow.xaml.h"

#include <microsoft.ui.xaml.media.dxinterop.h>
#include <microsoft.ui.xaml.window.h>

#include <d3d11.h>
#include <dxgi1_4.h>
#include <shobjidl_core.h>
#include <wrl/client.h>

#include <qtav/d3d11_frame_interop.h>
#include <qtav/d3d11_video_renderer.h>
#include <qtav/d3d11va_hardware_decoder.h>
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
#include <iterator>
#include <memory>
#include <sstream>
#include <string>
#include <utility>

namespace winrt::QtAVWinUI3::implementation {

namespace {

using ::Microsoft::WRL::ComPtr;

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

const wchar_t* renderEventName(qtav::VideoRenderEventType type)
{
    switch (type) {
    case qtav::VideoRenderEventType::RedrawRequested:
        return L"redraw-requested";
    case qtav::VideoRenderEventType::SurfaceLost:
        return L"surface-lost";
    case qtav::VideoRenderEventType::Error:
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

std::wstring errorText(HRESULT status)
{
    return std::wstring(
        hresult_error(status).message().c_str());
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
    std::atomic<bool> renderPending { false };
};

struct CallbackState final {
    std::atomic<bool> firstVideoFrame { false };
    std::atomic<bool> firstAudioFrame { false };
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
                })
            .setRenderCallback(
                [bridge](void*) {
                    if (bridge->renderPending.exchange(true)) {
                        return;
                    }
                    if (!bridge->post(
                            [bridge](MainWindowPrivate& window) {
                                bridge->renderPending.store(false);
                                window.RenderCurrentFrame();
                            })) {
                        bridge->renderPending.store(false);
                    }
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
        owner_.VideoPanel().SizeChanged(
            [bridge](auto const&, auto const&) {
                bridge->post(
                    [](MainWindowPrivate& window) {
                        window.ResizeSwapChain();
                    });
            });
        owner_.VideoPanel().CompositionScaleChanged(
            [bridge](auto const&, auto const&) {
                bridge->post(
                    [](MainWindowPrivate& window) {
                        window.ResizeSwapChain();
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

        if (!graphicsInitialized_) {
            InitializeGraphics();
        }

        callbackState_->firstVideoFrame.store(false);
        callbackState_->firstAudioFrame.store(false);
        presentedVideoFrame_ = false;
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
        seekTimer_.Stop();
        seekTimer_.Start();
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
        if (player_->seek(target)) {
            AppendLog(L"seek: " + formatTime(target));
        } else {
            AppendLog(L"seek request was rejected");
        }
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
            UpdateMediaInfo();
            SetStatus(L"已加载");
            break;
        case qtav::MediaStatus::Buffering:
            SetStatus(L"缓冲中…");
            break;
        case qtav::MediaStatus::EndOfMedia:
            SetStatus(L"播放结束");
            break;
        case qtav::MediaStatus::Invalid:
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
        qtav::VideoRenderEventType type,
        std::string const& detail)
    {
        if (type == qtav::VideoRenderEventType::RedrawRequested) {
            RenderCurrentFrame();
            return;
        }

        std::wostringstream message;
        message << L"renderer " << renderEventName(type);
        if (!detail.empty()) {
            message << L": " << to_hstring(detail).c_str();
        }
        AppendLog(message.str());

        if (type == qtav::VideoRenderEventType::SurfaceLost) {
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

    HRESULT CreateDevice(
        D3D_DRIVER_TYPE driverType,
        UINT flags,
        ComPtr<ID3D11Device>& device,
        ComPtr<ID3D11DeviceContext>& context)
    {
        const D3D_FEATURE_LEVEL levels[] {
            D3D_FEATURE_LEVEL_11_1,
            D3D_FEATURE_LEVEL_11_0,
        };
        D3D_FEATURE_LEVEL selected {};
        HRESULT status = D3D11CreateDevice(
            nullptr,
            driverType,
            nullptr,
            flags,
            levels,
            static_cast<UINT>(std::size(levels)),
            D3D11_SDK_VERSION,
            &device,
            &selected,
            &context);
        if (status == E_INVALIDARG) {
            device.Reset();
            context.Reset();
            status = D3D11CreateDevice(
                nullptr,
                driverType,
                nullptr,
                flags,
                levels + 1,
                1,
                D3D11_SDK_VERSION,
                &device,
                &selected,
                &context);
        }
        return status;
    }

    bool InitializeGraphics()
    {
        if (graphicsInitialized_ || shuttingDown_) {
            return graphicsInitialized_;
        }

        try {
            const UINT videoFlags =
                D3D11_CREATE_DEVICE_BGRA_SUPPORT
                | D3D11_CREATE_DEVICE_VIDEO_SUPPORT;
            HRESULT status = CreateDevice(
                D3D_DRIVER_TYPE_HARDWARE,
                videoFlags,
                device_,
                context_);
            std::wstring deviceDescription =
                L"hardware D3D11 with video support";

            if (FAILED(status)) {
                status = CreateDevice(
                    D3D_DRIVER_TYPE_HARDWARE,
                    D3D11_CREATE_DEVICE_BGRA_SUPPORT,
                    device_,
                    context_);
                deviceDescription = L"hardware D3D11";
            }
            if (FAILED(status)) {
                status = CreateDevice(
                    D3D_DRIVER_TYPE_WARP,
                    D3D11_CREATE_DEVICE_BGRA_SUPPORT,
                    device_,
                    context_);
                deviceDescription = L"WARP D3D11";
            }
            check_hresult(status);

            deviceAccess_ = qtav::D3D11DeviceAccess::create(
                qtav::BorrowedD3D11Device(device_.Get()),
                qtav::BorrowedD3D11DeviceContext(context_.Get()));
            if (!deviceAccess_) {
                throw hresult_error(
                    E_FAIL,
                    L"QtAVCore rejected the D3D11 immediate context");
            }

            ComPtr<IDXGIDevice1> dxgiDevice;
            check_hresult(device_.As(&dxgiDevice));
            dxgiDevice->SetMaximumFrameLatency(1);

            ComPtr<IDXGIAdapter> adapter;
            check_hresult(dxgiDevice->GetAdapter(&adapter));
            ComPtr<IDXGIFactory2> factory;
            check_hresult(
                adapter->GetParent(IID_PPV_ARGS(&factory)));

            const auto [width, height] = PanelPixelSize();
            DXGI_SWAP_CHAIN_DESC1 description {};
            description.Width = width;
            description.Height = height;
            description.Format = DXGI_FORMAT_B8G8R8A8_UNORM;
            description.SampleDesc.Count = 1;
            description.BufferUsage =
                DXGI_USAGE_RENDER_TARGET_OUTPUT;
            description.BufferCount = 2;
            description.Scaling = DXGI_SCALING_STRETCH;
            description.SwapEffect =
                DXGI_SWAP_EFFECT_FLIP_SEQUENTIAL;
            description.AlphaMode =
                DXGI_ALPHA_MODE_PREMULTIPLIED;

            ComPtr<IDXGISwapChain1> swapChain;
            check_hresult(factory->CreateSwapChainForComposition(
                device_.Get(),
                &description,
                nullptr,
                &swapChain));
            check_hresult(swapChain.As(&swapChain_));

            auto panelNative =
                owner_.VideoPanel().as<ISwapChainPanelNative>();
            check_hresult(panelNative->SetSwapChain(
                swapChain_.Get()));

            CreateRenderTarget();
            UpdateSwapChainTransform();

            renderer_ =
                std::make_shared<qtav::D3D11VideoRenderer>(
                    deviceAccess_,
                    [this] {
                        return qtav::D3D11RenderTarget {
                            renderTargetView_.Get(),
                            nullptr,
                        };
                    });
            auto bridge = uiBridge_;
            renderer_->setEventCallback(
                [bridge](const qtav::VideoRenderEvent& event) {
                    const auto type = event.type;
                    const std::string detail = event.detail;
                    bridge->post(
                        [type, detail](
                            MainWindowPrivate& window) {
                            window.HandleRenderEvent(type, detail);
                        });
                });
            interop_ =
                std::make_shared<qtav::D3D11FrameInterop>(
                    deviceAccess_);
            renderer_->setHardwareFrameInterop(interop_);
            renderer_->setAllowSoftwareMappingFallback(true);

            qtav::VideoRenderConfig renderConfig;
            renderConfig.surfaceSize = {
                static_cast<int>(width),
                static_cast<int>(height),
            };
            renderConfig.aspectRatio =
                qtav::VideoAspectRatioMode::Fit;
            if (!renderer_->open(renderConfig)) {
                throw hresult_error(
                    E_FAIL,
                    L"QtAVCore D3D11 renderer failed to open");
            }

            player_
                ->setHardwareDecodeConfig(
                    qtav::d3d11vaHardwareDecodeConfig(
                        deviceAccess_))
                .setVideoRenderAPI(renderer_);

            surfaceWidth_ = width;
            surfaceHeight_ = height;
            graphicsInitialized_ = true;
            AppendLog(
                L"graphics: " + deviceDescription
                + L", BGRA8 flip-model SwapChainPanel");
            AppendLog(
                L"video path: D3D11VA/Video Processor preferred; "
                L"software decode/map fallback enabled");
            return true;
        } catch (hresult_error const& error) {
            SetStatus(L"D3D11 初始化失败；请查看 Debug 窗口");
            AppendLog(
                L"graphics initialization failed: "
                + std::wstring(error.message().c_str())
                + L" (" + errorText(error.code()) + L')');
            ReleaseGraphics();
            return false;
        }
    }

    void CreateRenderTarget()
    {
        ComPtr<ID3D11Texture2D> backBuffer;
        check_hresult(swapChain_->GetBuffer(
            0,
            IID_PPV_ARGS(&backBuffer)));
        check_hresult(device_->CreateRenderTargetView(
            backBuffer.Get(),
            nullptr,
            &renderTargetView_));
    }

    void UpdateSwapChainTransform()
    {
        if (!swapChain_) {
            return;
        }
        const auto panel = owner_.VideoPanel();
        const float scaleX =
            std::max(panel.CompositionScaleX(), 0.01F);
        const float scaleY =
            std::max(panel.CompositionScaleY(), 0.01F);
        const DXGI_MATRIX_3X2_F transform {
            1.0F / scaleX,
            0.0F,
            0.0F,
            1.0F / scaleY,
            0.0F,
            0.0F,
        };
        check_hresult(swapChain_->SetMatrixTransform(&transform));
    }

    void ResizeSwapChain()
    {
        if (shuttingDown_) {
            return;
        }
        if (!graphicsInitialized_) {
            InitializeGraphics();
            return;
        }

        const auto [width, height] = PanelPixelSize();
        if (width == surfaceWidth_ && height == surfaceHeight_) {
            try {
                UpdateSwapChainTransform();
            } catch (hresult_error const& error) {
                AppendLog(
                    L"swap-chain transform failed: "
                    + std::wstring(error.message().c_str()));
            }
            return;
        }

        try {
            {
                auto guard = deviceAccess_->contextGuard();
                ID3D11RenderTargetView* noTarget = nullptr;
                context_->OMSetRenderTargets(
                    1,
                    &noTarget,
                    nullptr);
                context_->Flush();
                renderTargetView_.Reset();
                check_hresult(swapChain_->ResizeBuffers(
                    2,
                    width,
                    height,
                    DXGI_FORMAT_B8G8R8A8_UNORM,
                    0));
                CreateRenderTarget();
            }

            UpdateSwapChainTransform();
            qtav::VideoRenderConfig renderConfig;
            renderConfig.surfaceSize = {
                static_cast<int>(width),
                static_cast<int>(height),
            };
            renderConfig.aspectRatio =
                qtav::VideoAspectRatioMode::Fit;
            if (!renderer_->configure(renderConfig)) {
                throw hresult_error(
                    E_FAIL,
                    L"QtAVCore renderer rejected the new surface size");
            }

            surfaceWidth_ = width;
            surfaceHeight_ = height;
            std::wostringstream message;
            message
                << L"surface resized: "
                << width << L'x' << height;
            AppendLog(message.str());
            RenderCurrentFrame();
        } catch (hresult_error const& error) {
            SetStatus(L"视频表面调整失败；请查看 Debug 窗口");
            AppendLog(
                L"swap-chain resize failed: "
                + std::wstring(error.message().c_str())
                + L" (" + errorText(error.code()) + L')');
        }
    }

    void RenderCurrentFrame()
    {
        if (!graphicsInitialized_
            || !renderTargetView_
            || !swapChain_
            || !player_) {
            return;
        }

        try {
            {
                auto guard = deviceAccess_->contextGuard();
                const float black[] { 0.0F, 0.0F, 0.0F, 1.0F };
                context_->ClearRenderTargetView(
                    renderTargetView_.Get(),
                    black);
            }

            const double timestamp = player_->renderVideo();
            if (timestamp < 0.0) {
                return;
            }
            check_hresult(swapChain_->Present(1, 0));
            if (!presentedVideoFrame_) {
                presentedVideoFrame_ = true;
                const auto milliseconds =
                    static_cast<std::int64_t>(
                        std::llround(timestamp * 1000.0));
                AppendLog(
                    L"first video frame presented at "
                    + formatTime(milliseconds));
            }
        } catch (hresult_error const& error) {
            SetStatus(L"视频渲染失败；请查看 Debug 窗口");
            AppendLog(
                L"render failed: "
                + std::wstring(error.message().c_str())
                + L" (" + errorText(error.code()) + L')');
        }
    }

    void ReleaseGraphics() noexcept
    {
        if (renderer_) {
            renderer_->close();
        }
        interop_.reset();
        renderer_.reset();
        deviceAccess_.reset();
        renderTargetView_.Reset();
        swapChain_.Reset();
        context_.Reset();
        device_.Reset();
        surfaceWidth_ = 0;
        surfaceHeight_ = 0;
        graphicsInitialized_ = false;
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
            player_.reset();
        }

        if (swapChain_) {
            try {
                auto panelNative =
                    owner_.VideoPanel().as<ISwapChainPanelNative>();
                panelNative->SetSwapChain(nullptr);
            } catch (...) {
            }
        }
        ReleaseGraphics();
    }

    MainWindow& owner_;
    HWND windowHandle_ = nullptr;
    std::shared_ptr<UiBridge> uiBridge_;
    std::shared_ptr<CallbackState> callbackState_;
    std::unique_ptr<qtav::Player> player_;

    Microsoft::UI::Dispatching::DispatcherQueueTimer
        progressTimer_ { nullptr };
    Microsoft::UI::Dispatching::DispatcherQueueTimer
        seekTimer_ { nullptr };

    ComPtr<ID3D11Device> device_;
    ComPtr<ID3D11DeviceContext> context_;
    ComPtr<IDXGISwapChain3> swapChain_;
    ComPtr<ID3D11RenderTargetView> renderTargetView_;
    std::shared_ptr<qtav::D3D11DeviceAccess> deviceAccess_;
    std::shared_ptr<qtav::D3D11VideoRenderer> renderer_;
    std::shared_ptr<qtav::D3D11FrameInterop> interop_;

    winrt::QtAVWinUI3::DebugWindow
        debugWindow_ { nullptr };
    std::deque<std::wstring> debugLines_;
    static constexpr std::size_t maximumDebugLines_ = 1000;

    UINT surfaceWidth_ = 0;
    UINT surfaceHeight_ = 0;
    std::int64_t durationMilliseconds_ = 0;
    std::int64_t pendingSeekMilliseconds_ = 0;
    bool seekable_ = false;
    bool updatingProgress_ = false;
    bool presentedVideoFrame_ = false;
    bool graphicsInitialized_ = false;
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
    impl_->InitializeGraphics();
    impl_->ResizeSwapChain();
}

void MainWindow::Window_Closed(
    IInspectable const&,
    Microsoft::UI::Xaml::WindowEventArgs const&)
{
    impl_->Shutdown();
}

} // namespace winrt::QtAVWinUI3::implementation
