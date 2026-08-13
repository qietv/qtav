// SPDX-License-Identifier: LGPL-2.1-or-later

#include <qtav/ohcodec_hardware_decoder.h>

#include <algorithm>
#include <atomic>
#include <cerrno>
#include <utility>

#include "hardware_decode_device_internal.h"

extern "C" {
#include <libavcodec/ohcodec_surface.h>
#include <libavutil/buffer.h>
#include <libavutil/error.h>
#include <libavutil/hwcontext.h>
#include <libavutil/hwcontext_oh.h>
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

bool retainWindow(OHNativeWindow* window) noexcept
{
    return window
        && OH_NativeWindow_NativeObjectReference(window) == 0;
}

void releaseWindow(OHNativeWindow* window) noexcept
{
    if (window) {
        OH_NativeWindow_NativeObjectUnreference(window);
    }
}

void freeOHCodecDevice(AVHWDeviceContext* context) noexcept
{
    if (!context || !context->user_opaque) {
        return;
    }
    releaseWindow(static_cast<OHNativeWindow*>(context->user_opaque));
    context->user_opaque = nullptr;
}

HardwareDecodeDevice createDecodeDevice(
    OHNativeWindow* window) noexcept
{
    if (!window) {
        return {};
    }

    AVBufferRef* reference =
        av_hwdevice_ctx_alloc(AV_HWDEVICE_TYPE_OHCODEC);
    if (!reference) {
        return {};
    }
    if (!retainWindow(window)) {
        av_buffer_unref(&reference);
        return {};
    }

    auto* context =
        reinterpret_cast<AVHWDeviceContext*>(reference->data);
    auto* native =
        static_cast<AVOHCodecDeviceContext*>(context->hwctx);
    context->user_opaque = window;
    context->free = &freeOHCodecDevice;
    native->native_window = window;

    if (av_hwdevice_ctx_init(reference) < 0) {
        av_buffer_unref(&reference);
        return {};
    }

    HardwareDecodeDevice result =
        detail::HardwareDecodeDevicePrivate::create(
            HardwareDeviceType::OHCodec,
            reinterpret_cast<std::uintptr_t>(window),
            reference);
    av_buffer_unref(&reference);
    return result;
}

AVOHCodecBuffer* nativeBuffer(std::uintptr_t value) noexcept
{
    return reinterpret_cast<AVOHCodecBuffer*>(value);
}

} // namespace

class OHCodecSurface::Impl final {
public:
    explicit Impl(OHNativeWindow* source)
    {
        if (retainWindow(source)) {
            window = source;
            surfaceGeneration = acquireSurfaceGeneration();
        }
    }

    ~Impl()
    {
        releaseWindow(window);
    }

    OHNativeWindow* window = nullptr;
    std::uint32_t surfaceGeneration = 0;
};

OHCodecSurface::OHCodecSurface() noexcept = default;

OHCodecSurface::OHCodecSurface(OHNativeWindow* window)
{
    if (!window) {
        return;
    }
    auto impl = std::make_shared<Impl>(window);
    if (impl->window) {
        impl_ = std::move(impl);
    }
}

OHCodecSurface::~OHCodecSurface() = default;
OHCodecSurface::OHCodecSurface(const OHCodecSurface&) noexcept = default;
OHCodecSurface::OHCodecSurface(OHCodecSurface&&) noexcept = default;
OHCodecSurface& OHCodecSurface::operator=(
    const OHCodecSurface&) noexcept = default;
OHCodecSurface& OHCodecSurface::operator=(
    OHCodecSurface&&) noexcept = default;

OHCodecSurface::operator bool() const noexcept
{
    return isValid();
}

bool OHCodecSurface::isValid() const noexcept
{
    return impl_ && impl_->window && impl_->surfaceGeneration != 0;
}

OHNativeWindow* OHCodecSurface::nativeWindow() const noexcept
{
    return impl_ ? impl_->window : nullptr;
}

std::uint32_t OHCodecSurface::generation() const noexcept
{
    return impl_ ? impl_->surfaceGeneration : 0;
}

HardwareDecodeConfig ohCodecHardwareDecodeConfig(
    const OHCodecSurface& surface,
    OHCodecHardwareDecodeOptions options)
{
    HardwareDecodeConfig result;
    result.deviceType = HardwareDeviceType::OHCodec;
    result.allowSoftwareFallback = options.allowSoftwareFallback;
    result.device = createDecodeDevice(surface.nativeWindow());
    result.extraHardwareFrames = std::clamp(
        options.extraHardwareFrames,
        0,
        64);
    result.requireSuppliedDevice = true;
    result.decoderWrapper = "ohcodec";
    result.surfaceGeneration = surface.generation();
    return result;
}

