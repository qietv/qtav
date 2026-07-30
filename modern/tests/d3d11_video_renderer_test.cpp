// SPDX-License-Identifier: LGPL-2.1-or-later

#if defined(NDEBUG)
#  undef NDEBUG
#endif

#if !defined(NOMINMAX)
#  define NOMINMAX
#endif

#include <d3d11.h>
#include <wrl/client.h>

#include <qtav/d3d11_video_renderer.h>
#include <qtav/player.h>

#include "d3d11_video_renderer_p.h"
#include "frame_internal.h"

#include <algorithm>
#include <cassert>
#include <chrono>
#include <condition_variable>
#include <cstdint>
#include <cstring>
#include <future>
#include <iterator>
#include <memory>
#include <mutex>
#include <string>
#include <utility>
#include <vector>

namespace {

using Microsoft::WRL::ComPtr;

struct Pixel {
    std::uint8_t blue = 0;
    std::uint8_t green = 0;
    std::uint8_t red = 0;
    std::uint8_t alpha = 0;
};

struct DeviceResources {
    ComPtr<ID3D11Device> device;
    ComPtr<ID3D11DeviceContext> context;
};

struct Target {
    ComPtr<ID3D11Texture2D> texture;
    ComPtr<ID3D11RenderTargetView> view;
};

class MockMapping final : public qtav::HardwareFrameMapping {
public:
    MockMapping()
        : pixels_(64)
    {
        for (int y = 0; y < 8; ++y) {
            for (int x = 0; x < 8; ++x) {
                auto& value = pixels_[static_cast<std::size_t>(y * 8 + x)];
                value = x < 4
                    ? Pixel { 0, 0, 255, 255 }
                    : Pixel { 255, 0, 0, 255 };
            }
        }
    }

    int width() const noexcept override
    {
        return 8;
    }

    int height() const noexcept override
    {
        return 8;
    }

    qtav::PixelFormat format() const noexcept override
    {
        return qtav::PixelFormat::BGRA;
    }

    int planeCount() const noexcept override
    {
        return 1;
    }

    const std::uint8_t* data(int plane) const noexcept override
    {
        return plane == 0
            ? reinterpret_cast<const std::uint8_t*>(pixels_.data())
            : nullptr;
    }

    std::uint8_t* writableData(int plane) noexcept override
    {
        return plane == 0
            ? reinterpret_cast<std::uint8_t*>(pixels_.data())
            : nullptr;
    }

    int lineSize(int plane) const noexcept override
    {
        return plane == 0 ? 8 * static_cast<int>(sizeof(Pixel)) : 0;
    }

private:
    std::vector<Pixel> pixels_;
};

class MockHardwareFrameData final : public qtav::HardwareFrameData {
public:
    explicit MockHardwareFrameData(
        std::shared_ptr<qtav::HardwareFrameMapping> mapping = {},
        int* mapCalls = nullptr,
        bool mappable = false)
        : mapping_(std::move(mapping))
        , mapCalls_(mapCalls)
        , mappable_(mappable)
    {
    }

    qtav::HardwareDeviceType deviceType() const noexcept override
    {
        return qtav::HardwareDeviceType::D3D11;
    }

    int width() const noexcept override
    {
        return 8;
    }

    int height() const noexcept override
    {
        return 8;
    }

    qtav::PixelFormat softwareFormat() const noexcept override
    {
        return qtav::PixelFormat::NV12;
    }

    qtav::NativeHandle nativeHandle(
        qtav::HardwareHandleType type) const noexcept override
    {
        return { type, 0, 0 };
    }

    bool isMappable(qtav::HardwareMapMode mode) const noexcept override
    {
        return mappable_ && mode == qtav::HardwareMapMode::Read;
    }

    std::shared_ptr<qtav::HardwareFrameMapping> map(
        qtav::HardwareMapMode mode) const override
    {
        if (mapCalls_) {
            ++*mapCalls_;
        }
        return isMappable(mode) ? mapping_ : nullptr;
    }

private:
    std::shared_ptr<qtav::HardwareFrameMapping> mapping_;
    int* mapCalls_ = nullptr;
    bool mappable_ = false;
};

class MockTextureFrame final : public qtav::D3D11TextureFrame {
public:
    MockTextureFrame(
        ComPtr<ID3D11Texture2D> texture,
        ComPtr<ID3D11ShaderResourceView> view)
        : texture_(std::move(texture))
        , view_(std::move(view))
    {
    }

