// SPDX-License-Identifier: LGPL-2.1-or-later

#include <qtav/d3d11va_hardware_decoder.h>

#include <algorithm>
#include <cstdint>
#include <new>
#include <utility>

#include "d3d11_device_access_internal.h"
#include "hardware_decode_device_internal.h"

extern "C" {
#include <libavutil/buffer.h>
#include <libavutil/hwcontext.h>
#include <libavutil/hwcontext_d3d11va.h>
}

namespace qtav {
namespace {

struct D3D11VADeviceLifetime {
    explicit D3D11VADeviceLifetime(
        std::shared_ptr<D3D11DeviceAccess> selectedAccess)
        : access(std::move(selectedAccess))
    {
    }

    std::shared_ptr<D3D11DeviceAccess> access;
};

void lockD3D11Device(void* opaque) noexcept
{
    auto* lifetime =
        static_cast<D3D11VADeviceLifetime*>(opaque);
    if (lifetime && lifetime->access) {
        detail::D3D11DeviceAccessPrivate::lock(
            *lifetime->access);
    }
}

void unlockD3D11Device(void* opaque) noexcept
{
    auto* lifetime =
        static_cast<D3D11VADeviceLifetime*>(opaque);
    if (lifetime && lifetime->access) {
        detail::D3D11DeviceAccessPrivate::unlock(
            *lifetime->access);
    }
}

void freeD3D11Device(AVHWDeviceContext* context) noexcept
{
    if (!context) {
        return;
    }
    delete static_cast<D3D11VADeviceLifetime*>(
        context->user_opaque);
    context->user_opaque = nullptr;
}

HardwareDecodeDevice createDecodeDevice(
    const std::shared_ptr<D3D11DeviceAccess>& deviceAccess) noexcept
{
    if (!deviceAccess || !deviceAccess->device()
        || !deviceAccess->immediateContext()) {
        return {};
    }

    AVBufferRef* reference =
        av_hwdevice_ctx_alloc(AV_HWDEVICE_TYPE_D3D11VA);
    if (!reference) {
        return {};
    }

    auto* lifetime = new (std::nothrow) D3D11VADeviceLifetime(
        deviceAccess);
    if (!lifetime) {
        av_buffer_unref(&reference);
        return {};
    }

    auto* context =
        reinterpret_cast<AVHWDeviceContext*>(reference->data);
    auto* native =
        static_cast<AVD3D11VADeviceContext*>(context->hwctx);
    context->user_opaque = lifetime;
    context->free = &freeD3D11Device;
    native->device = deviceAccess->device().get();
    native->device_context =
        deviceAccess->immediateContext().get();
    native->device->AddRef();
    native->device_context->AddRef();
    native->lock = &lockD3D11Device;
    native->unlock = &unlockD3D11Device;
    native->lock_ctx = lifetime;

    if (av_hwdevice_ctx_init(reference) < 0) {
        av_buffer_unref(&reference);
        return {};
    }

    HardwareDecodeDevice result =
        detail::HardwareDecodeDevicePrivate::create(
            HardwareDeviceType::D3D11,
            reinterpret_cast<std::uintptr_t>(
                deviceAccess->device().get()),
            reference);
    av_buffer_unref(&reference);
    return result;
}

bool supportedSoftwareFormat(PixelFormat format) noexcept
{
    return format == PixelFormat::NV12
        || format == PixelFormat::P010;
}

} // namespace

HardwareDecodeConfig d3d11vaHardwareDecodeConfig(
    std::shared_ptr<D3D11DeviceAccess> deviceAccess,
    D3D11VAHardwareDecodeOptions options) noexcept
{
    HardwareDecodeConfig result;
    result.deviceType = HardwareDeviceType::D3D11;
    result.allowSoftwareFallback =
        options.allowSoftwareFallback;
    result.device = createDecodeDevice(deviceAccess);
    result.extraHardwareFrames = std::clamp(
        options.extraHardwareFrames,
        0,
        64);
    result.requireSuppliedDevice = true;
    return result;
}

D3D11VAFrame::D3D11VAFrame() noexcept = default;

D3D11VAFrame::D3D11VAFrame(
    HardwareFrame source,
    ID3D11Texture2D* texture,
    UINT arraySlice,
    ID3D11Device* device) noexcept
    : source_(std::move(source))
    , texture_(texture)
    , arraySlice_(arraySlice)
    , device_(device)
{
}

D3D11VAFrame::operator bool() const noexcept
{
    return isValid();
}

bool D3D11VAFrame::isValid() const noexcept
{
    return source_ && texture_ && device_;
}

ID3D11Texture2D* D3D11VAFrame::texture() const noexcept
{
    return texture_;
}

UINT D3D11VAFrame::arraySlice() const noexcept
{
    return arraySlice_;
}

ID3D11Device* D3D11VAFrame::device() const noexcept
{
    return device_;
}

int D3D11VAFrame::width() const noexcept
{
    return source_.width();
}

int D3D11VAFrame::height() const noexcept
{
    return source_.height();
}

PixelFormat D3D11VAFrame::softwareFormat() const noexcept
{
    return source_.softwareFormat();
}

const HardwareFrame& D3D11VAFrame::sourceFrame() const noexcept
{
    return source_;
}

D3D11VAFrame d3d11vaFrame(
    const HardwareFrame& frame) noexcept
{
    if (!frame
        || frame.deviceType() != HardwareDeviceType::D3D11
        || !supportedSoftwareFormat(frame.softwareFormat())) {
        return {};
    }

    const NativeHandle handle =
        frame.nativeHandle(HardwareHandleType::Texture);
    auto* texture =
        reinterpret_cast<ID3D11Texture2D*>(handle.value);
    if (!texture) {
        return {};
    }

    D3D11_TEXTURE2D_DESC description {};
    texture->GetDesc(&description);
    const DXGI_FORMAT expectedFormat =
        frame.softwareFormat() == PixelFormat::NV12
        ? DXGI_FORMAT_NV12
        : DXGI_FORMAT_P010;
    if (handle.subresource >= description.ArraySize
        || frame.width() <= 0 || frame.height() <= 0
        || description.Format != expectedFormat
        || static_cast<UINT>(frame.width()) > description.Width
        || static_cast<UINT>(frame.height()) > description.Height) {
        return {};
    }

    ID3D11Device* device = nullptr;
    texture->GetDevice(&device);
    if (!device) {
        return {};
    }
    // The texture retains its device. Balance GetDevice's reference while
    // keeping the borrowed identity valid through source_.
    device->Release();

    return D3D11VAFrame(
        frame,
        texture,
        handle.subresource,
        device);
}

} // namespace qtav
