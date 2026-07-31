// SPDX-License-Identifier: LGPL-2.1-or-later

#include <qtav/mediacodec_hardware_decoder.h>

#include <algorithm>
#include <atomic>
#include <new>
#include <utility>

#include "hardware_decode_device_internal.h"

extern "C" {
#include <libavcodec/mediacodec.h>
#include <libavutil/buffer.h>
#include <libavutil/hwcontext.h>
#include <libavutil/hwcontext_mediacodec.h>
}

namespace qtav {
namespace {

std::atomic<std::uint32_t> nextSurfaceGeneration { 1 };

std::uint32_t acquireSurfaceGeneration() noexcept
{
    std::uint32_t generation =
        nextSurfaceGeneration.fetch_add(1, std::memory_order_relaxed);
    while (generation == 0) {
        generation =
            nextSurfaceGeneration.fetch_add(1, std::memory_order_relaxed);
    }
    return generation;
}

void freeMediaCodecDevice(AVHWDeviceContext* context) noexcept
{
    if (!context || !context->user_opaque) {
        return;
    }
    ANativeWindow_release(
        static_cast<ANativeWindow*>(context->user_opaque));
    context->user_opaque = nullptr;
}

HardwareDecodeDevice createDecodeDevice(
    ANativeWindow* window) noexcept
{
    if (!window) {
        return {};
    }

    AVBufferRef* reference =
        av_hwdevice_ctx_alloc(AV_HWDEVICE_TYPE_MEDIACODEC);
    if (!reference) {
        return {};
    }

    ANativeWindow_acquire(window);
    auto* context =
        reinterpret_cast<AVHWDeviceContext*>(reference->data);
    auto* native =
        static_cast<AVMediaCodecDeviceContext*>(context->hwctx);
    context->user_opaque = window;
    context->free = &freeMediaCodecDevice;
    native->native_window = window;

    if (av_hwdevice_ctx_init(reference) < 0) {
        av_buffer_unref(&reference);
        return {};
    }

    HardwareDecodeDevice result =
        detail::HardwareDecodeDevicePrivate::create(
            HardwareDeviceType::MediaCodec,
            reinterpret_cast<std::uintptr_t>(window),
            reference);
    av_buffer_unref(&reference);
    return result;
}

AVMediaCodecBuffer* nativeBuffer(std::uintptr_t value) noexcept
{
    return reinterpret_cast<AVMediaCodecBuffer*>(value);
}

} // namespace

class MediaCodecSurface::Impl final {
public:
    explicit Impl(ANativeWindow* source)
        : window(source)
        , surfaceGeneration(acquireSurfaceGeneration())
    {
        ANativeWindow_acquire(window);
    }

    ~Impl()
    {
        ANativeWindow_release(window);
    }