    int width() const noexcept override
    {
        return 8;
    }

    int height() const noexcept override
    {
        return 8;
    }

    qtav::PixelFormat format() const noexcept override
    {
        return qtav::PixelFormat::BGRA;
    }

    ID3D11Texture2D* texture() const noexcept override
    {
        return texture_.Get();
    }

    ID3D11ShaderResourceView*
    shaderResourceView() const noexcept override
    {
        return view_.Get();
    }

private:
    ComPtr<ID3D11Texture2D> texture_;
    ComPtr<ID3D11ShaderResourceView> view_;
};

class MockHardwareFrameInterop final
    : public qtav::D3D11HardwareFrameInterop {
public:
    MockHardwareFrameInterop(
        std::shared_ptr<qtav::D3D11DeviceAccess> deviceAccess,
        std::shared_ptr<qtav::D3D11TextureFrame> imported)
        : deviceAccess_(std::move(deviceAccess))
        , imported_(std::move(imported))
    {
    }

    std::shared_ptr<qtav::D3D11DeviceAccess>
    deviceAccess() const noexcept override
    {
        return deviceAccess_;
    }

    qtav::HardwareInteropCapabilities capabilities() const override
    {
        return {
            { qtav::HardwareDeviceType::D3D11 },
            qtav::HardwareDeviceType::D3D11,
            true,
            false,
        };
    }

    bool supports(
        const qtav::HardwareFrame& frame) const noexcept override
    {
        return frame.deviceType() == qtav::HardwareDeviceType::D3D11;
    }

    std::shared_ptr<qtav::D3D11TextureFrame> importFrame(
        const qtav::HardwareFrame& frame) override
    {
        ++importCalls_;
        return supports(frame) && importSucceeds_
            ? imported_
            : std::shared_ptr<qtav::D3D11TextureFrame> {};
    }

    void setImportSucceeds(bool succeeds) noexcept
    {
        importSucceeds_ = succeeds;
    }

    int importCalls() const noexcept
    {
        return importCalls_;
    }

private:
    std::shared_ptr<qtav::D3D11DeviceAccess> deviceAccess_;
    std::shared_ptr<qtav::D3D11TextureFrame> imported_;
    bool importSucceeds_ = true;
    int importCalls_ = 0;
};

DeviceResources makeDevice()
{
    DeviceResources result;
    const D3D_FEATURE_LEVEL levels[] {
        D3D_FEATURE_LEVEL_11_1,
        D3D_FEATURE_LEVEL_11_0,
    };
    D3D_FEATURE_LEVEL selected = D3D_FEATURE_LEVEL_11_0;
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
    assert(SUCCEEDED(status));
    assert(selected >= D3D_FEATURE_LEVEL_11_0);
    return result;
}

Target makeTarget(ID3D11Device* device, int width, int height)
{
    D3D11_TEXTURE2D_DESC descriptor {};
    descriptor.Width = static_cast<UINT>(width);
    descriptor.Height = static_cast<UINT>(height);
    descriptor.MipLevels = 1;
    descriptor.ArraySize = 1;
    descriptor.Format = DXGI_FORMAT_B8G8R8A8_UNORM;
    descriptor.SampleDesc.Count = 1;
    descriptor.Usage = D3D11_USAGE_DEFAULT;
    descriptor.BindFlags =
        D3D11_BIND_RENDER_TARGET | D3D11_BIND_SHADER_RESOURCE;

    Target result;
    assert(SUCCEEDED(device->CreateTexture2D(
        &descriptor,
        nullptr,
        &result.texture)));
    assert(SUCCEEDED(device->CreateRenderTargetView(
        result.texture.Get(),
        nullptr,
        &result.view)));
    return result;
}

std::vector<Pixel> readTarget(
    ID3D11Device* device,
    ID3D11DeviceContext* context,
    ID3D11Texture2D* texture)
{
    context->OMSetRenderTargets(0, nullptr, nullptr);

    D3D11_TEXTURE2D_DESC descriptor {};
    texture->GetDesc(&descriptor);
    descriptor.Usage = D3D11_USAGE_STAGING;
    descriptor.BindFlags = 0;
    descriptor.CPUAccessFlags = D3D11_CPU_ACCESS_READ;
    descriptor.MiscFlags = 0;

    ComPtr<ID3D11Texture2D> staging;
    assert(SUCCEEDED(device->CreateTexture2D(
        &descriptor,
        nullptr,
        &staging)));
    context->CopyResource(staging.Get(), texture);

    D3D11_MAPPED_SUBRESOURCE mapped {};
    assert(SUCCEEDED(context->Map(
        staging.Get(),
        0,
        D3D11_MAP_READ,
        0,
        &mapped)));
    std::vector<Pixel> result(
        static_cast<std::size_t>(descriptor.Width)
        * static_cast<std::size_t>(descriptor.Height));
    for (UINT y = 0; y < descriptor.Height; ++y) {
        std::memcpy(
            result.data()
                + static_cast<std::size_t>(y) * descriptor.Width,
            static_cast<const std::uint8_t*>(mapped.pData)
                + static_cast<std::size_t>(y) * mapped.RowPitch,
            static_cast<std::size_t>(descriptor.Width) * sizeof(Pixel));
    }
    context->Unmap(staging.Get(), 0);
    return result;
}

Pixel pixel(
    const std::vector<Pixel>& pixels,
    int width,
    int x,
    int y)
{
    return pixels[static_cast<std::size_t>(y * width + x)];
}

bool isBlack(Pixel value)
{
    return value.red < 8 && value.green < 8 && value.blue < 8
        && value.alpha > 247;
}

bool isRed(Pixel value)
{
    return value.red > 180 && value.red > value.green * 2
        && value.red > value.blue * 2 && value.alpha > 247;
}

bool isBlue(Pixel value)
{
    return value.blue > 180 && value.blue > value.green * 2
        && value.blue > value.red * 2 && value.alpha > 247;
}

qtav::VideoFrame renderFile(
    const char* path,
    qtav::PixelFormat expectedFormat,
    const std::shared_ptr<qtav::D3D11VideoRenderer>& renderer)
{
    std::mutex mutex;
    std::condition_variable condition;
    bool finished = false;
    bool rendered = false;
    qtav::VideoFrame captured;

    qtav::Player player;
    player
        .setVideoRenderAPI(renderer)
        .onVideoFrame([&](const qtav::VideoFrame& frame, int) {
            captured = frame;
        })
        .setRenderCallback([&](void*) {
            rendered = player.renderVideo() >= 0.0;
            {
                std::lock_guard<std::mutex> lock(mutex);
                finished = true;
            }
            condition.notify_one();
            player.setState(qtav::State::Stopped);
        });
    player.setMedia(path);
    player.setState(qtav::State::Playing);

    {
        std::unique_lock<std::mutex> lock(mutex);
        assert(condition.wait_for(
            lock,
            std::chrono::seconds(10),
            [&] { return finished; }));
    }
    assert(rendered);
    assert(captured);
    assert(captured.format() == expectedFormat);
    return captured;
}

} // namespace

