// SPDX-License-Identifier: LGPL-2.1-or-later

#include <qtav/d3d11_frame_interop.h>

#include <qtav/d3d11va_hardware_decoder.h>

#include <utility>

namespace qtav {
namespace {

DXGI_FORMAT nativeFormat(PixelFormat format) noexcept
{
    switch (format) {
    case PixelFormat::NV12:
        return DXGI_FORMAT_NV12;
    case PixelFormat::P010:
        return DXGI_FORMAT_P010;
    default:
        return DXGI_FORMAT_UNKNOWN;
    }
}

bool supportedSource(
    const D3D11VAFrame& frame,
    ID3D11Device* expectedDevice) noexcept
{
    if (!frame || !expectedDevice
        || frame.device() != expectedDevice) {
        return false;
    }

    D3D11_TEXTURE2D_DESC description {};
    frame.texture()->GetDesc(&description);
    return description.Format == nativeFormat(frame.softwareFormat())
        && description.Usage == D3D11_USAGE_DEFAULT
        && description.MipLevels == 1
        && description.SampleDesc.Count == 1
        && frame.arraySlice() < description.ArraySize
        && (description.BindFlags & D3D11_BIND_SHADER_RESOURCE) != 0;
}

class ImportedD3D11DecoderFrame final : public D3D11TextureFrame {
public:
    explicit ImportedD3D11DecoderFrame(D3D11VAFrame frame)
        : frame_(std::move(frame))
    {
    }

    int width() const noexcept override
    {
        return frame_.width();
    }

    int height() const noexcept override
    {
        return frame_.height();
    }

    PixelFormat format() const noexcept override
    {
        return frame_.softwareFormat();
    }

    ID3D11Texture2D* texture() const noexcept override
    {
        return frame_.texture();
    }

    ID3D11ShaderResourceView*
    shaderResourceView() const noexcept override
    {
        // libplacebo creates plane-specific SRVs directly from the retained
        // NV12/P010 array slice. A pre-converted RGB view would destroy the
        // raw Profile 5 base-layer representation.
        return nullptr;
    }

    UINT arraySlice() const noexcept override
    {
        return frame_.arraySlice();
    }

    DXGI_FORMAT dxgiFormat() const noexcept override
    {
        return nativeFormat(frame_.softwareFormat());
    }

    DXGI_COLOR_SPACE_TYPE colorSpace() const noexcept override
    {
        // Source color semantics come from VideoFrame metadata, including the
        // Dolby Vision RPU, rather than a Video Processor RGB conversion.
        return DXGI_COLOR_SPACE_YCBCR_STUDIO_G22_LEFT_P709;
    }

private:
    D3D11VAFrame frame_;
};

} // namespace

class D3D11FrameInterop::Impl {
public:
    explicit Impl(std::shared_ptr<D3D11DeviceAccess> deviceAccess)
        : deviceAccess_(std::move(deviceAccess))
    {
    }

    bool available() const noexcept
    {
        return deviceAccess_ && deviceAccess_->device();
    }

    std::shared_ptr<D3D11DeviceAccess> deviceAccess_;
};

D3D11FrameInterop::D3D11FrameInterop(
    std::shared_ptr<D3D11DeviceAccess> deviceAccess)
    : impl_(std::make_unique<Impl>(std::move(deviceAccess)))
{
}

D3D11FrameInterop::~D3D11FrameInterop() = default;
D3D11FrameInterop::D3D11FrameInterop(
    D3D11FrameInterop&&) noexcept = default;
D3D11FrameInterop& D3D11FrameInterop::operator=(
    D3D11FrameInterop&&) noexcept = default;

std::shared_ptr<D3D11DeviceAccess>
D3D11FrameInterop::deviceAccess() const noexcept
{
    return impl_ ? impl_->deviceAccess_
                 : std::shared_ptr<D3D11DeviceAccess> {};
}

HardwareInteropCapabilities
D3D11FrameInterop::capabilities() const
{
    HardwareInteropCapabilities result;
    if (impl_ && impl_->available()) {
        result.sourceDevices = { HardwareDeviceType::D3D11 };
        result.targetDevice = HardwareDeviceType::D3D11;
        result.zeroCopy = true;
    }
    return result;
}

bool D3D11FrameInterop::supports(
    const HardwareFrame& frame) const noexcept
{
    return impl_ && impl_->available()
        && supportedSource(
            d3d11vaFrame(frame),
            impl_->deviceAccess_->device().get());
}

std::shared_ptr<D3D11TextureFrame>
D3D11FrameInterop::importFrame(
    const HardwareFrame& frame)
{
    return importFrame(frame, {});
}

std::shared_ptr<D3D11TextureFrame>
D3D11FrameInterop::importFrame(
    const HardwareFrame& frame,
    const VideoColorSpace&)
{
    if (!impl_ || !impl_->available()) {
        return {};
    }
    D3D11VAFrame native = d3d11vaFrame(frame);
    if (!supportedSource(
            native,
            impl_->deviceAccess_->device().get())) {
        return {};
    }
    return std::make_shared<ImportedD3D11DecoderFrame>(
        std::move(native));
}

void D3D11FrameInterop::flush() noexcept
{
    // The interop owns no conversion textures or queued work. libplacebo
    // wraps each retained decoder slice only for command submission.
}

} // namespace qtav
