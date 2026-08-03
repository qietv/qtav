// SPDX-License-Identifier: LGPL-2.1-or-later
#pragma once

#if !defined(__ANDROID__)
#  error "qtav/mediacodec_opengl_interop.h is available only on Android"
#endif

#include <jni.h>

#include <cstdint>
#include <memory>
#include <string>

#include <qtav/mediacodec_hardware_decoder.h>
#include <qtav/mediacodec_opengl_export.h>
#include <qtav/opengl_video_renderer.h>

namespace qtav {

struct QTAV_INTEROP_MEDIACODEC_OPENGL_EXPORT
MediaCodecOpenGLInteropConfig {
    // Retained for source compatibility; the AImageReader path is native and
    // does not require Java or SurfaceTexture.
    JavaVM* javaVM = nullptr;
    // Optional default AImageReader dimensions. MediaCodec overrides them;
    // non-positive values use a 1x1 default.
    int width = 0;
    int height = 0;
    // Bounds timestamp-correlated PRIVATE images. Values are clamped to
    // [4, 16]; the reader reserves two additional acquisition slots for the
    // renderer and callback-drain ownership overlap.
    int maximumPendingFrames = 4;
    // Retained for source compatibility. AImageReader callbacks schedule
    // redraws directly.
    int redrawRetryMilliseconds = 2;
    // Deprecated and ignored compatibility switches. HDR and Dolby Vision now
    // require the raw GL_EXT_YUV_target import contract and never rely on
    // implicit SurfaceTexture color conversion or dataspace inference.
    bool hdrExternalOesSamplingEnabled = false;
    // Deprecated and ignored; retained only for source compatibility.
    bool autoDetectHdrExternalOesSampling = true;
};

enum class MediaCodecOpenGLHdrSamplingStatus {
    Disabled,
    Unchecked,
    Supported,
    Unsupported,
};

struct QTAV_INTEROP_MEDIACODEC_OPENGL_EXPORT
MediaCodecOpenGLInteropStatistics {
    std::uint64_t codecOutputsQueued = 0;
    std::uint64_t imagesLatched = 0;
    std::uint64_t textureAttachments = 0;
    std::uint64_t textureDetaches = 0;
    std::uint64_t textureUpdates = 0;
    std::uint64_t redrawSignals = 0;
    std::uint64_t staleFramesDropped = 0;
    std::uint64_t maximumPendingFrames = 0;
    std::int64_t lastTimestampNanoseconds = 0;
    std::uint32_t textureName = 0;
    std::uint64_t cpuMapCalls = 0;
    std::uint64_t softwareTransferCalls = 0;
    std::uint64_t stagingCopies = 0;
    std::uint64_t rendererUploads = 0;
    MediaCodecOpenGLHdrSamplingStatus hdrSamplingStatus =
        MediaCodecOpenGLHdrSamplingStatus::Disabled;
    std::int32_t lastDataSpace = 0;
    std::uint64_t acquireFencesWaited = 0;
    std::uint64_t releaseFencesReturned = 0;
    std::uint64_t releaseFenceFallbacks = 0;
    std::uint64_t rawYcbcrImports = 0;
    std::uint32_t lastHardwareBufferFormat = 0;
};

// Owns a private AImageReader and exposes its producer ANativeWindow to
// FFmpeg's MediaCodec wrapper. Timestamp-correlated PRIVATE AImages are
// imported as AHardwareBuffer-backed EGLImages and exposed as raw Y/Cb/Cr
// GL_TEXTURE_EXTERNAL_OES textures. The renderer returns each AImage with an
// EGL native-fence sync fd. Decoded pixels are never CPU-mapped or uploaded.
class QTAV_INTEROP_MEDIACODEC_OPENGL_EXPORT
MediaCodecOpenGLInterop final : public OpenGLHardwareFrameInterop {
public:
    explicit MediaCodecOpenGLInterop(
        MediaCodecOpenGLInteropConfig config);
    ~MediaCodecOpenGLInterop() override;

    MediaCodecOpenGLInterop(MediaCodecOpenGLInterop&&) noexcept;
    MediaCodecOpenGLInterop& operator=(
        MediaCodecOpenGLInterop&&) noexcept;
    MediaCodecOpenGLInterop(
        const MediaCodecOpenGLInterop&) = delete;
    MediaCodecOpenGLInterop& operator=(
        const MediaCodecOpenGLInterop&) = delete;

    explicit operator bool() const noexcept;
    bool isValid() const noexcept;
    std::string lastError() const;
    MediaCodecSurface surface() const noexcept;

    HardwareInteropCapabilities capabilities() const override;
    bool supports(const HardwareFrame& frame) const noexcept override;
    // Non-blockingly registers the frame/timestamp association and releases
    // its MediaCodec output into the private AImageReader. Image acquisition
    // completes asynchronously and schedules a redraw through the frame-
    // available callback. This performs no EGL or OpenGL work, so an
    // application may call it from the video scheduler, retain the exact
    // VideoFrame, and render later on the graphics thread at the supplied
    // playback deadline.
    bool queueFrame(const VideoFrame& frame, std::string& detail);
    OpenGLHardwareImportResult prepareFrame(
        const VideoFrame& frame) override;
    bool releaseFrame(
        const OpenGLExternalTextureFrame& frame,
        std::string& detail) noexcept override;
    void releaseCurrentContextResources() noexcept override;
    void setFrameAvailableCallback(
        FrameAvailableCallback callback) override;

    // Reject pending timestamp associations and acquired images before seek,
    // loop, decoder replacement, explicit stop, or renderer fallback. The
    // AImageReader producer generation remains valid until this object is
    // replaced.
    void flush() noexcept;
    MediaCodecOpenGLInteropStatistics statistics() const noexcept;

private:
    class Impl;
    std::unique_ptr<Impl> impl_;
};

} // namespace qtav