OHCodecFrame::OHCodecFrame() noexcept = default;

OHCodecFrame::~OHCodecFrame()
{
    abandon();
}

OHCodecFrame::OHCodecFrame(OHCodecFrame&& other) noexcept
    : source_(std::move(other.source_))
    , buffer_(std::exchange(other.buffer_, 0))
    , generation_(std::exchange(other.generation_, 0))
    , timestamp_(std::exchange(other.timestamp_, 0))
    , decided_(std::exchange(other.decided_, true))
{
}

OHCodecFrame& OHCodecFrame::operator=(OHCodecFrame&& other) noexcept
{
    if (this != &other) {
        abandon();
        source_ = std::move(other.source_);
        buffer_ = std::exchange(other.buffer_, 0);
        generation_ = std::exchange(other.generation_, 0);
        timestamp_ = std::exchange(other.timestamp_, 0);
        decided_ = std::exchange(other.decided_, true);
    }
    return *this;
}

OHCodecFrame::OHCodecFrame(
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

OHCodecFrame::operator bool() const noexcept
{
    return isValid();
}

bool OHCodecFrame::isValid() const noexcept
{
    return source_
        && source_.deviceType() == HardwareDeviceType::OHCodec
        && buffer_ != 0 && generation_ != 0;
}

bool OHCodecFrame::isPending() const noexcept
{
    return isValid() && !decided_;
}

std::uint32_t OHCodecFrame::surfaceGeneration() const noexcept
{
    return generation_;
}

std::int64_t OHCodecFrame::timestamp() const noexcept
{
    return timestamp_;
}

const HardwareFrame& OHCodecFrame::sourceFrame() const noexcept
{
    return source_;
}

bool OHCodecFrame::present() noexcept
{
    return presentResult() == OHCodecFrameDecisionResult::Applied;
}

OHCodecFrameDecisionResult OHCodecFrame::presentResult() noexcept
{
    if (!isPending()) {
        return OHCodecFrameDecisionResult::Failed;
    }
    decided_ = true;
    const int result =
        av_ohcodec_release_buffer(nativeBuffer(buffer_), 1);
    if (result == AVERROR(EALREADY)) {
        return OHCodecFrameDecisionResult::AlreadyDecided;
    }
    return result >= 0
        ? OHCodecFrameDecisionResult::Applied
        : OHCodecFrameDecisionResult::Failed;
}

bool OHCodecFrame::presentAt(
    std::int64_t monotonicNanoseconds) noexcept
{
    return presentAtResult(monotonicNanoseconds)
        == OHCodecFrameDecisionResult::Applied;
}

OHCodecFrameDecisionResult OHCodecFrame::presentAtResult(
    std::int64_t monotonicNanoseconds) noexcept
{
    if (!isPending() || monotonicNanoseconds <= 0) {
        return OHCodecFrameDecisionResult::Failed;
    }
    decided_ = true;
    const int result = av_ohcodec_render_buffer_at_time(
        nativeBuffer(buffer_),
        monotonicNanoseconds);
    if (result == AVERROR(EALREADY)) {
        return OHCodecFrameDecisionResult::AlreadyDecided;
    }
    return result >= 0
        ? OHCodecFrameDecisionResult::Applied
        : OHCodecFrameDecisionResult::Failed;
}

bool OHCodecFrame::drop() noexcept
{
    return dropResult() == OHCodecFrameDecisionResult::Applied;
}

OHCodecFrameDecisionResult OHCodecFrame::dropResult() noexcept
{
    if (!isPending()) {
        return OHCodecFrameDecisionResult::Failed;
    }
    decided_ = true;
    const int result =
        av_ohcodec_release_buffer(nativeBuffer(buffer_), 0);
    if (result == AVERROR(EALREADY)) {
        return OHCodecFrameDecisionResult::AlreadyDecided;
    }
    return result >= 0
        ? OHCodecFrameDecisionResult::Applied
        : OHCodecFrameDecisionResult::Failed;
}

void OHCodecFrame::abandon() noexcept
{
    if (isPending()) {
        drop();
    }
}

OHCodecFrame ohCodecFrame(
    const VideoFrame& frame,
    const OHCodecSurface& surface) noexcept
{
    if (!frame || !surface || !frame.hasHardwareFrame()) {
        return {};
    }
    HardwareFrame hardware = frame.hardwareFrame();
    if (!hardware
        || hardware.deviceType() != HardwareDeviceType::OHCodec) {
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

    return OHCodecFrame(
        std::move(hardware),
        output.value,
        output.subresource,
        frame.timestamp());
}

} // namespace qtav
