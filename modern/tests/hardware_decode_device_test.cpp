// SPDX-License-Identifier: LGPL-2.1-or-later

#include <qtav/player.h>

#include <cassert>
#include <chrono>
#include <condition_variable>
#include <cstdint>
#include <mutex>

#include "hardware_decode_device_internal.h"

extern "C" {
#include <libavutil/buffer.h>
#include <libavutil/hwcontext.h>
}

namespace {

AVHWDeviceType ffmpegDeviceType(qtav::HardwareDeviceType type)
{
    switch (type) {
    case qtav::HardwareDeviceType::D3D11:
        return AV_HWDEVICE_TYPE_D3D11VA;
    case qtav::HardwareDeviceType::VideoToolbox:
        return AV_HWDEVICE_TYPE_VIDEOTOOLBOX;
    default:
        return AV_HWDEVICE_TYPE_NONE;
    }
}

AVBufferRef* makeContext(qtav::HardwareDeviceType type)
{
    auto* context = av_buffer_allocz(sizeof(AVHWDeviceContext));
    assert(context);
    auto* deviceContext =
        reinterpret_cast<AVHWDeviceContext*>(context->data);
    deviceContext->type = ffmpegDeviceType(type);
    return context;
}

qtav::HardwareDecodeDevice makeDevice(
    qtav::HardwareDeviceType type,
    std::uintptr_t identity)
{
    auto* context = makeContext(type);
    const auto device =
        qtav::detail::HardwareDecodeDevicePrivate::create(
            type,
            identity,
            context);
    av_buffer_unref(&context);
    return device;
}

void testTokenLifetimeAndIdentity()
{
    const qtav::HardwareDecodeDevice empty;
    assert(!empty);
    assert(!empty.isValid());
    assert(empty.deviceType() == qtav::HardwareDeviceType::Unknown);
    assert(empty.nativeIdentity() == 0);

    auto* context = makeContext(qtav::HardwareDeviceType::D3D11);
    assert(!qtav::detail::HardwareDecodeDevicePrivate::create(
        qtav::HardwareDeviceType::Unknown,
        1,
        context));
    assert(!qtav::detail::HardwareDecodeDevicePrivate::create(
        qtav::HardwareDeviceType::VideoToolbox,
        1,
        context));
    assert(!qtav::detail::HardwareDecodeDevicePrivate::create(
        qtav::HardwareDeviceType::D3D11,
        1,
        nullptr));

    auto device = qtav::detail::HardwareDecodeDevicePrivate::create(
        qtav::HardwareDeviceType::D3D11,
        0x1234,
        context);
    assert(device);
    assert(device.deviceType() == qtav::HardwareDeviceType::D3D11);
    assert(device.nativeIdentity() == 0x1234);
    assert(av_buffer_get_ref_count(context) == 2);

    const auto copy = device;
    assert(copy == device);
    assert(av_buffer_get_ref_count(context) == 2);

    auto* contextCopy =
        qtav::detail::HardwareDecodeDevicePrivate::contextRef(copy);
    assert(contextCopy);
    assert(contextCopy->buffer == context->buffer);
    assert(av_buffer_get_ref_count(context) == 3);
    av_buffer_unref(&contextCopy);

    const auto secondToken =
        qtav::detail::HardwareDecodeDevicePrivate::create(
            qtav::HardwareDeviceType::D3D11,
            0x1234,
            context);
    assert(secondToken);
    assert(secondToken != device);

    av_buffer_unref(&context);
    assert(device);
    auto* retainedContext =
        qtav::detail::HardwareDecodeDevicePrivate::contextRef(device);
    assert(retainedContext);
    av_buffer_unref(&retainedContext);

    qtav::HardwareDecodeConfig config {
        qtav::HardwareDeviceType::D3D11,
        false,
        device,
        7,
        true,
    };
    assert(config.isValid());
    assert(config.device == device);

    qtav::Player player;
    player.setHardwareDecodeConfig(config);
    const auto copiedConfig = player.hardwareDecodeConfig();
    assert(copiedConfig.deviceType == qtav::HardwareDeviceType::D3D11);
    assert(!copiedConfig.allowSoftwareFallback);
    assert(copiedConfig.device == device);
    assert(copiedConfig.extraHardwareFrames == 7);
    assert(copiedConfig.requireSuppliedDevice);
}

void testMismatchedSuppliedDeviceFallsBack(const char* media)
{
    qtav::Player player;
    std::mutex mutex;
    std::condition_variable changed;
    bool fallbackReported = false;
    bool frameReceived = false;
    bool stopped = false;

    player
        .setHardwareDecodeConfig({
            qtav::HardwareDeviceType::D3D11,
            true,
            makeDevice(qtav::HardwareDeviceType::VideoToolbox, 0x5678),
        })
        .onEvent([&](const qtav::MediaEvent& event) {
            if (event.category == "decoder.hardware.fallback") {
                std::lock_guard<std::mutex> lock(mutex);
                fallbackReported = true;
            }
            return false;
        })
        .onStateChanged([&](qtav::State state) {
            if (state == qtav::State::Stopped) {
                {
                    std::lock_guard<std::mutex> lock(mutex);
                    stopped = true;
                }
                changed.notify_all();
            }
        })
        .onVideoFrame([&](const qtav::VideoFrame& frame, int) {
            assert(frame);
            assert(!frame.hasHardwareFrame());
            {
                std::lock_guard<std::mutex> lock(mutex);
                frameReceived = true;
            }
            player.setState(qtav::State::Stopped);
        });

    player.setMedia(media);
    player.setState(qtav::State::Playing);

    std::unique_lock<std::mutex> lock(mutex);
    assert(changed.wait_for(
        lock,
        std::chrono::seconds(5),
        [&] { return stopped; }));
    assert(fallbackReported);
    assert(frameReceived);
}

void testMismatchedSuppliedDeviceErrors(const char* media)
{
    qtav::Player player;
    std::mutex mutex;
    std::condition_variable changed;
    bool hardwareErrorReported = false;
    int frames = 0;

    player
        .setHardwareDecodeConfig({
            qtav::HardwareDeviceType::D3D11,
            false,
            makeDevice(qtav::HardwareDeviceType::VideoToolbox, 0x9abc),
        })
        .onEvent([&](const qtav::MediaEvent& event) {
            if (event.category == "decoder.hardware.error") {
                {
                    std::lock_guard<std::mutex> lock(mutex);
                    hardwareErrorReported = true;
                }
                changed.notify_all();
                player.setState(qtav::State::Stopped);
            }
            return false;
        })
        .onVideoFrame([&](const qtav::VideoFrame&, int) {
            std::lock_guard<std::mutex> lock(mutex);
            ++frames;
        });

    player.setMedia(media);
    player.setState(qtav::State::Playing);

    std::unique_lock<std::mutex> lock(mutex);
    assert(changed.wait_for(
        lock,
        std::chrono::seconds(5),
        [&] { return hardwareErrorReported; }));
    assert(hardwareErrorReported);
    assert(frames == 0);
}

} // namespace

int main(int argc, char** argv)
{
    assert(argc == 1 || argc == 2);
    testTokenLifetimeAndIdentity();
    if (argc == 2) {
        testMismatchedSuppliedDeviceFallsBack(argv[1]);
        testMismatchedSuppliedDeviceErrors(argv[1]);
    }
    return 0;
}
