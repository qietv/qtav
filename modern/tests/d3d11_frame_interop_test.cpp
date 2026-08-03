// SPDX-License-Identifier: LGPL-2.1-or-later

#if defined(NDEBUG)
#  undef NDEBUG
#endif

#if !defined(NOMINMAX)
#  define NOMINMAX
#endif

#include <d3d11.h>
#include <wrl/client.h>

#include <qtav/d3d11_frame_interop.h>
#include <qtav/d3d11va_hardware_decoder.h>
#include <qtav/player.h>

#include <algorithm>
#include <atomic>
#include <cassert>
#include <chrono>
#include <cmath>
#include <condition_variable>
#include <cstddef>
#include <cstdint>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <future>
#include <iostream>
#include <iterator>
#include <memory>
#include <mutex>
#include <string_view>
#include <thread>
#include <vector>

namespace {

using Microsoft::WRL::ComPtr;

struct Pixel {
    std::uint8_t blue = 0;
    std::uint8_t green = 0;
    std::uint8_t red = 0;
    std::uint8_t alpha = 0;
};

struct HalfPixel {
    std::uint16_t red = 0;
    std::uint16_t green = 0;
    std::uint16_t blue = 0;
    std::uint16_t alpha = 0;
};

struct DeviceResources {
    ComPtr<ID3D11Device> device;
    ComPtr<ID3D11DeviceContext> context;
};

struct RenderTarget {
    ComPtr<ID3D11Texture2D> texture;
    ComPtr<ID3D11RenderTargetView> view;
};

DeviceResources makeDevice(D3D_DRIVER_TYPE type)
{
    DeviceResources result;
    const D3D_FEATURE_LEVEL levels[] {
        D3D_FEATURE_LEVEL_11_1,
        D3D_FEATURE_LEVEL_11_0,
    };
    D3D_FEATURE_LEVEL selected {};
    HRESULT status = D3D11CreateDevice(
        nullptr,
        type,
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
            type,
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
        return {};
    }
    return result;
}

std::shared_ptr<qtav::D3D11DeviceAccess> makeAccess(
    const DeviceResources& resources)
{
    return qtav::D3D11DeviceAccess::create(
        qtav::BorrowedD3D11Device(resources.device.Get()),
        qtav::BorrowedD3D11DeviceContext(
            resources.context.Get()));
}

bool supportsHevcMain10(ID3D11Device* device)
{
    ComPtr<ID3D11VideoDevice> videoDevice;
    if (!device
        || FAILED(device->QueryInterface(
            IID_PPV_ARGS(&videoDevice)))) {
        return false;
    }

    bool profileFound = false;
    const UINT profileCount =
        videoDevice->GetVideoDecoderProfileCount();
    for (UINT index = 0; index < profileCount; ++index) {
        GUID profile {};
        if (SUCCEEDED(videoDevice->GetVideoDecoderProfile(
                index,
                &profile))
            && IsEqualGUID(
                profile,
                D3D11_DECODER_PROFILE_HEVC_VLD_MAIN10)) {
            profileFound = true;
            break;
        }
    }
    if (!profileFound) {
        return false;
    }

    BOOL p010Supported = FALSE;
    return SUCCEEDED(videoDevice->CheckVideoDecoderFormat(
               &D3D11_DECODER_PROFILE_HEVC_VLD_MAIN10,
               DXGI_FORMAT_P010,
               &p010Supported))
        && p010Supported;
}

ComPtr<ID3D11Texture2D> makeDecoderTexture(
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
    description.BindFlags = D3D11_BIND_SHADER_RESOURCE;

    ComPtr<ID3D11Texture2D> texture;
    device->CreateTexture2D(
        &description,
        nullptr,
        &texture);
    return texture;
}

class TextureFrameData final : public qtav::HardwareFrameData {
public:
    TextureFrameData(
        ComPtr<ID3D11Texture2D> texture,
        std::uint32_t slice,
        qtav::PixelFormat format,
        int* mapCalls = nullptr)
        : texture_(std::move(texture))
        , slice_(slice)
        , format_(format)
        , mapCalls_(mapCalls)
    {
    }

    qtav::HardwareDeviceType deviceType() const noexcept override
    {
        return qtav::HardwareDeviceType::D3D11;
    }

