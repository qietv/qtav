// SPDX-License-Identifier: LGPL-2.1-or-later

#include <qtav/qtav.h>

#include <array>
#include <cassert>
#include <cstdint>
#include <memory>
#include <utility>

namespace {

class MockMapping final : public qtav::HardwareFrameMapping {
public:
    int width() const noexcept override { return 2; }
    int height() const noexcept override { return 2; }
    qtav::PixelFormat format() const noexcept override
    {
        return qtav::PixelFormat::RGBA;
    }
    int planeCount() const noexcept override { return 1; }
    const std::uint8_t* data(int plane) const noexcept override
    {
        return plane == 0 ? pixels_.data() : nullptr;
    }
    std::uint8_t* writableData(int plane) noexcept override
    {
        return plane == 0 ? pixels_.data() : nullptr;
    }
    int lineSize(int plane) const noexcept override
    {
        return plane == 0 ? 8 : 0;
    }

private:
    std::array<std::uint8_t, 16> pixels_ {};
};

class MockHardwareFrameData final : public qtav::HardwareFrameData {
public:
    qtav::HardwareDeviceType deviceType() const noexcept override
    {
        return qtav::HardwareDeviceType::Metal;
    }
    int width() const noexcept override { return 2; }
    int height() const noexcept override { return 2; }
    qtav::PixelFormat softwareFormat() const noexcept override
    {
        return qtav::PixelFormat::RGBA;
    }
    qtav::NativeHandle nativeHandle(
        qtav::HardwareHandleType type) const noexcept override
    {
        return { type, type == qtav::HardwareHandleType::Texture ? 0x1234U : 0 };
    }
    bool isMappable(qtav::HardwareMapMode mode) const noexcept override
    {
        return mode == qtav::HardwareMapMode::Read;
    }
    std::shared_ptr<qtav::HardwareFrameMapping> map(
        qtav::HardwareMapMode mode) const override
    {
        return isMappable(mode) ? std::make_shared<MockMapping>() : nullptr;
    }
};

class MockInterop final : public qtav::HardwareFrameInterop {
public:
    qtav::HardwareInteropCapabilities capabilities() const override
    {
        return {
            { qtav::HardwareDeviceType::Metal },
            qtav::HardwareDeviceType::Metal,
            true,
            true,
        };
    }
    bool supports(const qtav::HardwareFrame& frame) const noexcept override
    {
        return frame.deviceType() == qtav::HardwareDeviceType::Metal;
    }
    qtav::HardwareFrame importFrame(
        const qtav::HardwareFrame& frame) override
    {
        return supports(frame) ? frame : qtav::HardwareFrame {};
    }
};

class MockVideoRenderAPI final : public qtav::VideoRenderAPI {
public:
    qtav::VideoRenderCapabilities capabilities() const override
    {
        return {
            { qtav::PixelFormat::RGBA },
            { qtav::HardwareDeviceType::Metal },
            true,
            true,
            false,
            false,
            false,
        };
    }
    void setEventCallback(EventCallback callback) override
    {
        callback_ = std::move(callback);
    }
    bool open(const qtav::VideoRenderConfig& config) override
    {
        config_ = config;
        open_ = config.surfaceSize.isValid();
        return open_;
    }
    bool configure(const qtav::VideoRenderConfig& config) override
    {
        config_ = config;
        return open_;
    }
    bool render(const qtav::VideoFrame&) override
    {
        return open_;
    }
    void close() noexcept override { open_ = false; }

    void requestRedraw()
    {
        if (callback_) {
            callback_({ qtav::VideoRenderEventType::RedrawRequested, {} });
        }
    }

private:
    EventCallback callback_;
    qtav::VideoRenderConfig config_;
    bool open_ = false;
};

class MockAudioSink final : public qtav::AudioSink {
public:
    qtav::AudioSinkCapabilities capabilities() const override
    {
        return {
            { qtav::SampleFormat::Float },
            8'000,
            192'000,
            8,
            true,
            true,
        };
    }
    void setEventCallback(EventCallback callback) override
    {
        callback_ = std::move(callback);
    }
    qtav::AudioSinkOpenResult open(
        const qtav::AudioFormat& decodedFormat) override
    {
        open_ = decodedFormat.isValid();
        return { open_, decodedFormat, open_ ? "" : "invalid format" };
    }
    void close() noexcept override { open_ = false; }
    void pause(bool paused) override { paused_ = paused; }
    void flush() override { flushed_ = true; }
    bool write(const qtav::AudioBufferView& buffer) override
    {
        return open_ && !paused_ && buffer.isValid();
    }
    qtav::AudioSinkClock clock() const noexcept override
    {
        return { open_, 125, 20 };
    }

    void reportUnderrun()
    {
        if (callback_) {
            callback_({ qtav::AudioSinkEventType::Underrun, {} });
        }
    }
    bool wasFlushed() const noexcept { return flushed_; }

private:
    EventCallback callback_;
    bool open_ = false;
    bool paused_ = false;
    bool flushed_ = false;
};

} // namespace

int main()
{
    qtav::HardwareFrame hardware(
        std::make_shared<MockHardwareFrameData>());
    assert(hardware);
    assert(hardware.width() == 2);
    assert(hardware.height() == 2);
    assert(hardware.softwareFormat() == qtav::PixelFormat::RGBA);
    assert(hardware.nativeHandle(qtav::HardwareHandleType::Texture).value
        == 0x1234U);
    assert(hardware.isMappable());
    const auto mapping = hardware.map();
    assert(mapping);
    assert(mapping->planeCount() == 1);
    assert(mapping->data(0));

    MockInterop interop;
    assert(interop.capabilities().zeroCopy);
    assert(interop.supports(hardware));
    assert(interop.importFrame(hardware));

    qtav::Player player;
    bool redrawRequested = false;
    MockVideoRenderAPI video;
    video.setEventCallback([&](const qtav::VideoRenderEvent& event) {
        redrawRequested =
            event.type == qtav::VideoRenderEventType::RedrawRequested;
        player.setState(qtav::State::Stopped);
    });
    qtav::VideoRenderConfig renderConfig;
    renderConfig.surfaceSize = { 1920, 1080 };
    renderConfig.viewport = { 0, 0, 1920, 1080 };
    assert(video.open(renderConfig));
    video.requestRedraw();
    assert(redrawRequested);
    video.close();

    bool underrunReported = false;
    MockAudioSink audio;
    audio.setEventCallback([&](const qtav::AudioSinkEvent& event) {
        underrunReported = event.type == qtav::AudioSinkEventType::Underrun;
        player.setState(qtav::State::Stopped);
    });
    const qtav::AudioFormat decoded {
        48'000,
        2,
        qtav::SampleFormat::Float,
        "stereo",
    };
    const auto opened = audio.open(decoded);
    assert(opened.success);
    assert(opened.deviceFormat.sampleRate == 48'000);
    assert(audio.clock().valid);
    std::array<std::uint8_t, 8> samples {};
    const qtav::AudioBufferView buffer {
        decoded,
        1,
        { samples.data() },
        { static_cast<int>(samples.size()) },
        0,
        1,
    };
    assert(audio.write(buffer));
    assert(!qtav::audioBufferView(qtav::AudioFrame {}).isValid());
    audio.flush();
    assert(audio.wasFlushed());
    audio.reportUnderrun();
    assert(underrunReported);
    audio.close();
    return 0;
}
