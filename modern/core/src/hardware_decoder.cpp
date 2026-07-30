// SPDX-License-Identifier: LGPL-2.1-or-later

#include <qtav/hardware_decoder.h>

#include <utility>

#include "hardware_decode_device_internal.h"

extern "C" {
#include <libavutil/buffer.h>
#include <libavutil/hwcontext.h>
}

namespace qtav {
namespace {

HardwareDeviceType hardwareDeviceType(AVHWDeviceType type) noexcept
{
    switch (type) {
    case AV_HWDEVICE_TYPE_D3D11VA:
        return HardwareDeviceType::D3D11;
    case AV_HWDEVICE_TYPE_VIDEOTOOLBOX:
        return HardwareDeviceType::VideoToolbox;
    case AV_HWDEVICE_TYPE_VAAPI:
        return HardwareDeviceType::VAAPI;
    case AV_HWDEVICE_TYPE_MEDIACODEC:
        return HardwareDeviceType::MediaCodec;
    case AV_HWDEVICE_TYPE_VULKAN:
        return HardwareDeviceType::Vulkan;
    default:
        return HardwareDeviceType::Unknown;
    }
}

} // namespace

class HardwareDecodeDevice::Impl {
public:
    Impl(
        HardwareDeviceType deviceType,
        std::uintptr_t identity,
        AVBufferRef* deviceContext) noexcept
        : type(deviceType)
        , nativeIdentity(identity)
        , context(deviceContext)
    {
    }

    ~Impl()
    {
        av_buffer_unref(&context);
    }

    HardwareDeviceType type = HardwareDeviceType::Unknown;
    std::uintptr_t nativeIdentity = 0;
    AVBufferRef* context = nullptr;
};

HardwareDecodeDevice::HardwareDecodeDevice() noexcept = default;
HardwareDecodeDevice::HardwareDecodeDevice(
    const HardwareDecodeDevice&) noexcept = default;
HardwareDecodeDevice::HardwareDecodeDevice(
    HardwareDecodeDevice&&) noexcept = default;
HardwareDecodeDevice& HardwareDecodeDevice::operator=(
    const HardwareDecodeDevice&) noexcept = default;
HardwareDecodeDevice& HardwareDecodeDevice::operator=(
    HardwareDecodeDevice&&) noexcept = default;
HardwareDecodeDevice::~HardwareDecodeDevice() = default;

HardwareDecodeDevice::HardwareDecodeDevice(
    std::shared_ptr<const Impl> impl) noexcept
    : impl_(std::move(impl))
{
}

HardwareDecodeDevice::operator bool() const noexcept
{
    return isValid();
}

bool HardwareDecodeDevice::isValid() const noexcept
{
    return impl_ && impl_->type != HardwareDeviceType::Unknown
        && impl_->context;
}

HardwareDeviceType HardwareDecodeDevice::deviceType() const noexcept
{
    return impl_ ? impl_->type : HardwareDeviceType::Unknown;
}

std::uintptr_t HardwareDecodeDevice::nativeIdentity() const noexcept
{
    return impl_ ? impl_->nativeIdentity : 0;
}

bool HardwareDecodeDevice::operator==(
    const HardwareDecodeDevice& other) const noexcept
{
    return impl_ == other.impl_;
}

bool HardwareDecodeDevice::operator!=(
    const HardwareDecodeDevice& other) const noexcept
{
    return !(*this == other);
}

namespace detail {

HardwareDecodeDevice HardwareDecodeDevicePrivate::create(
    HardwareDeviceType type,
    std::uintptr_t nativeIdentity,
    AVBufferRef* context) noexcept
{
    if (type == HardwareDeviceType::Unknown || !context || !context->data
        || context->size < sizeof(AVHWDeviceContext)) {
        return {};
    }

    const auto* deviceContext =
        reinterpret_cast<const AVHWDeviceContext*>(context->data);
    if (hardwareDeviceType(deviceContext->type) != type) {
        return {};
    }

    auto* referencedContext = av_buffer_ref(context);
    if (!referencedContext) {
        return {};
    }

    try {
        return HardwareDecodeDevice(std::make_shared<HardwareDecodeDevice::Impl>(
            type,
            nativeIdentity,
            referencedContext));
    } catch (...) {
        av_buffer_unref(&referencedContext);
        return {};
    }
}

AVBufferRef* HardwareDecodeDevicePrivate::contextRef(
    const HardwareDecodeDevice& device) noexcept
{
    return device.impl_ ? av_buffer_ref(device.impl_->context) : nullptr;
}

} // namespace detail
} // namespace qtav
