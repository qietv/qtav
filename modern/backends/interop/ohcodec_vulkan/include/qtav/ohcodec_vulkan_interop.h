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

enum class OHCodecVulkanExternalFormatProbeMode {
    Disabled,
    // Diagnostic only: reinterpret a recognized opaque externalFormat as its
    // numerically identical explicit VkFormat, then sample it through the
    // renderer's native Vulkan normalization shader.
    ForcedVkFormatNativeSampling,
    // Diagnostic only: reinterpret the externalFormat as an explicit VkFormat
    // and expose it directly to pl_vulkan_wrap/libplacebo.
    ForcedVkFormatLibplacebo,
};

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
    // Required when vkGetNativeBufferPropertiesOHOS exposes an opaque
    // externalFormat and Vulkan must create a sampler YCbCr conversion.
    bool samplerYcbcrConversionEnabled = false;
    // Diagnostic override. Disabled means use the production policy below.
    OHCodecVulkanExternalFormatProbeMode externalFormatProbeMode =
        OHCodecVulkanExternalFormatProbeMode::Disabled;
    // Legacy diagnostic compatibility only. externalFormat is an opaque,
    // implementation-defined identifier and must not normally be reinterpreted
    // as VkFormat even when the numeric values happen to match. The production
    // default uses VkExternalFormatOHOS. Enabling this option requests the
    // historical, runtime-gated explicit-format reinterpretation for a closed
    // allow-list; applications must not treat success as a portable contract.
    bool externalFormatWorkaroundEnabled = false;
};

struct QTAV_INTEROP_OHCODEC_VULKAN_EXPORT
OHCodecVulkanInteropStatistics {
    std::uint64_t nativeBuffersAcquired = 0;
    std::uint64_t nativeBuffersImported = 0;
    std::uint64_t directPlaneImports = 0;
    std::uint64_t opaqueExternalImports = 0;
    std::uint64_t forcedVkFormatImports = 0;
    std::uint64_t forcedVkFormatNativeSamples = 0;
    std::uint64_t forcedVkFormatLibplaceboImports = 0;
    std::uint64_t codecOutputsQueued = 0;
    std::uint64_t frameAvailableCallbacks = 0;
    std::uint64_t acquireFencesImported = 0;
    std::uint64_t opaqueExternalObjectProbes = 0;
    std::uint64_t opaqueExternalObjectProbeSuccesses = 0;
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
    VkFormat lastForcedVulkanFormat = VK_FORMAT_UNDEFINED;
    std::uint64_t lastExternalFormat = 0;
    std::uint64_t externalFormatWorkaroundImports = 0;
};

// Last acquired decoder allocation and Vulkan format observations. This
// separate additive API preserves the ABI of OHCodecVulkanInteropStatistics.
// The values diagnose the strict explicit-plane hardware gate; they do not
// make an opaque externalFormat a portable VkFormat contract.
struct QTAV_INTEROP_OHCODEC_VULKAN_EXPORT
OHCodecVulkanNativeBufferObservation {
    std::int32_t nativeWidth = 0;
    std::int32_t nativeHeight = 0;
    std::int32_t nativeStride = 0;
    std::uint64_t nativeUsage = 0;
    std::int32_t nativeColorSpace = 0;
    std::int32_t nativeColorSpaceResult = 0;
    std::uint64_t formatFeatures = 0;
    std::uint64_t optimalTilingFeatures = 0;
    std::uint64_t allocationSize = 0;
    std::uint32_t memoryTypeBits = 0;
};

// Presents one OHCodec output into a private OH_ConsumerSurface, acquires the
// exact queued OHNativeWindowBuffer, and imports its OH_NativeBuffer through
// VK_OHOS_external_memory. Explicit two- or three-plane Vulkan formats can be
// wrapped by libplacebo directly. Opaque external formats expose both the
// driver's suggested display conversion and an identity raw-Y/Cb/Cr
// conversion/view/sampler pair. The renderer uses the identity pair for Dolby
// Vision before libplacebo performs its reshape and color processing.
// The acquired consumer buffer remains retained until VulkanVideoRenderer's
// GPU completion timeline retires the returned texture frame.
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
    void invalidatePendingFrames() noexcept override;
    void completePendingFrameInvalidation() noexcept override;

    OHCodecVulkanInteropStatistics statistics() const noexcept;
    OHCodecVulkanNativeBufferObservation
    nativeBufferObservation() const noexcept;

private:
    class Impl;
    std::unique_ptr<Impl> impl_;
};

} // namespace qtav