    ANativeWindow* window = nullptr;
    std::uint32_t surfaceGeneration = 0;
};

MediaCodecSurface::MediaCodecSurface() noexcept = default;

MediaCodecSurface::MediaCodecSurface(ANativeWindow* window)
{
    if (!window) {
        return;
    }
    impl_ = std::make_shared<Impl>(window);
}

MediaCodecSurface::~MediaCodecSurface() = default;
MediaCodecSurface::MediaCodecSurface(
    const MediaCodecSurface&) noexcept = default;
MediaCodecSurface::MediaCodecSurface(
    MediaCodecSurface&&) noexcept = default;
MediaCodecSurface& MediaCodecSurface::operator=(
    const MediaCodecSurface&) noexcept = default;
MediaCodecSurface& MediaCodecSurface::operator=(
    MediaCodecSurface&&) noexcept = default;

MediaCodecSurface::operator bool() const noexcept
{
    return isValid();
}

bool MediaCodecSurface::isValid() const noexcept
{
    return impl_ && impl_->window && impl_->surfaceGeneration != 0;
}

ANativeWindow* MediaCodecSurface::nativeWindow() const noexcept
{
    return impl_ ? impl_->window : nullptr;
}

std::uint32_t MediaCodecSurface::generation() const noexcept
{
    return impl_ ? impl_->surfaceGeneration : 0;
}

HardwareDecodeConfig mediaCodecHardwareDecodeConfig(
    const MediaCodecSurface& surface,
    MediaCodecHardwareDecodeOptions options)
{
    HardwareDecodeConfig result;
    result.deviceType = HardwareDeviceType::MediaCodec;
    result.allowSoftwareFallback = options.allowSoftwareFallback;
    result.device = createDecodeDevice(surface.nativeWindow());
    result.extraHardwareFrames = std::clamp(
        options.extraHardwareFrames,
        0,
        64);
    result.requireSuppliedDevice = true;
    result.decoderWrapper = "mediacodec";
    result.surfaceGeneration = surface.generation();
    return result;
}

MediaCodecFrame::MediaCodecFrame() noexcept = default;
MediaCodecFrame::~MediaCodecFrame() = default;
MediaCodecFrame::MediaCodecFrame(MediaCodecFrame&&) noexcept = default;
MediaCodecFrame& MediaCodecFrame::operator=(
    MediaCodecFrame&&) noexcept = default;

MediaCodecFrame::MediaCodecFrame(
    HardwareFrame source,
    std::uintptr_t buffer,
    std::uint32_t generation,
    std::int64_t timestamp) noexcept
    : source_(std::move(source))
    , buffer_(buffer)
    , generation_(generation)
    , timestamp_(timestamp)
{
}

MediaCodecFrame::operator bool() const noexcept
{
    return isValid();
}

bool MediaCodecFrame::isValid() const noexcept
{
    return source_
        && source_.deviceType() == HardwareDeviceType::MediaCodec
        && buffer_ != 0 && generation_ != 0;
}

bool MediaCodecFrame::isPending() const noexcept
{
    return isValid() && !decided_;
}

std::uint32_t MediaCodecFrame::surfaceGeneration() const noexcept
{
    return generation_;
}

std::int64_t MediaCodecFrame::timestamp() const noexcept
{
    return timestamp_;
}

const HardwareFrame& MediaCodecFrame::sourceFrame() const noexcept
{
    return source_;
}

bool MediaCodecFrame::present() noexcept
{
    if (!isPending()) {
        return false;
    }
    decided_ = true;
    return av_mediacodec_release_buffer(nativeBuffer(buffer_), 1) >= 0;
}

bool MediaCodecFrame::presentAt(
    std::int64_t monotonicNanoseconds) noexcept
{
    if (!isPending() || monotonicNanoseconds <= 0) {
        return false;
    }
    decided_ = true;
    return av_mediacodec_render_buffer_at_time(
               nativeBuffer(buffer_),
               monotonicNanoseconds)
        >= 0;
}

bool MediaCodecFrame::drop() noexcept
{
    if (!isPending()) {
        return false;
    }
    decided_ = true;
    return av_mediacodec_release_buffer(nativeBuffer(buffer_), 0) >= 0;
}

MediaCodecFrame mediaCodecFrame(
    const VideoFrame& frame,
    const MediaCodecSurface& surface) noexcept
{
    if (!frame || !surface || !frame.hasHardwareFrame()) {
        return {};
    }
    HardwareFrame hardware = frame.hardwareFrame();
    if (!hardware
        || hardware.deviceType() != HardwareDeviceType::MediaCodec) {
        return {};
    }

    const NativeHandle output =
        hardware.nativeHandle(HardwareHandleType::Frame);
    const NativeHandle sourceSurface =
        hardware.nativeHandle(HardwareHandleType::Surface);
    const auto expectedWindow = reinterpret_cast<std::uintptr_t>(
        surface.nativeWindow());
    if (!output || !sourceSurface
        || sourceSurface.value != expectedWindow
        || output.subresource != surface.generation()
        || sourceSurface.subresource != surface.generation()) {
        return {};
    }

    return MediaCodecFrame(
        std::move(hardware),
        output.value,
        output.subresource,
        frame.timestamp());
}

} // namespace qtav
