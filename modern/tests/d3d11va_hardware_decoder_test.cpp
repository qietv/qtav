// SPDX-License-Identifier: LGPL-2.1-or-later

#if !defined(NOMINMAX)
#  define NOMINMAX
#endif

#include <d3d11.h>
#include <wrl/client.h>

#include <qtav/d3d11va_hardware_decoder.h>
#include <qtav/player.h>

#include <atomic>
#if defined(NDEBUG)
#  undef NDEBUG
#endif
#include <cassert>
#include <chrono>
#include <condition_variable>
#include <cstdint>
#include <cstdlib>
#include <iterator>
#include <iostream>
#include <memory>
#include <mutex>
#include <thread>
#include <utility>

#include "hardware_decode_device_internal.h"

extern "C" {
#include <libavutil/buffer.h>
#include <libavutil/hwcontext.h>
#include <libavutil/hwcontext_d3d11va.h>
}

namespace {

using Microsoft::WRL::ComPtr;

struct D3D11Resources {
    ComPtr<ID3D11Device> device;
    ComPtr<ID3D11DeviceContext> context;
};

D3D11Resources makeDevice()
{
    D3D11Resources result;
    D3D_FEATURE_LEVEL selected {};
    const D3D_FEATURE_LEVEL levels[] = {
        D3D_FEATURE_LEVEL_11_1,
        D3D_FEATURE_LEVEL_11_0,
    };
    HRESULT status = D3D11CreateDevice(
        nullptr,
        D3D_DRIVER_TYPE_HARDWARE,
        nullptr,
        0,
        levels,
        static_cast<UINT>(std::size(levels)),
        D3D11_SDK_VERSION,
        &result.device,
        &selected,
        &result.context);
    if (status == E_INVALIDARG) {
        status = D3D11CreateDevice(
            nullptr,
            D3D_DRIVER_TYPE_HARDWARE,
            nullptr,
            0,
            levels + 1,
            1,
            D3D11_SDK_VERSION,
            &result.device,
            &selected,
            &result.context);
    }
    if (FAILED(status)) {
        status = D3D11CreateDevice(
            nullptr,
            D3D_DRIVER_TYPE_WARP,
            nullptr,
            0,
            levels + 1,
            1,
            D3D11_SDK_VERSION,
            &result.device,
            &selected,
            &result.context);
    }
    if (FAILED(status)) {
        std::abort();
    }
    return result;
}

D3D11Resources makeWarpDevice()
{
    D3D11Resources result;
    D3D_FEATURE_LEVEL selected {};
    const D3D_FEATURE_LEVEL levels[] = {
        D3D_FEATURE_LEVEL_11_1,
        D3D_FEATURE_LEVEL_11_0,
    };
    HRESULT status = D3D11CreateDevice(
        nullptr,
        D3D_DRIVER_TYPE_WARP,
        nullptr,
        0,
        levels,
        static_cast<UINT>(std::size(levels)),
        D3D11_SDK_VERSION,
        &result.device,
        &selected,
        &result.context);
    if (status == E_INVALIDARG) {
        status = D3D11CreateDevice(
            nullptr,
            D3D_DRIVER_TYPE_WARP,
            nullptr,
            0,
            levels + 1,
            1,
            D3D11_SDK_VERSION,
            &result.device,
            &selected,
            &result.context);
    }
    if (FAILED(status)) {
        std::abort();
    }
    return result;
}

class TextureFrameData final : public qtav::HardwareFrameData {
public:
    TextureFrameData(
        ComPtr<ID3D11Texture2D> texture,
        std::uint32_t slice,
        qtav::PixelFormat format,
        qtav::HardwareDeviceType deviceType =
            qtav::HardwareDeviceType::D3D11,
        int width = 64,
        int height = 32)
        : texture_(std::move(texture))
        , slice_(slice)
        , format_(format)
        , deviceType_(deviceType)
        , width_(width)
        , height_(height)
    {
    }

    qtav::HardwareDeviceType deviceType() const noexcept override
    {
        return deviceType_;
    }

    int width() const noexcept override
    {
        return width_;
    }

    int height() const noexcept override
    {
        return height_;
    }

    qtav::PixelFormat softwareFormat() const noexcept override
    {
        return format_;
    }