    int width() const noexcept override
    {
        return 64;
    }

    int height() const noexcept override
    {
        return 32;
    }

    qtav::PixelFormat softwareFormat() const noexcept override
    {
        return format_;
    }

    qtav::NativeHandle nativeHandle(
        qtav::HardwareHandleType type) const noexcept override
    {
        if (type != qtav::HardwareHandleType::Texture) {
            return { type, 0, 0 };
        }
        return {
            type,
            reinterpret_cast<std::uintptr_t>(texture_.Get()),
            slice_,
        };
    }

    bool isMappable(qtav::HardwareMapMode) const noexcept override
    {
        return true;
    }

    std::shared_ptr<qtav::HardwareFrameMapping> map(
        qtav::HardwareMapMode) const override
    {
        if (mapCalls_) {
            ++*mapCalls_;
        }
        return {};
    }

private:
    ComPtr<ID3D11Texture2D> texture_;
    std::uint32_t slice_ = 0;
    qtav::PixelFormat format_ = qtav::PixelFormat::Unknown;
    int* mapCalls_ = nullptr;
};

qtav::HardwareFrame makeHardwareFrame(
    ComPtr<ID3D11Texture2D> texture,
    std::uint32_t slice,
    qtav::PixelFormat format,
    int* mapCalls = nullptr)
{
    return qtav::HardwareFrame(
        std::make_shared<TextureFrameData>(
            std::move(texture),
            slice,
            format,
            mapCalls));
}

void testWarpContracts()
{
    const auto warp = makeDevice(D3D_DRIVER_TYPE_WARP);
    assert(warp.device && warp.context);
    const auto access = makeAccess(warp);
    assert(access);

    qtav::D3D11FrameInterop interop(access);
    assert(interop.deviceAccess() == access);
    assert(!interop.supports({}));
    assert(!interop.importFrame({}));

    auto texture = makeDecoderTexture(
        warp.device.Get(),
        DXGI_FORMAT_NV12);
    if (!texture) {
        std::cout
            << "WARP cannot allocate an NV12 texture; "
               "texture-source contracts skipped\n";
        return;
    }

    int mapCalls = 0;
    auto frame = makeHardwareFrame(
        texture,
        1,
        qtav::PixelFormat::NV12,
        &mapCalls);
    const auto native = qtav::d3d11vaFrame(frame);
    assert(native);
    assert(native.device() == warp.device.Get());
    assert(native.texture() == texture.Get());
    assert(native.arraySlice() == 1);
    assert(native.softwareFormat() == qtav::PixelFormat::NV12);
    assert(!qtav::d3d11vaFrame(makeHardwareFrame(
        texture,
        2,
        qtav::PixelFormat::NV12)));
    assert(!qtav::d3d11vaFrame(makeHardwareFrame(
        texture,
        0,
        qtav::PixelFormat::BGRA)));

    auto retainedFrame = frame;
    texture.Reset();
    assert(qtav::d3d11vaFrame(retainedFrame).texture());

    std::future<bool> lockPending;
    {
        auto guard = access->contextGuard();
        lockPending = std::async(
            std::launch::async,
            [&] {
                auto nestedGuard = access->contextGuard();
                (void)nestedGuard;
                return true;
            });
        assert(
            lockPending.wait_for(std::chrono::milliseconds(50))
            == std::future_status::timeout);
    }
    assert(lockPending.get());

    const auto capabilities = interop.capabilities();
    assert(capabilities.sourceDevices.size() == 1);
    assert(
        capabilities.sourceDevices.front()
        == qtav::HardwareDeviceType::D3D11);
    assert(
        capabilities.targetDevice
        == qtav::HardwareDeviceType::D3D11);
    assert(capabilities.zeroCopy);
    assert(!capabilities.cpuFallback);

    texture = makeDecoderTexture(
        warp.device.Get(),
        DXGI_FORMAT_NV12);
    if (!texture) {
        std::cout
            << "WARP cannot allocate an NV12 texture; "
               "texture import contracts skipped\n";
        return;
    }

    mapCalls = 0;
    frame = makeHardwareFrame(
        texture,
        1,
        qtav::PixelFormat::NV12,
        &mapCalls);
    assert(interop.supports(frame));

    const auto foreignResources =
        makeDevice(D3D_DRIVER_TYPE_WARP);
    assert(foreignResources.device);
    auto foreignTexture = makeDecoderTexture(
        foreignResources.device.Get(),
        DXGI_FORMAT_NV12);
    assert(foreignTexture);
    assert(!interop.supports(makeHardwareFrame(
        foreignTexture,
        0,
        qtav::PixelFormat::NV12)));
    assert(!interop.supports(makeHardwareFrame(
        texture,
        2,
        qtav::PixelFormat::NV12)));
    assert(!interop.supports(makeHardwareFrame(
        texture,
        0,
        qtav::PixelFormat::BGRA)));

    std::future<std::shared_ptr<qtav::D3D11TextureFrame>> pending;
    {
        auto guard = access->contextGuard();
        pending = std::async(
            std::launch::async,
            [&] { return interop.importFrame(frame); });
        assert(
            pending.wait_for(std::chrono::milliseconds(50))
            == std::future_status::ready);
    }
    auto imported = pending.get();
    assert(mapCalls == 0);
    assert(imported);
    assert(imported->width() == 64);
    assert(imported->height() == 32);
    assert(imported->format() == qtav::PixelFormat::NV12);
    assert(imported->dxgiFormat() == DXGI_FORMAT_NV12);
    assert(imported->arraySlice() == 1);
    assert(
        imported->colorSpace()
        == DXGI_COLOR_SPACE_YCBCR_STUDIO_G22_LEFT_P709);
    assert(imported->texture() == texture.Get());
    assert(!imported->shaderResourceView());
    ComPtr<ID3D11Device> importedDevice;
    imported->texture()->GetDevice(&importedDevice);
    assert(importedDevice.Get() == warp.device.Get());

    frame = {};
    texture.Reset();
    assert(imported->texture());
    assert(!imported->shaderResourceView());
    interop.flush();
    assert(imported->texture());
}

RenderTarget makeTarget(
    ID3D11Device* device,
    int width,
    int height,
    DXGI_FORMAT format = DXGI_FORMAT_B8G8R8A8_UNORM)
{
    D3D11_TEXTURE2D_DESC description {};
    description.Width = static_cast<UINT>(width);
    description.Height = static_cast<UINT>(height);
    description.MipLevels = 1;
    description.ArraySize = 1;
    description.Format = format;
    description.SampleDesc.Count = 1;
    description.Usage = D3D11_USAGE_DEFAULT;
    description.BindFlags =
        D3D11_BIND_RENDER_TARGET
        | D3D11_BIND_SHADER_RESOURCE;

    RenderTarget result;
    if (FAILED(device->CreateTexture2D(
            &description,
            nullptr,
            &result.texture))
        || FAILED(device->CreateRenderTargetView(
            result.texture.Get(),
            nullptr,
            &result.view))) {
        return {};
    }
    return result;
}

std::vector<Pixel> readTarget(
    ID3D11Device* device,
    ID3D11DeviceContext* context,
    ID3D11Texture2D* texture)
{
    D3D11_TEXTURE2D_DESC source {};
    texture->GetDesc(&source);
    D3D11_TEXTURE2D_DESC staging = source;
    staging.Usage = D3D11_USAGE_STAGING;
    staging.BindFlags = 0;
    staging.CPUAccessFlags = D3D11_CPU_ACCESS_READ;

    ComPtr<ID3D11Texture2D> readback;
    assert(SUCCEEDED(device->CreateTexture2D(
        &staging,
        nullptr,
        &readback)));
    context->CopyResource(readback.Get(), texture);

    D3D11_MAPPED_SUBRESOURCE mapped {};
    assert(SUCCEEDED(context->Map(
        readback.Get(),
        0,
        D3D11_MAP_READ,
        0,
        &mapped)));
    std::vector<Pixel> result(
        static_cast<std::size_t>(source.Width)
        * static_cast<std::size_t>(source.Height));
    for (UINT row = 0; row < source.Height; ++row) {
        const auto* input = reinterpret_cast<const Pixel*>(
            static_cast<const std::uint8_t*>(mapped.pData)
            + static_cast<std::size_t>(row) * mapped.RowPitch);
        std::copy(
            input,
            input + source.Width,
            result.begin()
                + static_cast<std::ptrdiff_t>(row * source.Width));
    }
    context->Unmap(readback.Get(), 0);
    return result;
}

std::vector<HalfPixel> readHalfTarget(
    ID3D11Device* device,
    ID3D11DeviceContext* context,
    ID3D11Texture2D* texture)
{
    D3D11_TEXTURE2D_DESC source {};
    texture->GetDesc(&source);
    D3D11_TEXTURE2D_DESC staging = source;
    staging.Usage = D3D11_USAGE_STAGING;
    staging.BindFlags = 0;
    staging.CPUAccessFlags = D3D11_CPU_ACCESS_READ;

    ComPtr<ID3D11Texture2D> readback;
    assert(SUCCEEDED(device->CreateTexture2D(
        &staging,
        nullptr,
        &readback)));
    context->CopyResource(readback.Get(), texture);

    D3D11_MAPPED_SUBRESOURCE mapped {};
    assert(SUCCEEDED(context->Map(
        readback.Get(),
        0,
        D3D11_MAP_READ,
        0,
        &mapped)));
    std::vector<HalfPixel> result(
        static_cast<std::size_t>(source.Width)
        * static_cast<std::size_t>(source.Height));
    for (UINT row = 0; row < source.Height; ++row) {
        std::memcpy(
            result.data()
                + static_cast<std::size_t>(row) * source.Width,
            static_cast<const std::uint8_t*>(mapped.pData)
                + static_cast<std::size_t>(row) * mapped.RowPitch,
            static_cast<std::size_t>(source.Width)
                * sizeof(HalfPixel));
    }
    context->Unmap(readback.Get(), 0);
    return result;
}

float halfToFloat(std::uint16_t value)
{
    const std::uint32_t sign =
        static_cast<std::uint32_t>(value & 0x8000U) << 16U;
    int exponent = static_cast<int>((value >> 10U) & 0x1fU);
    std::uint32_t mantissa = value & 0x03ffU;
    std::uint32_t bits = 0;
    if (exponent == 0) {
        if (mantissa == 0) {
            bits = sign;
        } else {
            exponent = 1;
            while ((mantissa & 0x0400U) == 0) {
                mantissa <<= 1U;
                --exponent;
            }
            mantissa &= 0x03ffU;
            bits = sign
                | (static_cast<std::uint32_t>(exponent + 112) << 23U)
                | (mantissa << 13U);
        }
    } else if (exponent == 31) {
        bits = sign | 0x7f800000U | (mantissa << 13U);
    } else {
        bits = sign
            | (static_cast<std::uint32_t>(exponent + 112) << 23U)
            | (mantissa << 13U);
    }
    float result = 0.0F;
    std::memcpy(&result, &bits, sizeof(result));
    return result;
}

void testNativeZeroCopy(
    const char* media,
    qtav::PixelFormat expectedFormat,
    bool exerciseLifecycle,
    bool requireDolbyVision = false,
    int stopAfterRenderedFrames = 0)
{
    const auto resources = makeDevice(D3D_DRIVER_TYPE_HARDWARE);
    if (!resources.device || !resources.context) {
        std::cout
            << "No hardware D3D11 adapter; native zero-copy test skipped\n";
        return;
    }
    if (expectedFormat == qtav::PixelFormat::P010
        && !supportsHevcMain10(resources.device.Get())) {
        std::cout
            << "Adapter exposes no HEVC Main10/P010 D3D11VA "
               "profile; Main10 zero-copy test skipped\n";
        return;
    }
    const auto access = makeAccess(resources);
    assert(access);
    const auto decodeConfig =
        qtav::d3d11vaHardwareDecodeConfig(access);
    if (!decodeConfig.device) {
        std::cout
            << "D3D11VA selected-device creation unavailable; "
               "native zero-copy test skipped\n";
        return;
    }

    constexpr int width = 160;
    constexpr int height = 90;
    auto target = makeTarget(
        resources.device.Get(),
        width,
        height);
    assert(target.texture && target.view);
    auto interop =
        std::make_shared<qtav::D3D11FrameInterop>(access);
    assert(!interop->capabilities().sourceDevices.empty());
    const DXGI_FORMAT nativeFormat =
        expectedFormat == qtav::PixelFormat::P010
        ? DXGI_FORMAT_P010
        : DXGI_FORMAT_NV12;
    auto contractTexture = makeDecoderTexture(
        resources.device.Get(),
        nativeFormat);
    assert(contractTexture);
    assert(interop->supports(makeHardwareFrame(
        contractTexture,
        0,
        expectedFormat)));
    assert(!interop->supports(makeHardwareFrame(
        contractTexture,
        2,
        expectedFormat)));
    const auto foreignResources =
        makeDevice(D3D_DRIVER_TYPE_HARDWARE);
    auto foreignTexture = makeDecoderTexture(
        foreignResources.device.Get(),
        nativeFormat);
    assert(foreignTexture);
    assert(!interop->supports(makeHardwareFrame(
        foreignTexture,
        0,
        expectedFormat)));

    auto renderer =
        std::make_shared<qtav::D3D11VideoRenderer>(
            access,
            [&] {
                return qtav::D3D11RenderTarget {
                    target.view.Get(),
                };
            });
    renderer->setHardwareFrameInterop(interop);
    qtav::VideoRenderConfig renderConfig;
    renderConfig.surfaceSize = { width, height };
    renderConfig.aspectRatio =
        qtav::VideoAspectRatioMode::Stretch;
    assert(renderer->open(renderConfig));

    std::atomic<bool> softwareFallback { false };
    std::atomic<bool> renderError { false };
    std::atomic<int> hardwareFrames { 0 };
    std::atomic<int> colorAwareImports { 0 };
    std::atomic<int> dolbyVisionFrames { 0 };
    std::atomic<int> renderRequests { 0 };
    std::atomic<bool> stopRequested { false };
    std::mutex mutex;
    std::condition_variable changed;
    int stage = 0;
    bool paused = false;
    bool stopped = false;
    bool failed = false;
    qtav::VideoFrame retainedFrame;
    std::shared_ptr<qtav::D3D11TextureFrame> retainedImport;
    renderer->setEventCallback(
        [&](const qtav::VideoRenderEvent& event) {
            if (event.type == qtav::VideoRenderEventType::Error
                || event.type
                    == qtav::VideoRenderEventType::SurfaceLost) {
                renderError.store(true);
                std::cerr << "D3D11 render event: "
                          << event.detail << '\n';
            }
        });

    {
        qtav::Player player;
        player
            .setHardwareDecodeConfig(decodeConfig)
            .setVideoRenderAPI(renderer)
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
                if (exerciseLifecycle
                    && state == qtav::State::Paused
                    && stage == 1) {
                    stage = 2;
                    paused = true;
                    changed.notify_all();
                }
                if (state == qtav::State::Stopped
                    && (!exerciseLifecycle || stage == 6)) {
                    stopped = true;
                    changed.notify_all();
                }
            })
            .onVideoFrame([&](const qtav::VideoFrame& frame, int) {
                if (!frame.hasHardwareFrame()) {
                    return;
                }
                if (frame.hasDolbyVisionMetadata()) {
                    ++dolbyVisionFrames;
                }
                const auto native =
                    qtav::d3d11vaFrame(frame.hardwareFrame());
                if (!native
                    || native.device() != resources.device.Get()
                    || native.softwareFormat() != expectedFormat) {
                    std::abort();
                }
                D3D11_TEXTURE2D_DESC decoderDescription {};
                native.texture()->GetDesc(&decoderDescription);
                if (!(decoderDescription.BindFlags
                      & D3D11_BIND_SHADER_RESOURCE)) {
                    std::abort();
                }
                const bool retainThisFrame = requireDolbyVision
                    ? frame.hasDolbyVisionMetadata() && !retainedFrame
                    : hardwareFrames.load() == 0;
                hardwareFrames.fetch_add(1);
                if (retainThisFrame) {
                    const qtav::VideoColorSpace color =
                        frame.colorSpaceInfo();
                    if (expectedFormat == qtav::PixelFormat::P010
                        && !requireDolbyVision) {
                        assert(
                            color.range
                            == qtav::ColorRange::Limited);
                        assert(
                            color.primaries
                            == qtav::ColorPrimaries::BT2020);
                        assert(
                            color.transfer
                            == qtav::ColorTransfer::PQ);
                        assert(
                            color.matrix
                            == qtav::ColorMatrix::BT2020NCL);
                    }
                    retainedImport = interop->importFrame(
                        frame.hardwareFrame(),
                        color);
                    if (!retainedImport) {
                        std::abort();
                    }
                    retainedFrame = frame;
                    ++colorAwareImports;
                }
            })
            .setRenderCallback([&](void*) {
                if (player.renderVideo() < 0.0) {
                    return;
                }
                const int rendered = ++renderRequests;
                if (stopAfterRenderedFrames > 0
                    && rendered >= stopAfterRenderedFrames
                    && !stopRequested.exchange(true)) {
                    player.setState(qtav::State::Stopped);
                    return;
                }
                if (!exerciseLifecycle) {
                    return;
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
                    } else if (stage == 4) {
                        stage = 5;
                        action = 3;
                    } else if (stage == 5) {
                        stage = 6;
                        action = 4;
                    }
                }

                if (action == 1) {
                    player.setState(qtav::State::Paused);
                } else if (action == 2) {
                    if (!player.seek(
                            500,
                            qtav::SeekFlag::FromStart,
                            [&](std::int64_t position) {
                                if (position != 500) {
                                    std::abort();
                                }
                                std::lock_guard<std::mutex> lock(
                                    mutex);
                                if (stage != 3) {
                                    std::abort();
                                }
                                stage = 4;
                            })) {
                        std::abort();
                    }
                } else if (action == 3) {
                    player.setMedia(media);
                } else if (action == 4) {
                    player.setState(qtav::State::Stopped);
                }
            });
        player.setPlaybackRate(exerciseLifecycle ? 8.0F : 1.0F);
        player.setMedia(media);
        player.setState(qtav::State::Playing);

        if (exerciseLifecycle) {
            {
                std::unique_lock<std::mutex> lock(mutex);
                if (!changed.wait_for(
                        lock,
                        std::chrono::seconds(10),
                        [&] { return paused || failed; })
                    || failed || !paused) {
                    std::abort();
                }
            }
            player.setState(qtav::State::Playing);
            std::unique_lock<std::mutex> lock(mutex);
            if (!changed.wait_for(
                    lock,
                    std::chrono::seconds(15),
                    [&] { return stopped || failed; })
                || failed || !stopped) {
                std::abort();
            }
        } else {
            if (!player.waitFor(qtav::State::Playing, 10'000)
                && player.mediaStatus()
                    == qtav::MediaStatus::Invalid) {
                std::abort();
            }
            if (!player.waitFor(
                    qtav::State::Stopped,
                    stopAfterRenderedFrames > 0 ? 60'000 : 15'000)) {
                std::abort();
            }
        }
    }

    if (softwareFallback.load()) {
        if (expectedFormat == qtav::PixelFormat::P010) {
            std::abort();
        }
        std::cout
            << "Adapter/codec D3D11VA path unavailable; "
               "native zero-copy test skipped\n";
        return;
    }
    if (hardwareFrames.load() == 0) {
        std::cerr
            << "Expected "
            << (expectedFormat == qtav::PixelFormat::P010
                    ? "HEVC Main10/P010"
                    : "H.264/NV12")
            << " hardware frames, but received none; render requests: "
            << renderRequests.load() << '\n';
    }
    assert(hardwareFrames.load() > 0);
    assert(colorAwareImports.load() == 1);
    assert(renderRequests.load() > 0);
    assert(!renderError.load());
    if (requireDolbyVision) {
        assert(dolbyVisionFrames.load() > 0);
    }
    assert(retainedFrame);
    assert(retainedImport);
    assert(retainedImport->texture());
    assert(!retainedImport->shaderResourceView());
    assert(retainedImport->format() == expectedFormat);
    assert(retainedImport->dxgiFormat() == nativeFormat);

    interop->flush();
    assert(retainedImport->texture());
    assert(!retainedImport->shaderResourceView());

    target = makeTarget(
        resources.device.Get(),
        width,
        height,
        expectedFormat == qtav::PixelFormat::P010
            ? DXGI_FORMAT_R16G16B16A16_FLOAT
            : DXGI_FORMAT_B8G8R8A8_UNORM);
    assert(target.texture && target.view);
    assert(renderer->configure(renderConfig));
    const auto renderDeadline =
        std::chrono::steady_clock::now()
        + std::chrono::seconds(2);
    bool retainedFrameRendered = false;
    while (!(retainedFrameRendered =
                 renderer->render(retainedFrame))
           && std::chrono::steady_clock::now()
               < renderDeadline) {
        std::this_thread::yield();
    }
    assert(retainedFrameRendered);

    auto guard = access->contextGuard();
    if (expectedFormat == qtav::PixelFormat::P010) {
        const auto pixels = readHalfTarget(
            resources.device.Get(),
            resources.context.Get(),
            target.texture.Get());
        (void)guard;
        double leftRed = 0.0;
        double leftBlue = 0.0;
        double rightRed = 0.0;
        double rightBlue = 0.0;
        float maximumComponent = 0.0F;
        for (int y = 0; y < height; ++y) {
            for (int x = 0; x < width; ++x) {
                const auto& pixel = pixels[
                    static_cast<std::size_t>(y * width + x)];
                const float red = halfToFloat(pixel.red);
                const float blue = halfToFloat(pixel.blue);
                maximumComponent = std::max(
                    maximumComponent,
                    std::max(red, blue));
                if (x < width / 2) {
                    leftRed += red;
                    leftBlue += blue;
                } else {
                    rightRed += red;
                    rightBlue += blue;
                }
            }
        }
        if (requireDolbyVision) {
            assert(std::isfinite(maximumComponent));
            assert(maximumComponent > 0.0F);
        } else {
            assert(leftRed > leftBlue * 2.0);
            assert(rightBlue > rightRed * 2.0);
            assert(maximumComponent > 1.0F);
        }
        const auto output = renderer->advancedColorInfo();
        assert(
            output.outputColorSpace
            == qtav::D3D11OutputColorSpace::ScRGB);
    } else {
        const auto pixels = readTarget(
            resources.device.Get(),
            resources.context.Get(),
            target.texture.Get());
        (void)guard;
        std::uint64_t leftRed = 0;
        std::uint64_t leftBlue = 0;
        std::uint64_t rightRed = 0;
        std::uint64_t rightBlue = 0;
        for (int y = 0; y < height; ++y) {
            for (int x = 0; x < width; ++x) {
                const auto& pixel = pixels[
                    static_cast<std::size_t>(y * width + x)];
                if (x < width / 2) {
                    leftRed += pixel.red;
                    leftBlue += pixel.blue;
                } else {
                    rightRed += pixel.red;
                    rightBlue += pixel.blue;
                }
            }
        }
        assert(leftRed > leftBlue * 2);
        assert(rightBlue > rightRed * 2);
    }
    std::cout
        << (expectedFormat == qtav::PixelFormat::P010
                ? "HEVC Main10/P010"
                : "H.264/NV12")
        << " zero-CPU-map frames rendered: "
        << hardwareFrames.load();
    if (requireDolbyVision) {
        std::cout
            << ", Dolby Vision metadata frames: "
            << dolbyVisionFrames.load();
    }
    std::cout << '\n';
}

} // namespace

int main(int argc, char** argv)
{
    if (argc < 1 || argc > 3) {
        return 2;
    }
    testWarpContracts();
    if (argc >= 2) {
        const std::string_view first = argv[1];
        if (first.rfind("https://", 0) == 0
            || first.rfind("http://", 0) == 0) {
            testNativeZeroCopy(
                argv[1],
                qtav::PixelFormat::P010,
                false,
                true,
                48);
        } else {
            testNativeZeroCopy(
                argv[1],
                qtav::PixelFormat::NV12,
                true);
        }
    }
    if (argc == 3) {
        if (std::filesystem::exists(argv[2])) {
            testNativeZeroCopy(
                argv[2],
                qtav::PixelFormat::P010,
                false);
        } else {
            std::cout
                << "HEVC Main10 test media unavailable; "
                   "Main10 zero-copy test skipped\n";
        }
    }
    return 0;
}
