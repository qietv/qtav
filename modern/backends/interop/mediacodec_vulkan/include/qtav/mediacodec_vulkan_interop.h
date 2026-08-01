// SPDX-License-Identifier: LGPL-2.1-or-later
#pragma once

#if !defined(__ANDROID__)
#  error "qtav/mediacodec_vulkan_interop.h is available only on Android"
#endif

#include <cstdint>
#include <memory>
#include <string>

#include <qtav/mediacodec_hardware_decoder.h>
#include <qtav/mediacodec_vulkan_export.h>
#include <qtav/vulkan_video_renderer.h>

namespace qtav {

struct QTAV_INTEROP_MEDIACODEC_VULKAN_EXPORT
MediaCodecVulkanInteropConfig {
    // Optional default AImageReader dimensions. MediaCodec overrides these
    // with the decoded output size; non-positive values use a 1x1 default.
    // Imported frames always use their actual AImage dimensions and crop.
    int width = 0;
    int height = 0;
    // Must cover the renderer's in-flight ring plus at least one producer
    // buffer. Values are clamped to [4, 16].
    int maximumImages = 5;

    // These flags confirm that the application enabled the corresponding
    // device extensions or Vulkan 1.1 core capabilities before creating the
    // borrowed device. The interop refuses to run when any required contract
    // is not explicitly confirmed.
    bool androidHardwareBufferExternalMemoryEnabled = false;
    bool externalSemaphoreFdEnabled = false;
    bool samplerYcbcrConversionEnabled = false;
    bool foreignQueueFamilyEnabled = false;
};

struct QTAV_INTEROP_MEDIACODEC_VULKAN_EXPORT
MediaCodecVulkanInteropStatistics {
    std::uint64_t codecOutputsQueued = 0;
    std::uint64_t imagesAcquired = 0;
    std::uint64_t imagesImported = 0;
    std::uint64_t acquireFencesImported = 0;
    std::uint64_t releaseFencesReturned = 0;
    std::uint64_t releaseFenceFallbacks = 0;
    std::uint64_t staleImagesDropped = 0;
    std::uint64_t maximumPendingImages = 0;
    // imagesImported counts frame imports. The following counters describe
    // the persistent Vulkan image/memory/view cache keyed by AHardwareBuffer.
    std::uint64_t hardwareBufferImports = 0;
    std::uint64_t hardwareBufferImportCacheHits = 0;
    std::uint64_t hardwareBufferImportsRemoved = 0;
    std::uint64_t maximumCachedHardwareBufferImports = 0;
    std::uint64_t cpuMapCalls = 0;
    std::uint64_t softwareTransferCalls = 0;
    std::uint64_t stagingCopies = 0;
    std::uint64_t rendererUploads = 0;
    std::uint32_t lastHardwareBufferFormat = 0;
    VkFormat lastVulkanFormat = VK_FORMAT_UNDEFINED;
    std::uint64_t lastExternalFormat = 0;
};

// On Android API 26 or newer, owns a private AImageReader whose GPU-sampled
// ANativeWindow is supplied to FFmpeg's MediaCodec wrapper. Decoded outputs
// are timestamp-correlated with asynchronously acquired AImages, imported
// through
// VK_ANDROID_external_memory_android_hardware_buffer, and returned with a
// Vulkan-exported sync fd. No decoded pixel is CPU-mapped or uploaded.
class QTAV_INTEROP_MEDIACODEC_VULKAN_EXPORT
MediaCodecVulkanInterop final : public VulkanHardwareFrameInterop {
public:
    MediaCodecVulkanInterop(
        BorrowedVulkanDevice device,
        MediaCodecVulkanInteropConfig config);
    ~MediaCodecVulkanInterop() override;

    MediaCodecVulkanInterop(MediaCodecVulkanInterop&&) noexcept;
    MediaCodecVulkanInterop& operator=(
        MediaCodecVulkanInterop&&) noexcept;
    MediaCodecVulkanInterop(
        const MediaCodecVulkanInterop&) = delete;
    MediaCodecVulkanInterop& operator=(
        const MediaCodecVulkanInterop&) = delete;

    explicit operator bool() const noexcept;
    bool isValid() const noexcept;
    std::string lastError() const;
    MediaCodecSurface surface() const noexcept;

    BorrowedVulkanDevice device() const noexcept override;
    HardwareInteropCapabilities capabilities() const override;
    bool supports(const HardwareFrame& frame) const noexcept override;
    // Registers the frame/timestamp association, releases its MediaCodec
    // output into the private AImageReader, and waits for at most 100 ms for
    // the matching asynchronous image acquisition. The application may retain
    // the VideoFrame and render it later at its playback deadline.
    bool queueFrame(const VideoFrame& frame, std::string& detail);
    VulkanHardwareImportStatus prepareFrame(
        const VideoFrame& frame,
        std::string& detail) override;
    VulkanHardwareImportResult importFrame(
        const VideoFrame& frame) override;
    void setFrameAvailableCallback(
        FrameAvailableCallback callback) override;

    // Drop images and timestamp associations that have not entered a Vulkan
    // submission. Call before seek, decoder replacement, or explicit stop.
    void flush() noexcept;
    MediaCodecVulkanInteropStatistics statistics() const noexcept;

private:
    class Impl;
    std::unique_ptr<Impl> impl_;
};

} // namespace qtav
