// SPDX-License-Identifier: LGPL-2.1-or-later
#pragma once

#include <cstdint>
#include <memory>
#include <string>

#include <qtav/color.h>
#include <qtav/export.h>
#include <qtav/format.h>
#include <qtav/hardware_frame.h>

namespace qtav {
namespace detail {
struct FrameFactory;
}

class QTAV_CORE_EXPORT VideoFrame {
public:
    VideoFrame() = default;

    explicit operator bool() const noexcept;
    bool isValid() const noexcept;

    int width() const noexcept;
    int height() const noexcept;
    PixelFormat format() const noexcept;
    int nativeFormat() const noexcept;
    std::string formatName() const;
    int planeCount() const noexcept;
    const std::uint8_t* data(int plane = 0) const noexcept;
    int lineSize(int plane = 0) const noexcept;
    std::int64_t timestamp() const noexcept;
    std::int64_t duration() const noexcept;
    std::string colorSpace() const;
    VideoColorSpace colorSpaceInfo() const noexcept;
    MasteringDisplayMetadata masteringDisplayMetadata() const noexcept;
    ContentLightMetadata contentLightMetadata() const noexcept;
    bool hasDolbyVisionMetadata() const noexcept;
    bool hasHardwareFrame() const noexcept;
    HardwareFrame hardwareFrame() const;

private:
    struct Storage;
    explicit VideoFrame(std::shared_ptr<const Storage> storage);

    std::shared_ptr<const Storage> storage_;
    friend struct detail::FrameFactory;
};

class QTAV_CORE_EXPORT AudioFrame {
public:
    AudioFrame() = default;

    explicit operator bool() const noexcept;
    bool isValid() const noexcept;

    int sampleRate() const noexcept;
    int channels() const noexcept;
    int samplesPerChannel() const noexcept;
    SampleFormat format() const noexcept;
    int nativeFormat() const noexcept;
    std::string formatName() const;
    std::string channelLayout() const;
    int planeCount() const noexcept;
    const std::uint8_t* data(int plane = 0) const noexcept;
    int lineSize(int plane = 0) const noexcept;
    std::int64_t timestamp() const noexcept;
    std::int64_t duration() const noexcept;

private:
    struct Storage;
    explicit AudioFrame(std::shared_ptr<const Storage> storage);

    std::shared_ptr<const Storage> storage_;
    friend struct detail::FrameFactory;
};

} // namespace qtav
