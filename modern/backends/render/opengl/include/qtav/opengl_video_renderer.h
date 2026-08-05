// SPDX-License-Identifier: LGPL-2.1-or-later
#pragma once

#include <array>
#include <cstdint>
#include <functional>
#include <memory>
#include <string>

#include <qtav/opengl_export.h>
#include <qtav/video_render_api.h>

namespace qtav {

// PreferHdr selects native HDR when the platform adapter exposes it and falls
// back to SDR. RequireHdr rejects an SDR-only surface. SdrOnly always requests
// the deterministic HDR-to-SDR rendering path.
enum class OpenGLOutputPreference {
    PreferHdr,
    RequireHdr,
    SdrOnly,
};

// Defines the presentation encoding produced by OpenGLVideoRenderer. Platform
// adapters must ensure that the framebuffer and presentation surface use the
// same color space. For SdrSrgb the renderer queries the framebuffer attachment
// encoding and supplies linear BT.709 to an sRGB attachment or explicitly
// encodes sRGB for a linear attachment. HDR10PQ and HDR10HLG contain BT.2020
// primaries with the named transfer function.
enum class OpenGLOutputColorSpace {
    SdrSrgb,
    HDR10PQ,
    HDR10HLG,
};

// OpenGLVideoRenderer renders into the application-provided framebuffer of
// the OpenGL ES 3.x context that is current on the calling thread. Framebuffer
// zero is valid and represents the current default framebuffer.
struct QTAV_RENDER_OPENGL_EXPORT OpenGLRenderTarget {
    std::uint32_t framebuffer = 0;
    VideoSize size;
    std::uint64_t generation = 0;
    OpenGLOutputColorSpace colorSpace =
        OpenGLOutputColorSpace::SdrSrgb;

    bool isValid() const noexcept
    {
        return size.isValid();
    }

    bool isHdr() const noexcept;
};

QTAV_RENDER_OPENGL_EXPORT bool openGLColorSpaceIsHdr(
    OpenGLOutputColorSpace colorSpace) noexcept;

using OpenGLCurrentTargetCallback =
    std::function<OpenGLRenderTarget()>;
// Optional platform presentation hook. It runs with the caller's OpenGL ES
// context current after rendering has been submitted, but before a hardware
// source image is returned to its producer. Window-system adapters use this
// ordering so the interop release fence covers presentation work as well as
// sampling work.
using OpenGLPresentCallback =
    std::function<bool(std::string& detail)>;

enum class OpenGLHardwareImportStatus {
    Ready,
    Pending,
    Unsupported,
    Stale,
    Error,
};

// A current-context view of a native image sampled through
// GL_TEXTURE_EXTERNAL_OES. rawYcbcr requires GL_EXT_YUV_target and means that
// sampling yields normalized Y, Cb, and Cr in R, G, and B respectively. The
// renderer copies those raw components, with crop applied, to an internal
// floating-point texture before libplacebo performs any color processing.
struct QTAV_RENDER_OPENGL_EXPORT OpenGLExternalTextureFrame {
    std::uint32_t texture = 0;
    // Column-major matrix mapping the renderer's top-left normalized source
    // coordinates into the external texture's sampling coordinates.
    std::array<float, 16> transform {
        1.0F, 0.0F, 0.0F, 0.0F,
        0.0F, 1.0F, 0.0F, 0.0F,
        0.0F, 0.0F, 1.0F, 0.0F,
        0.0F, 0.0F, 0.0F, 1.0F,
    };
    bool rawYcbcr = false;
    int bitDepth = 0;
    std::int64_t timestampNanoseconds = 0;
    std::uint32_t generation = 0;

    explicit operator bool() const noexcept
    {
        return texture != 0 && timestampNanoseconds >= 0
            && generation != 0;
    }
};

struct QTAV_RENDER_OPENGL_EXPORT OpenGLHardwareImportResult {
    OpenGLHardwareImportStatus status =
        OpenGLHardwareImportStatus::Unsupported;
    OpenGLExternalTextureFrame texture;
    std::string detail;

    explicit operator bool() const noexcept
    {
        return status == OpenGLHardwareImportStatus::Ready
            && static_cast<bool>(texture);
    }
};

// Implemented by an optional platform interop target. prepareFrame() and
// releaseCurrentContextResources() run with the renderer's OpenGL ES context
// current. Implementations must not map, transfer, stage, or re-upload
// decoded pixels through CPU memory.
class QTAV_RENDER_OPENGL_EXPORT OpenGLHardwareFrameInterop {
public:
    using FrameAvailableCallback = std::function<void()>;

    virtual ~OpenGLHardwareFrameInterop();

    virtual HardwareInteropCapabilities capabilities() const = 0;
    virtual bool supports(const HardwareFrame& frame) const noexcept = 0;
    virtual OpenGLHardwareImportResult prepareFrame(
        const VideoFrame& frame) = 0;
    // Called after all sampling commands and the optional platform-present
    // callback for the imported image have been submitted. Android
    // implementations use this point to return an AImageReader buffer with a
    // GPU release fence that also covers window presentation.
    virtual bool releaseFrame(
        const OpenGLExternalTextureFrame& frame,
        std::string& detail) noexcept;
    virtual void releaseCurrentContextResources() noexcept = 0;
    virtual void setFrameAvailableCallback(
        FrameAvailableCallback callback) = 0;
};

class QTAV_RENDER_OPENGL_EXPORT OpenGLVideoRenderer final
    : public VideoRenderAPI {
public:
    explicit OpenGLVideoRenderer(
        OpenGLCurrentTargetCallback currentTarget,
        std::shared_ptr<OpenGLHardwareFrameInterop>
            hardwareInterop = {});
    ~OpenGLVideoRenderer() override;

    OpenGLVideoRenderer(OpenGLVideoRenderer&&) noexcept;
    OpenGLVideoRenderer& operator=(OpenGLVideoRenderer&&) noexcept;
    OpenGLVideoRenderer(const OpenGLVideoRenderer&) = delete;
    OpenGLVideoRenderer& operator=(const OpenGLVideoRenderer&) = delete;

    VideoRenderCapabilities capabilities() const override;
    void setEventCallback(EventCallback callback) override;
    bool open(const VideoRenderConfig& config) override;
    bool configure(const VideoRenderConfig& config) override;
    VideoRenderAttemptResult renderDetailed(
        const VideoFrame& frame) override;
    bool render(const VideoFrame& frame) override;
    void close() noexcept override;

    void setCurrentTargetCallback(
        OpenGLCurrentTargetCallback callback);
    void setPresentCallback(OpenGLPresentCallback callback);
    OpenGLHardwareImportStatus prepareHardwareFrame(
        const VideoFrame& frame,
        std::string* detail = nullptr);
    void setHardwareFrameInterop(
        std::shared_ptr<OpenGLHardwareFrameInterop> hardwareInterop);
    std::shared_ptr<OpenGLHardwareFrameInterop>
    hardwareFrameInterop() const noexcept;

private:
    class Impl;
    std::unique_ptr<Impl> impl_;
};

} // namespace qtav
