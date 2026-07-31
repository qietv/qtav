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
    JavaVM* javaVM = nullptr;
    int width = 0;
    int height = 0;
    // Bounds MediaCodec outputs released but not yet matched to the
    // SurfaceTexture's single current image. Values are clamped to [2, 16].
    int maximumPendingFrames = 4;
    // A native retry worker only schedules redraws. SurfaceTexture and GL
    // calls remain on the renderer thread with its context current.
    int redrawRetryMilliseconds = 2;
    // Keep false unless the selected Android device, codec, GL driver, and
    // output path have independently verified P010/HDR external-OES sampling
    // with the required color control. SDR 8-bit output remains available.
    bool hdrExternalOesSamplingEnabled = false;
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
};

// Owns a detached android.graphics.SurfaceTexture and exposes its producer
// ANativeWindow to FFmpeg's MediaCodec wrapper. With the renderer's OpenGL ES
// context current, decoded outputs are released to the producer, latched by
// timestamp through ASurfaceTexture_updateTexImage(), and sampled directly as
// GL_TEXTURE_EXTERNAL_OES. The current image remains retained by
// SurfaceTexture until the next update; decoded pixels are never CPU-mapped
// or uploaded.
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
    OpenGLHardwareImportResult prepareFrame(
        const VideoFrame& frame) override;
    void releaseCurrentContextResources() noexcept override;
    void setFrameAvailableCallback(
        FrameAvailableCallback callback) override;

    // Reject pending timestamp associations before seek, loop, decoder
    // replacement, explicit stop, or renderer fallback. The SurfaceTexture
    // generation remains valid until this object is replaced.
    void flush() noexcept;
    MediaCodecOpenGLInteropStatistics statistics() const noexcept;

private:
    class Impl;
    std::unique_ptr<Impl> impl_;
};

} // namespace qtav
