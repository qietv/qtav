// SPDX-License-Identifier: LGPL-2.1-or-later
#pragma once

#if !defined(__OHOS__)
#  error "qtav/ohcodec_opengl_interop.h is OHOS-only"
#endif

#include <cstdint>
#include <memory>
#include <string>

#include <qtav/ohcodec_hardware_decoder.h>
#include <qtav/ohcodec_opengl_export.h>
#include <qtav/opengl_video_renderer.h>

namespace qtav {

struct QTAV_INTEROP_OHCODEC_OPENGL_EXPORT OHCodecOpenGLInteropConfig {
    // Bounds timestamp associations awaiting OH_NativeImage notification and
    // current-context latching. Values are clamped to [1, 16].
    int maximumPendingFrames = 4;
};

struct QTAV_INTEROP_OHCODEC_OPENGL_EXPORT
OHCodecOpenGLInteropStatistics {
    std::uint64_t codecOutputsQueued = 0;
    std::uint64_t surfaceImagesUpdated = 0;
    std::uint64_t frameAvailableSignals = 0;
    std::uint64_t redrawSignals = 0;
    std::uint64_t transformQueries = 0;
    std::uint64_t timestampMatches = 0;
    // FFmpeg-attached RPU metadata stayed on the exact queued VideoFrame,
    // survived normalized native-image timestamp matching, and remained
    // present until the rendered image was released.
    std::uint64_t dolbyVisionFramesQueued = 0;
    std::uint64_t dolbyVisionTimestampMatches = 0;
    std::uint64_t dolbyVisionFramesReleased = 0;
    // OHCodec surface producers may publish the AVCodec microsecond PTS
    // directly. These observations were normalized to nanoseconds before
    // exact frame association.
    std::uint64_t microsecondTimestampsNormalized = 0;
    std::uint64_t staleFramesDropped = 0;
    std::uint64_t maximumPendingFrames = 0;
    std::uint64_t contextAttachments = 0;
    std::uint64_t contextDetaches = 0;
    std::uint64_t framesReleased = 0;
    std::uint64_t unsupportedFrames = 0;
    // Images sampled as raw normalized Y/Cb/Cr through GL_EXT_YUV_target.
    // The renderer performs one GPU representation-normalization pass before
    // libplacebo; this is zero CPU copy, not strict no-intermediate zero-copy.
    std::uint64_t rawYcbcrImages = 0;
    std::uint64_t implicitRgbImages = 0;
    std::uint64_t cpuMapCalls = 0;
    std::uint64_t softwareTransferCalls = 0;
    std::uint64_t stagingCopies = 0;
    std::uint64_t rendererUploads = 0;
    std::int64_t lastTimestampNanoseconds = 0;
    std::uint32_t textureName = 0;
    std::uint32_t surfaceGeneration = 0;
};

// Owns an OH_NativeImage attached to a current-context
// GL_TEXTURE_EXTERNAL_OES texture and exposes its producer surface to
// FFmpeg's OHCodec wrapper. The decoder output is latched only after the
// frame-available callback, on the renderer's GL thread. The path requires
// GL_EXT_YUV_target and exposes normalized raw Y/Cb/Cr. A small GPU pass
// applies the producer transform and preserves those components in RGBA16F;
// libplacebo remains the sole owner of YUV conversion, supported Dolby Vision
// reshaping when FFmpeg supplies RPU metadata, gamut/tone mapping, scaling,
// and output encoding. Decoded pixels are never
// CPU-mapped, transferred, staged, or uploaded. Because the raw components use
// an intermediate GPU texture, this is zero CPU copy but not strict
// no-intermediate source zero-copy; the OHOS Vulkan native-buffer path is the
// preferred route when its imported format can be wrapped directly.
class QTAV_INTEROP_OHCODEC_OPENGL_EXPORT OHCodecOpenGLInterop final
    : public OpenGLHardwareFrameInterop {
public:
    explicit OHCodecOpenGLInterop(
        OHCodecOpenGLInteropConfig config = {});
    ~OHCodecOpenGLInterop() override;

    OHCodecOpenGLInterop(OHCodecOpenGLInterop&&) noexcept;
    OHCodecOpenGLInterop& operator=(OHCodecOpenGLInterop&&) noexcept;
    OHCodecOpenGLInterop(const OHCodecOpenGLInterop&) = delete;
    OHCodecOpenGLInterop& operator=(const OHCodecOpenGLInterop&) = delete;

    explicit operator bool() const noexcept;
    bool isValid() const noexcept;
    std::string lastError() const;
    OHCodecSurface surface() const noexcept;

    bool initializeCurrentContext(std::string& detail) override;
    HardwareInteropCapabilities capabilities() const override;
    bool supports(const HardwareFrame& frame) const noexcept override;
    // Registers one output/timestamp association, then presents that exact
    // output once into the private OH_NativeImage producer surface. No GL or
    // OH_NativeImage consumer API is called from this method.
    bool queueFrame(const VideoFrame& frame, std::string& detail);
    OpenGLHardwareImportResult prepareFrame(
        const VideoFrame& frame) override;
    bool releaseFrame(
        const OpenGLExternalTextureFrame& frame,
        std::string& detail) noexcept override;
    void releaseCurrentContextResources() noexcept override;
    void setFrameAvailableCallback(
        FrameAvailableCallback callback) override;

    // Rejects pending timestamp associations across seek, decoder flush,
    // replacement, stop, or renderer fallback. The producer surface and its
    // generation remain valid until this interop object is destroyed.
    void flush() noexcept;
    OHCodecOpenGLInteropStatistics statistics() const noexcept;

private:
    class Impl;
    std::unique_ptr<Impl> impl_;
};

} // namespace qtav