int main(int argc, char** argv)
{
    assert(argc == 4);
    assert(qtav::detail::d3d11FailureEvent(DXGI_ERROR_DEVICE_HUNG)
        == qtav::VideoRenderEventType::SurfaceLost);
    assert(qtav::detail::d3d11FailureEvent(DXGI_ERROR_DEVICE_REMOVED)
        == qtav::VideoRenderEventType::SurfaceLost);
    assert(qtav::detail::d3d11FailureEvent(DXGI_ERROR_DEVICE_RESET)
        == qtav::VideoRenderEventType::SurfaceLost);
    assert(qtav::detail::d3d11FailureEvent(
               DXGI_ERROR_DRIVER_INTERNAL_ERROR)
        == qtav::VideoRenderEventType::SurfaceLost);
    assert(qtav::detail::d3d11FailureEvent(E_FAIL)
        == qtav::VideoRenderEventType::Error);

    DeviceResources d3d = makeDevice();
    qtav::BorrowedD3D11Device borrowedDevice(d3d.device.Get());
    qtav::BorrowedD3D11DeviceContext borrowedContext(d3d.context.Get());
    assert(borrowedDevice);
    assert(borrowedContext);
    assert(borrowedDevice.get() == d3d.device.Get());
    assert(borrowedContext.get() == d3d.context.Get());

    Target target = makeTarget(d3d.device.Get(), 8, 8);
    bool exposeTarget = true;
    auto deviceAccess = qtav::D3D11DeviceAccess::create(
        borrowedDevice,
        borrowedContext);
    assert(deviceAccess);

    Target importedTarget = makeTarget(d3d.device.Get(), 8, 8);
    std::vector<Pixel> importedPixels(64);
    for (int y = 0; y < 8; ++y) {
        for (int x = 0; x < 8; ++x) {
            importedPixels[static_cast<std::size_t>(y * 8 + x)] =
                x < 4 ? Pixel { 0, 0, 255, 255 }
                      : Pixel { 255, 0, 0, 255 };
        }
    }
    d3d.context->UpdateSubresource(
        importedTarget.texture.Get(),
        0,
        nullptr,
        importedPixels.data(),
        8 * static_cast<UINT>(sizeof(Pixel)),
        0);
    ComPtr<ID3D11ShaderResourceView> importedView;
    assert(SUCCEEDED(d3d.device->CreateShaderResourceView(
        importedTarget.texture.Get(),
        nullptr,
        &importedView)));
    auto textureFrame = std::make_shared<MockTextureFrame>(
        importedTarget.texture,
        importedView);
    auto mockInterop = std::make_shared<MockHardwareFrameInterop>(
        deviceAccess,
        textureFrame);
    const auto interopCapabilities = mockInterop->capabilities();
    assert(interopCapabilities.zeroCopy);
    assert(!interopCapabilities.cpuFallback);
    assert(
        interopCapabilities.targetDevice
        == qtav::HardwareDeviceType::D3D11);
    assert(interopCapabilities.sourceDevices.size() == 1);
    assert(
        interopCapabilities.sourceDevices.front()
        == qtav::HardwareDeviceType::D3D11);
    assert(mockInterop->deviceAccess() == deviceAccess);

    const qtav::HardwareFrame hardwareFrame(
        std::make_shared<MockHardwareFrameData>());
    assert(mockInterop->supports(hardwareFrame));
    auto importedFrame = mockInterop->importFrame(hardwareFrame);
    assert(importedFrame == textureFrame);
    importedTarget = {};
    importedView.Reset();
    textureFrame.reset();
    assert(importedFrame->width() == 8);
    assert(importedFrame->height() == 8);
    assert(importedFrame->format() == qtav::PixelFormat::BGRA);
    assert(importedFrame->texture());
    assert(importedFrame->shaderResourceView());
    D3D11_TEXTURE2D_DESC importedDescription {};
    importedFrame->texture()->GetDesc(&importedDescription);
    assert(importedDescription.Width == 8);
    assert(importedDescription.Height == 8);

    auto renderer = std::make_shared<qtav::D3D11VideoRenderer>(
        deviceAccess,
        [&] {
            return qtav::D3D11RenderTarget {
                exposeTarget ? target.view.Get() : nullptr,
            };
        });

    const auto capabilities = renderer->capabilities();
    assert(capabilities.customViewport);
    assert(capabilities.rotation);
    assert(std::find(
               capabilities.softwareFormats.begin(),
               capabilities.softwareFormats.end(),
               qtav::PixelFormat::NV12)
        != capabilities.softwareFormats.end());
    assert(std::find(
               capabilities.softwareFormats.begin(),
               capabilities.softwareFormats.end(),
               qtav::PixelFormat::P010)
        != capabilities.softwareFormats.end());
    assert(capabilities.hardwareDevices.empty());
    assert(!renderer->hardwareFrameInterop());
    assert(!renderer->allowSoftwareMappingFallback());

    int errors = 0;
    int surfacesLost = 0;
    int redraws = 0;
    bool softwareFallbackReported = false;
    renderer->setEventCallback([&](const qtav::VideoRenderEvent& event) {
        if (event.type == qtav::VideoRenderEventType::Error) {
            ++errors;
            softwareFallbackReported =
                softwareFallbackReported
                || event.detail.find("software-mapping fallback")
                    != std::string::npos;
        } else if (event.type == qtav::VideoRenderEventType::SurfaceLost) {
            ++surfacesLost;
        } else if (
            event.type == qtav::VideoRenderEventType::RedrawRequested) {
            ++redraws;
        }
    });

    qtav::VideoRenderConfig invalid;
    invalid.surfaceSize = { 8, 8 };
    invalid.viewport = { 7, 0, 2, 2 };
    assert(!renderer->open(invalid));
    assert(errors == 1);

    qtav::VideoRenderConfig config;
    config.surfaceSize = { 8, 8 };
    assert(renderer->open(config));
    assert(renderer->device().get() == d3d.device.Get());
    assert(renderer->context().get() == d3d.context.Get());
    assert(renderer->deviceAccess() == deviceAccess);

    int mapCalls = 0;
    const auto mappedPixels = std::make_shared<MockMapping>();
    const qtav::HardwareFrame renderHardware(
        std::make_shared<MockHardwareFrameData>(
            mappedPixels,
            &mapCalls,
            true));
    const qtav::VideoFrame hardwareVideo =
        qtav::detail::FrameFactory::hardware(renderHardware);
    assert(hardwareVideo);
    assert(hardwareVideo.hasHardwareFrame());

    const int importsBeforeRender = mockInterop->importCalls();
    renderer->setHardwareFrameInterop(mockInterop);
    assert(renderer->hardwareFrameInterop() == mockInterop);
    const auto hardwareCapabilities = renderer->capabilities();
    assert(hardwareCapabilities.hardwareDevices.size() == 1);
    assert(
        hardwareCapabilities.hardwareDevices.front()
        == qtav::HardwareDeviceType::D3D11);
    assert(renderer->render(hardwareVideo));
    assert(mockInterop->importCalls() == importsBeforeRender + 1);
    assert(mapCalls == 0);
    auto pixels =
        readTarget(d3d.device.Get(), d3d.context.Get(), target.texture.Get());
    assert(isRed(pixel(pixels, 8, 1, 3)));
    assert(isBlue(pixel(pixels, 8, 6, 3)));

    mockInterop->setImportSucceeds(false);
    assert(!renderer->render(hardwareVideo));
    assert(mapCalls == 0);

    renderer->setAllowSoftwareMappingFallback(true);
    assert(renderer->allowSoftwareMappingFallback());
    assert(renderer->render(hardwareVideo));
    assert(mapCalls == 1);
    assert(softwareFallbackReported);
    pixels =
        readTarget(d3d.device.Get(), d3d.context.Get(), target.texture.Get());
    assert(isRed(pixel(pixels, 8, 1, 3)));
    assert(isBlue(pixel(pixels, 8, 6, 3)));

    int failedMapCalls = 0;
    const qtav::VideoFrame unmappableVideo =
        qtav::detail::FrameFactory::hardware(qtav::HardwareFrame(
            std::make_shared<MockHardwareFrameData>(
                nullptr,
                &failedMapCalls,
                true)));
    assert(!renderer->render(unmappableVideo));
    assert(failedMapCalls == 1);

    renderer->setAllowSoftwareMappingFallback(false);
    renderer->setHardwareFrameInterop({});
    assert(renderer->capabilities().hardwareDevices.empty());
    errors = 0;
    redraws = 0;

    const qtav::VideoFrame rgb =
        renderFile(argv[1], qtav::PixelFormat::RGB24, renderer);
    std::future<bool> blockedRender;
    {
        auto contextGuard = deviceAccess->contextGuard();
        blockedRender = std::async(
            std::launch::async,
            [&] { return renderer->render(rgb); });
        assert(blockedRender.wait_for(std::chrono::milliseconds(50))
            == std::future_status::timeout);
    }
    assert(blockedRender.wait_for(std::chrono::seconds(2))
        == std::future_status::ready);
    assert(blockedRender.get());

    pixels =
        readTarget(d3d.device.Get(), d3d.context.Get(), target.texture.Get());
    assert(isBlack(pixel(pixels, 8, 3, 0)));
    assert(isRed(pixel(pixels, 8, 1, 3)));
    assert(isBlue(pixel(pixels, 8, 6, 3)));
    assert(isBlack(pixel(pixels, 8, 3, 7)));

    exposeTarget = false;
    assert(!renderer->render(rgb));
    assert(surfacesLost == 1);
    exposeTarget = true;

    config.viewport = { 2, 1, 4, 6 };
    config.aspectRatio = qtav::VideoAspectRatioMode::Stretch;
    assert(renderer->configure(config));
    assert(redraws == 1);
    assert(renderer->render(rgb));
    pixels =
        readTarget(d3d.device.Get(), d3d.context.Get(), target.texture.Get());
    assert(isBlack(pixel(pixels, 8, 1, 3)));
    assert(isRed(pixel(pixels, 8, 2, 3)));
    assert(isBlue(pixel(pixels, 8, 5, 3)));
    assert(isBlack(pixel(pixels, 8, 6, 3)));

    config.viewport = {};
    config.rotation = qtav::VideoRotation::Rotate180;
    assert(renderer->configure(config));
    assert(renderer->render(rgb));
    pixels =
        readTarget(d3d.device.Get(), d3d.context.Get(), target.texture.Get());
    assert(isBlue(pixel(pixels, 8, 1, 3)));
    assert(isRed(pixel(pixels, 8, 6, 3)));

    config.rotation = qtav::VideoRotation::Rotate90;
    config.aspectRatio = qtav::VideoAspectRatioMode::Fit;
    assert(renderer->configure(config));
    assert(renderer->render(rgb));
    pixels =
        readTarget(d3d.device.Get(), d3d.context.Get(), target.texture.Get());
    assert(isBlack(pixel(pixels, 8, 0, 3)));
    assert(isRed(pixel(pixels, 8, 3, 1)));
    assert(isBlue(pixel(pixels, 8, 3, 6)));

    config.rotation = qtav::VideoRotation::Rotate0;
    config.aspectRatio = qtav::VideoAspectRatioMode::Fill;
    assert(renderer->configure(config));
    assert(renderer->render(rgb));
    pixels =
        readTarget(d3d.device.Get(), d3d.context.Get(), target.texture.Get());
    assert(!isBlack(pixel(pixels, 8, 0, 0)));
    assert(!isBlack(pixel(pixels, 8, 7, 7)));

    config.aspectRatio = qtav::VideoAspectRatioMode::Stretch;
    assert(renderer->configure(config));
    renderFile(argv[2], qtav::PixelFormat::YUV420P, renderer);
    pixels =
        readTarget(d3d.device.Get(), d3d.context.Get(), target.texture.Get());
    assert(isRed(pixel(pixels, 8, 1, 3)));
    assert(isBlue(pixel(pixels, 8, 6, 3)));

    renderFile(argv[3], qtav::PixelFormat::NV12, renderer);
    pixels =
        readTarget(d3d.device.Get(), d3d.context.Get(), target.texture.Get());
    assert(isRed(pixel(pixels, 8, 1, 3)));
    assert(isBlue(pixel(pixels, 8, 6, 3)));

    Target firstSurface = std::move(target);
    target = makeTarget(d3d.device.Get(), 6, 4);
    config.surfaceSize = { 6, 4 };
    assert(renderer->configure(config));
    assert(renderer->render(rgb));
    pixels =
        readTarget(d3d.device.Get(), d3d.context.Get(), target.texture.Get());
    assert(isRed(pixel(pixels, 6, 1, 2)));
    assert(isBlue(pixel(pixels, 6, 4, 2)));

    Target recreated = makeTarget(d3d.device.Get(), 6, 4);
    target = std::move(recreated);
    assert(renderer->render(rgb));
    pixels =
        readTarget(d3d.device.Get(), d3d.context.Get(), target.texture.Get());
    assert(isRed(pixel(pixels, 6, 1, 2)));
    assert(isBlue(pixel(pixels, 6, 4, 2)));

    DeviceResources foreignDevice = makeDevice();
    Target foreignTarget =
        makeTarget(foreignDevice.device.Get(), 6, 4);
    Target localTarget = std::move(target);
    target = std::move(foreignTarget);
    assert(!renderer->render(rgb));
    assert(errors == 1);
    target = std::move(localTarget);

    renderer->close();
    assert(!renderer->render(rgb));
    assert(errors == 2);
    return 0;
}
