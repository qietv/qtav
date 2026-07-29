// SPDX-License-Identifier: LGPL-2.1-or-later
#pragma once

#include <cstdint>
#include <memory>
#include <string>
#include <vector>

#include <qtav/export.h>
#include <qtav/format.h>

namespace qtav {

enum class HardwareDeviceType {
    Unknown,
    D3D11,
    VideoToolbox,
    VAAPI,
    MediaCodec,
    Vulkan,
    Metal,
};

enum class HardwareHandleType {
    Device,
    Context,
    Frame,
    Surface,
    Texture,
};

enum class HardwareMapMode {
    Read,
    Write,
    ReadWrite,
};

struct QTAV_CORE_EXPORT NativeHandle {
    HardwareHandleType type = HardwareHandleType::Frame;
    std::uintptr_t value = 0;

    explicit operator bool() const noexcept
    {
        return value != 0;
    }
};

class QTAV_CORE_EXPORT HardwareFrameMapping {
public:
    virtual ~HardwareFrameMapping();

    virtual int width() const noexcept = 0;
    virtual int height() const noexcept = 0;
    virtual PixelFormat format() const noexcept = 0;
    virtual int planeCount() const noexcept = 0;
    virtual const std::uint8_t* data(int plane) const noexcept = 0;
    virtual std::uint8_t* writableData(int plane) noexcept = 0;
    virtual int lineSize(int plane) const noexcept = 0;
};

class QTAV_CORE_EXPORT HardwareFrameData {
public:
    virtual ~HardwareFrameData();

    virtual HardwareDeviceType deviceType() const noexcept = 0;
    virtual int width() const noexcept = 0;
    virtual int height() const noexcept = 0;
    virtual PixelFormat softwareFormat() const noexcept = 0;
    virtual NativeHandle nativeHandle(HardwareHandleType type) const noexcept = 0;
    virtual bool isMappable(HardwareMapMode mode) const noexcept = 0;
    virtual std::shared_ptr<HardwareFrameMapping> map(
        HardwareMapMode mode) const = 0;
};

class QTAV_CORE_EXPORT HardwareFrame {
public:
    HardwareFrame() = default;
    explicit HardwareFrame(std::shared_ptr<const HardwareFrameData> data);

    explicit operator bool() const noexcept;
    bool isValid() const noexcept;
    HardwareDeviceType deviceType() const noexcept;
    int width() const noexcept;
    int height() const noexcept;
    PixelFormat softwareFormat() const noexcept;
    NativeHandle nativeHandle(HardwareHandleType type) const noexcept;
    bool isMappable(HardwareMapMode mode = HardwareMapMode::Read) const noexcept;
    std::shared_ptr<HardwareFrameMapping> map(
        HardwareMapMode mode = HardwareMapMode::Read) const;

private:
    std::shared_ptr<const HardwareFrameData> data_;
};

struct QTAV_CORE_EXPORT HardwareInteropCapabilities {
    std::vector<HardwareDeviceType> sourceDevices;
    HardwareDeviceType targetDevice = HardwareDeviceType::Unknown;
    bool zeroCopy = false;
    bool cpuFallback = false;
};

class QTAV_CORE_EXPORT HardwareFrameInterop {
public:
    virtual ~HardwareFrameInterop();

    virtual HardwareInteropCapabilities capabilities() const = 0;
    virtual bool supports(const HardwareFrame& frame) const noexcept = 0;
    virtual HardwareFrame importFrame(const HardwareFrame& frame) = 0;
};

} // namespace qtav