    qtav::NativeHandle nativeHandle(
        qtav::HardwareHandleType type) const noexcept override
    {
        return type == qtav::HardwareHandleType::Texture
            ? qtav::NativeHandle {
                  type,
                  reinterpret_cast<std::uintptr_t>(texture_.Get()),
                  slice_,
              }
            : qtav::NativeHandle { type, 0, 0 };
    }

    bool isMappable(qtav::HardwareMapMode) const noexcept override
    {
        return false;
    }

    std::shared_ptr<qtav::HardwareFrameMapping> map(
        qtav::HardwareMapMode) const override
    {
        return {};
    }

private:
    ComPtr<ID3D11Texture2D> texture_;
    std::uint32_t slice_ = 0;
    qtav::PixelFormat format_ = qtav::PixelFormat::Unknown;
    qtav::HardwareDeviceType deviceType_ =
        qtav::HardwareDeviceType::Unknown;
    int width_ = 0;
    int height_ = 0;
};

ComPtr<ID3D11Texture2D> makeTexture(
    ID3D11Device* device,
    DXGI_FORMAT format,
    UINT arraySize = 2)
{
    D3D11_TEXTURE2D_DESC description {};
    description.Width = 64;
    description.Height = 32;
    description.MipLevels = 1;
    description.ArraySize = arraySize;
    description.Format = format;
    description.SampleDesc.Count = 1;
    description.Usage = D3D11_USAGE_DEFAULT;

    ComPtr<ID3D11Texture2D> texture;
    if (FAILED(device->CreateTexture2D(
            &description,
            nullptr,
            &texture))) {
        std::abort();
    }
    return texture;
}

void testDeviceConfig(
    const std::shared_ptr<qtav::D3D11DeviceAccess>& access)
{
    auto config = qtav::d3d11vaHardwareDecodeConfig(
        access,
        { false, 7 });
    assert(config.isValid());
    assert(config.deviceType == qtav::HardwareDeviceType::D3D11);
    assert(!config.allowSoftwareFallback);
    assert(config.extraHardwareFrames == 7);
    assert(config.requireSuppliedDevice);

    const auto low = qtav::d3d11vaHardwareDecodeConfig(
        access,
        { true, -5 });
    assert(low.extraHardwareFrames == 0);
    const auto high = qtav::d3d11vaHardwareDecodeConfig(
        access,
        { true, 1000 });
    assert(high.extraHardwareFrames == 64);

    const auto missing =
        qtav::d3d11vaHardwareDecodeConfig(nullptr);
    assert(missing.isValid());
    assert(!missing.device);
    assert(missing.requireSuppliedDevice);

    if (!config.device) {
        std::cout
            << "D3D11 video interfaces are unavailable; "
               "selected-device initialization checks skipped\n";
        return;
    }
    assert(
        config.device.nativeIdentity()
        == reinterpret_cast<std::uintptr_t>(
            access->device().get()));
    AVBufferRef* reference =
        qtav::detail::HardwareDecodeDevicePrivate::contextRef(
            config.device);
    assert(reference);
    auto* context =
        reinterpret_cast<AVHWDeviceContext*>(reference->data);
    assert(context->type == AV_HWDEVICE_TYPE_D3D11VA);
    auto* native =
        static_cast<AVD3D11VADeviceContext*>(context->hwctx);
    assert(native);
    assert(native->device == access->device().get());
    assert(
        native->device_context
        == access->immediateContext().get());
    assert(native->lock);
    assert(native->unlock);
    assert(native->lock_ctx);
    assert(!(native->BindFlags & D3D11_BIND_SHADER_RESOURCE));

    std::mutex mutex;
    std::condition_variable changed;
    bool started = false;
    std::atomic<bool> acquired { false };
    std::thread worker;
    {
        auto guard = access->contextGuard();
        worker = std::thread([&] {
            {
                std::lock_guard<std::mutex> lock(mutex);
                started = true;
                changed.notify_all();
            }
            native->lock(native->lock_ctx);
            acquired.store(true);
            native->unlock(native->lock_ctx);
        });
        std::unique_lock<std::mutex> lock(mutex);
        assert(changed.wait_for(
            lock,
            std::chrono::seconds(1),
            [&] { return started; }));
        std::this_thread::sleep_for(
            std::chrono::milliseconds(20));
        assert(!acquired.load());
    }
    worker.join();
    assert(acquired.load());
    av_buffer_unref(&reference);

    qtav::D3D11VAHardwareDecodeOptions directOptions;
    directOptions.directDecoderTextureSampling = true;
    const auto direct = qtav::d3d11vaHardwareDecodeConfig(
        access,
        directOptions);
    assert(direct.device);
    reference = qtav::detail::HardwareDecodeDevicePrivate::contextRef(
        direct.device);
    assert(reference);
    context = reinterpret_cast<AVHWDeviceContext*>(reference->data);
    native = static_cast<AVD3D11VADeviceContext*>(context->hwctx);
    assert(native);
    assert(native->BindFlags & D3D11_BIND_SHADER_RESOURCE);
    av_buffer_unref(&reference);
}

void testRetainedTextureFrame(ID3D11Device* device)
{
    const auto texture = makeTexture(
        device,
        DXGI_FORMAT_NV12);
    qtav::HardwareFrame hardware(
        std::make_shared<TextureFrameData>(
            texture,
            1,
            qtav::PixelFormat::NV12));
    auto frame = qtav::d3d11vaFrame(hardware);
    assert(frame);
    assert(frame.texture() == texture.Get());
    assert(frame.arraySlice() == 1);
    assert(frame.device() == device);
    assert(frame.width() == 64);
    assert(frame.height() == 32);
    assert(frame.softwareFormat() == qtav::PixelFormat::NV12);
    assert(frame.sourceFrame().nativeHandle(
        qtav::HardwareHandleType::Texture).subresource == 1);

    hardware = {};
    assert(frame);
    assert(frame.texture() == texture.Get());

    assert(!qtav::d3d11vaFrame(qtav::HardwareFrame {}));
    assert(!qtav::d3d11vaFrame(qtav::HardwareFrame(
        std::make_shared<TextureFrameData>(
            nullptr,
            0,
            qtav::PixelFormat::NV12))));
    assert(!qtav::d3d11vaFrame(qtav::HardwareFrame(
        std::make_shared<TextureFrameData>(
            texture,
            2,
            qtav::PixelFormat::NV12))));
    assert(!qtav::d3d11vaFrame(qtav::HardwareFrame(
        std::make_shared<TextureFrameData>(
            texture,
            0,
            qtav::PixelFormat::BGRA))));
    assert(!qtav::d3d11vaFrame(qtav::HardwareFrame(
        std::make_shared<TextureFrameData>(
            texture,
            0,
            qtav::PixelFormat::NV12,
            qtav::HardwareDeviceType::MediaCodec))));
    assert(!qtav::d3d11vaFrame(qtav::HardwareFrame(
        std::make_shared<TextureFrameData>(
            texture,
            0,
            qtav::PixelFormat::NV12,
            qtav::HardwareDeviceType::D3D11,
            65,
            32))));

    const auto wrongTexture = makeTexture(
        device,
        DXGI_FORMAT_B8G8R8A8_UNORM);
    assert(!qtav::d3d11vaFrame(qtav::HardwareFrame(
        std::make_shared<TextureFrameData>(
            wrongTexture,
            0,
            qtav::PixelFormat::NV12))));
}

void testPlayerLifecycle(
    const char* media,
    const std::shared_ptr<qtav::D3D11DeviceAccess>& access)
{
    auto config = qtav::d3d11vaHardwareDecodeConfig(access);
    if (!config.device) {
        std::cout
            << "D3D11VA selected-device creation unavailable; "
               "native decode test skipped\n";
        return;
    }

    qtav::HardwareFrame retainedFrame;
    std::atomic<int> videoFrames { 0 };
    std::atomic<int> hardwareFrames { 0 };
    std::atomic<bool> softwareFallback { false };
    std::mutex mutex;
    std::condition_variable changed;
    int stage = 0;
    bool stopped = false;
    bool failed = false;

    {
        qtav::Player player;
        player
            .setHardwareDecodeConfig(config)
            .onEvent([&](const qtav::MediaEvent& event) {
                if (event.category == "decoder.hardware.fallback") {
                    softwareFallback.store(true);
                }
                return false;
            })
            .onMediaStatus(
                [&](qtav::MediaStatus, qtav::MediaStatus status) {
                    if (status == qtav::MediaStatus::Invalid) {
                        std::lock_guard<std::mutex> lock(mutex);
                        failed = true;
                        changed.notify_all();
                    }
                    return false;
                })
            .onStateChanged([&](qtav::State state) {
                std::lock_guard<std::mutex> lock(mutex);
                if (state == qtav::State::Stopped && stage == 4) {
                    stopped = true;
                    changed.notify_all();
                }
            })
            .onVideoFrame([&](const qtav::VideoFrame& frame, int) {
                ++videoFrames;
                if (frame.hasHardwareFrame()) {
                    const auto hardware = frame.hardwareFrame();
                    const auto native = qtav::d3d11vaFrame(hardware);
                    if (!native
                        || native.device() != access->device().get()
                        || native.width() != frame.width()
                        || native.height() != frame.height()) {
                        std::abort();
                    }
                    D3D11_TEXTURE2D_DESC description {};
                    native.texture()->GetDesc(&description);
                    if (description.BindFlags
                        & D3D11_BIND_SHADER_RESOURCE) {
                        std::abort();
                    }

                    if (hardwareFrames.fetch_add(1) == 0) {
                        const auto mapping = hardware.map();
                        if (!mapping || !mapping->data(0)
                            || mapping->width() != frame.width()
                            || mapping->height() != frame.height()) {
                            std::abort();
                        }
                        std::lock_guard<std::mutex> lock(mutex);
                        retainedFrame = hardware;
                    }
                }

                int action = 0;
                {
                    std::lock_guard<std::mutex> lock(mutex);
                    if (stage == 0) {
                        stage = 1;
                        action = 1;
                    } else if (stage == 2) {
                        stage = 3;
                        action = 2;
                    } else if (stage == 3) {
                        stage = 4;
                        action = 3;
                    }
                }

                if (action == 1) {
                    if (!player.seek(
                            500,
                            qtav::SeekFlag::FromStart,
                            [&](std::int64_t position) {
                                if (position != 500) {
                                    std::abort();
                                }
                                std::lock_guard<std::mutex> lock(mutex);
                                if (stage != 1) {
                                    std::abort();
                                }
                                stage = 2;
                            })) {
                        std::abort();
                    }
                } else if (action == 2) {
                    player.setMedia(media);
                } else if (action == 3) {
                    player.setState(qtav::State::Stopped);
                }
            });

        player.setPlaybackRate(8.0F);
        player.setMedia(media);
        player.setState(qtav::State::Playing);

        std::unique_lock<std::mutex> lock(mutex);
        if (!changed.wait_for(
                lock,
                std::chrono::seconds(10),
                [&] { return stopped || failed; })
            || failed || !stopped) {
            std::abort();
        }
    }

    if (videoFrames.load() < 3) {
        std::abort();
    }
    if (!softwareFallback.load() && hardwareFrames.load() == 0) {
        std::abort();
    }
    if (softwareFallback.load()) {
        std::cout
            << "D3D11VA codec path unavailable; "
               "software fallback lifecycle exercised\n";
    } else {
        std::cout
            << "D3D11VA native frames: "
            << hardwareFrames.load() << '\n';
    }
    if (retainedFrame) {
        const auto native = qtav::d3d11vaFrame(retainedFrame);
        const auto mapping = retainedFrame.map();
        if (!native || !mapping || !mapping->data(0)
            || native.device() != access->device().get()) {
            std::abort();
        }
    }
}

} // namespace

int main(int argc, char** argv)
{
    if (argc != 1 && argc != 2) {
        return 2;
    }
    const auto d3d = makeDevice();
    const auto access = qtav::D3D11DeviceAccess::create(
        qtav::BorrowedD3D11Device(d3d.device.Get()),
        qtav::BorrowedD3D11DeviceContext(d3d.context.Get()));
    assert(access);

    testDeviceConfig(access);
    const auto warp = makeWarpDevice();
    testRetainedTextureFrame(warp.device.Get());
    if (argc == 2) {
        testPlayerLifecycle(argv[1], access);
    }
    return 0;
}
