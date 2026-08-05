// SPDX-License-Identifier: LGPL-2.1-or-later
#pragma once

#if !defined(__OHOS__)
#  error "qtav/ohcodec_vulkan_interop.h is OHOS-only"
#endif

#include <cstdint>
#include <memory>
#include <string>

#include <qtav/ohcodec_hardware_decoder.h>
#include <qtav/ohcodec_vulkan_export.h>
#include <qtav/vulkan_video_renderer.h>

namespace qtav {

struct QTAV_INTEROP_OHCODEC_VULKAN_EXPORT
OHCodecVulkanInteropConfig {
    // Optional default consumer-surface dimensions. OHCodec may replace these
    // with the coded output size; non-positive values keep platform defaults.
    int width = 0;
    int height = 0;
    // The application must explicitly confirm that these extensions were
    // enabled when it created the borrowed logical device.
    bool ohosExternalMemoryEnabled = false;
    bool foreignQueueFamilyEnabled = false;
    bool syncFdSemaphoreEnabled = false;
};

struct QTAV_INTEROP_OHCODEC_VULKAN_EXPORT
OHCodecVulkanInteropStatistics {
    std::uint64_t nativeBuffersAcquired = 0;
    std::uint64_t nativeBuffersImported = 0;
    std::uint64_t directPlaneImports = 0;
    std::uint64_t codecOutputsQueued = 0;
    std::uint64_t frameAvailableCallbacks = 0;
    std::uint64_t acquireFencesImported = 0;
    std::uint64_t opaqueFormatsRejected = 0;
    std::uint64_t unsupportedFormatsRejected = 0;
    std::uint64_t outputsReleasedAfterGpu = 0;
    std::uint64_t cpuMapCalls = 0;
    std::uint64_t softwareTransferCalls = 0;
    std::uint64_t stagingCopies = 0;
    std::uint64_t rendererUploads = 0;
    std::uint64_t normalizationPasses = 0;
    std::int32_t lastNativeFormat = 0;
    VkFormat lastVulkanFormat = VK_FORMAT_UNDEFINED;
    std::uint64_t lastExternalFormat = 0;
};

// Presents one OHCodec output into a private OH_ConsumerSurface, acquires the
// exact queued OHNativeWindowBuffer, and imports its OH_NativeBuffer through
// VK_OHOS_external_memory. Only explicit two- or three-plane Vulkan formats
// accepted by libplacebo are exposed; opaque external formats are reported as
// unsupported rather than normalized or copied. The acquired consumer buffer
// remains retained until VulkanVideoRenderer's GPU completion timeline retires
// the returned texture frame.
class QTAV_INTEROP_OHCODEC_VULKAN_EXPORT
OHCodecVulkanInterop final : public VulkanHardwareFrameInterop {
public:
    OHCodecVulkanInterop(
        BorrowedVulkanDevice device,
        OHCodecVulkanInteropConfig config);
    ~OHCodecVulkanInterop() override;

    OHCodecVulkanInterop(OHCodecVulkanInterop&&) noexcept;
    OHCodecVulkanInterop& operator=(OHCodecVulkanInterop&&) noexcept;
    OHCodecVulkanInterop(const OHCodecVulkanInterop&) = delete;
    OHCodecVulkanInterop& operator=(const OHCodecVulkanInterop&) = delete;

    explicit operator bool() const noexcept;
    bool isValid() const noexcept;
    std::string lastError() const;
    OHCodecSurface surface() const noexcept;

    BorrowedVulkanDevice device() const noexcept override;
    HardwareInteropCapabilities capabilities() const override;
    bool supports(const HardwareFrame& frame) const noexcept override;
    VulkanHardwareImportStatus prepareFrame(
        const VideoFrame& frame,
        std::string& detail) override;
    VulkanHardwareImportResult importFrame(
        const VideoFrame& frame) override;
    void setFrameAvailableCallback(
        FrameAvailableCallback callback) override;

    OHCodecVulkanInteropStatistics statistics() const noexcept;

private:
    class Impl;
    std::unique_ptr<Impl> impl_;
};

} // namespace qtav
