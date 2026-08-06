// SPDX-License-Identifier: LGPL-2.1-or-later

#include <qtav/opengl_video_renderer.h>

#include <EGL/egl.h>
#include <GLES3/gl3.h>
#include <GLES2/gl2ext.h>

#include <libplacebo/log.h>
#include <libplacebo/opengl.h>
#include <libplacebo/renderer.h>
#include <libplacebo/swapchain.h>

#include <algorithm>
#include <array>
#include <cstdint>
#include <cstring>
#include <mutex>
#include <string>
#include <utility>

#include "frame_internal.h"
#include "qtav_libplacebo_ffmpeg_bridge.h"

namespace qtav {
namespace {

constexpr char NormalizeVertexShader[] = R"qtav(#version 300 es

out vec2 textureCoordinate;

void main()
{
    const vec2 positions[3] = vec2[](
        vec2(-1.0, -1.0),
        vec2( 3.0, -1.0),
        vec2(-1.0,  3.0));
    const vec2 coordinates[3] = vec2[](
        vec2(0.0, 0.0),
        vec2(2.0, 0.0),
        vec2(0.0, 2.0));
    gl_Position = vec4(positions[gl_VertexID], 0.0, 1.0);
    textureCoordinate = coordinates[gl_VertexID];
}
)qtav";

constexpr char RawNormalizeFragmentShader[] = R"qtav(#version 300 es
#extension GL_OES_EGL_image_external_essl3 : require
#extension GL_EXT_YUV_target : require

precision highp float;
precision highp __samplerExternal2DY2YEXT;

in vec2 textureCoordinate;
layout(location = 0) out vec4 outputColor;

uniform __samplerExternal2DY2YEXT externalImage;
uniform mat4 externalTransform;

void main()
{
    vec4 coordinate = externalTransform * vec4(
        clamp(textureCoordinate, vec2(0.0), vec2(1.0)),
        0.0,
        1.0);
    // GL_EXT_YUV_target defines the sampled R/G/B components as raw
    // normalized Y/Cb/Cr. This pass deliberately performs no matrix,
    // transfer, gamut, tone-mapping, or output-encoding operation.
    outputColor = vec4(texture(externalImage, coordinate.xy).rgb, 1.0);
}
)qtav";

bool validViewport(
    const VideoViewport& viewport,
    const VideoSize& surface) noexcept
{
    if (!viewport.isValid()) {
        return true;
    }
    return viewport.x >= 0 && viewport.y >= 0
        && viewport.x <= surface.width - viewport.width
        && viewport.y <= surface.height - viewport.height;
}

bool supportedConfig(const VideoRenderConfig& config) noexcept
{
    return config.surfaceSize.isValid()
        && validViewport(config.viewport, config.surfaceSize)
        && config.deviceOwnership == NativeResourceOwnership::Borrowed
        && config.contextOwnership == NativeResourceOwnership::Borrowed
        && config.surfaceOwnership == NativeResourceOwnership::Borrowed;
}

VideoViewport effectiveViewport(const VideoRenderConfig& config) noexcept
{
    return config.viewport.isValid()
        ? config.viewport
        : VideoViewport {
              0,
              0,
              config.surfaceSize.width,
              config.surfaceSize.height,
          };
}

pl_rotation rotation(VideoRotation value) noexcept
{
    switch (value) {
    case VideoRotation::Rotate90:
        return PL_ROTATION_90;
    case VideoRotation::Rotate180:
        return PL_ROTATION_180;
    case VideoRotation::Rotate270:
        return PL_ROTATION_270;
    default:
        return PL_ROTATION_0;
    }
}

enum pl_color_levels levels(ColorRange value) noexcept
{
    switch (value) {
    case ColorRange::Limited:
        return PL_COLOR_LEVELS_LIMITED;
    case ColorRange::Full:
        return PL_COLOR_LEVELS_FULL;
    default:
        return PL_COLOR_LEVELS_UNKNOWN;
    }
}

enum pl_color_system system(ColorMatrix value) noexcept
{
    switch (value) {
    case ColorMatrix::RGB:
        return PL_COLOR_SYSTEM_RGB;
    case ColorMatrix::BT709:
        return PL_COLOR_SYSTEM_BT_709;
    case ColorMatrix::SMPTE240M:
        return PL_COLOR_SYSTEM_SMPTE_240M;
    case ColorMatrix::BT2020NCL:
        return PL_COLOR_SYSTEM_BT_2020_NC;
    case ColorMatrix::BT2020CL:
        return PL_COLOR_SYSTEM_BT_2020_C;
    case ColorMatrix::ICtCp:
        return PL_COLOR_SYSTEM_BT_2100_PQ;
    case ColorMatrix::YCgCo:
        return PL_COLOR_SYSTEM_YCGCO;
    default:
        return PL_COLOR_SYSTEM_BT_601;
    }
}

enum pl_color_primaries primaries(ColorPrimaries value) noexcept
{
    switch (value) {
    case ColorPrimaries::BT470M:
        return PL_COLOR_PRIM_BT_470M;
    case ColorPrimaries::BT470BG:
        return PL_COLOR_PRIM_BT_601_625;
    case ColorPrimaries::SMPTE170M:
    case ColorPrimaries::SMPTE240M:
        return PL_COLOR_PRIM_BT_601_525;
    case ColorPrimaries::BT2020:
        return PL_COLOR_PRIM_BT_2020;
    case ColorPrimaries::SMPTE431:
        return PL_COLOR_PRIM_DCI_P3;
    case ColorPrimaries::SMPTE432:
        return PL_COLOR_PRIM_DISPLAY_P3;
    case ColorPrimaries::EBU3213:
        return PL_COLOR_PRIM_EBU_3213;
    case ColorPrimaries::BT709:
    default:
        return PL_COLOR_PRIM_BT_709;
    }
}

enum pl_color_transfer transfer(ColorTransfer value) noexcept
{
    switch (value) {
    case ColorTransfer::Gamma22:
        return PL_COLOR_TRC_GAMMA22;
    case ColorTransfer::Gamma28:
        return PL_COLOR_TRC_GAMMA28;
    case ColorTransfer::Linear:
        return PL_COLOR_TRC_LINEAR;
    case ColorTransfer::SRGB:
        return PL_COLOR_TRC_SRGB;
    case ColorTransfer::PQ:
        return PL_COLOR_TRC_PQ;
    case ColorTransfer::HLG:
        return PL_COLOR_TRC_HLG;
    case ColorTransfer::SMPTE428:
        return PL_COLOR_TRC_ST428;
    default:
        return PL_COLOR_TRC_BT_1886;
    }
}

enum pl_chroma_location chromaLocation(ChromaLocation value) noexcept
{
    switch (value) {
    case ChromaLocation::Left:
        return PL_CHROMA_LEFT;
    case ChromaLocation::Center:
        return PL_CHROMA_CENTER;
    case ChromaLocation::TopLeft:
        return PL_CHROMA_TOP_LEFT;
    case ChromaLocation::Top:
        return PL_CHROMA_TOP_CENTER;
    case ChromaLocation::BottomLeft:
        return PL_CHROMA_BOTTOM_LEFT;
    case ChromaLocation::Bottom:
        return PL_CHROMA_BOTTOM_CENTER;
    default:
        return PL_CHROMA_UNKNOWN;
    }
}

void setHdrMetadata(
    const VideoFrame& frame,
    struct pl_hdr_metadata& destination) noexcept
{
    const MasteringDisplayMetadata mastering =
        frame.masteringDisplayMetadata();
    if (mastering.hasPrimaries) {
        destination.prim.red = {
            static_cast<float>(mastering.primaries[0].x),
            static_cast<float>(mastering.primaries[0].y),
        };
        destination.prim.green = {
            static_cast<float>(mastering.primaries[1].x),
            static_cast<float>(mastering.primaries[1].y),
        };
        destination.prim.blue = {
            static_cast<float>(mastering.primaries[2].x),
            static_cast<float>(mastering.primaries[2].y),
        };
        destination.prim.white = {
            static_cast<float>(mastering.whitePoint.x),
            static_cast<float>(mastering.whitePoint.y),
        };
    }
    if (mastering.hasLuminance) {
        destination.min_luma = static_cast<float>(
            std::max(mastering.minimumLuminance, 0.000001));
        destination.max_luma = static_cast<float>(
            mastering.maximumLuminance);
    }
    const ContentLightMetadata light = frame.contentLightMetadata();
    destination.max_cll = static_cast<float>(
        light.maximumContentLightLevel);
    destination.max_fall = static_cast<float>(
        light.maximumFrameAverageLightLevel);
}

void setSourceColor(const VideoFrame& source, struct pl_frame& frame) noexcept
{
    const VideoColorSpace color = source.colorSpaceInfo();
    frame.repr.sys = system(color.matrix);
    frame.repr.levels = levels(color.range);
    frame.repr.alpha = PL_ALPHA_NONE;
    frame.color.primaries = primaries(color.primaries);
    frame.color.transfer = transfer(color.transfer);
    setHdrMetadata(source, frame.color.hdr);
    pl_frame_set_chroma_location(&frame, chromaLocation(color.chromaLocation));
}

void setTargetColor(
    OpenGLOutputColorSpace colorSpace,
    struct pl_frame& target) noexcept
{
    target.repr.sys = PL_COLOR_SYSTEM_RGB;
    target.repr.levels = PL_COLOR_LEVELS_FULL;
    target.repr.alpha = PL_ALPHA_NONE;
    switch (colorSpace) {
    case OpenGLOutputColorSpace::HDR10PQ:
        target.color = pl_color_space_hdr10;
        break;
    case OpenGLOutputColorSpace::HDR10HLG:
        target.color = pl_color_space_bt2020_hlg;
        break;
    default:
        target.color = pl_color_space_srgb;
        break;
    }
}

void applyGeometry(
    struct pl_frame& image,
    struct pl_frame& target,
    const VideoRenderConfig& config) noexcept
{
    image.rotation = rotation(config.rotation);
    const VideoViewport viewport = effectiveViewport(config);
    target.crop = {
        static_cast<float>(viewport.x),
        static_cast<float>(viewport.y),
        static_cast<float>(viewport.x + viewport.width),
        static_cast<float>(viewport.y + viewport.height),
    };
    if (config.aspectRatio == VideoAspectRatioMode::Stretch) {
        return;
    }
    if (config.aspectRatio == VideoAspectRatioMode::Fit) {
        const float sourceAspect = pl_aspect_rotate(
            pl_rect2df_aspect(&image.crop),
            image.rotation);
        pl_rect2df_aspect_set(&target.crop, sourceAspect, 0.0F);
        return;
    }
    const float targetAspect = pl_rect2df_aspect(&target.crop);
    const float sourceAspect = pl_aspect_rotate(
        targetAspect,
        image.rotation);
    pl_rect2df_aspect_set(&image.crop, sourceAspect, 0.0F);
}

void initializePlane(
    struct pl_plane& plane,
    pl_tex texture,
    int components,
    int firstComponent) noexcept
{
    plane.texture = texture;
    plane.components = components;
    std::fill(
        std::begin(plane.component_mapping),
        std::end(plane.component_mapping),
        -1);
    for (int component = 0; component < components; ++component) {
        plane.component_mapping[component] = firstComponent + component;
    }
}

std::string shaderLog(GLuint object, bool program)
{
    GLint length = 0;
    if (program) {
        glGetProgramiv(object, GL_INFO_LOG_LENGTH, &length);
    } else {
        glGetShaderiv(object, GL_INFO_LOG_LENGTH, &length);
    }
    if (length <= 1) {
        return {};
    }
    std::string result(static_cast<std::size_t>(length), '\0');
    GLsizei written = 0;
    if (program) {
        glGetProgramInfoLog(object, length, &written, result.data());
    } else {
        glGetShaderInfoLog(object, length, &written, result.data());
    }
    result.resize(written > 0 ? static_cast<std::size_t>(written) : 0U);
    return result;
}

GLuint compileShader(GLenum type, const char* source, std::string& error)
{
    const GLuint shader = glCreateShader(type);
    if (!shader) {
        error = "glCreateShader(external-image normalization) failed";
        return 0;
    }
    glShaderSource(shader, 1, &source, nullptr);
    glCompileShader(shader);
    GLint compiled = GL_FALSE;
    glGetShaderiv(shader, GL_COMPILE_STATUS, &compiled);
    if (compiled != GL_TRUE) {
        error = "External-image normalization shader compilation failed: "
            + shaderLog(shader, false);
        glDeleteShader(shader);
        return 0;
    }
    return shader;
}

bool hasExtension(const char* extensions, const char* name) noexcept
{
    if (!extensions || !name || !*name) {
        return false;
    }
    const std::size_t length = std::strlen(name);
    const char* current = extensions;
    while ((current = std::strstr(current, name))) {
        const bool left = current == extensions || current[-1] == ' ';
        const bool right = current[length] == '\0'
            || current[length] == ' ';
        if (left && right) {
            return true;
        }
        current += length;
    }
    return false;
}

const char* glErrorName(GLenum error) noexcept
{
    switch (error) {
    case GL_NO_ERROR:
        return "GL_NO_ERROR";
    case GL_INVALID_ENUM:
        return "GL_INVALID_ENUM";
    case GL_INVALID_VALUE:
        return "GL_INVALID_VALUE";
    case GL_INVALID_OPERATION:
        return "GL_INVALID_OPERATION";
    case GL_INVALID_FRAMEBUFFER_OPERATION:
        return "GL_INVALID_FRAMEBUFFER_OPERATION";
    case GL_OUT_OF_MEMORY:
        return "GL_OUT_OF_MEMORY";
    default:
        return "unknown OpenGL ES error";
    }
}

bool checkError(const char* operation, std::string& error)
{
    const GLenum code = glGetError();
    if (code == GL_NO_ERROR) {
        return true;
    }
    error = std::string(operation) + " failed: " + glErrorName(code)
        + " (" + std::to_string(code) + ')';
    while (glGetError() != GL_NO_ERROR) {
    }
    return false;
}

pl_voidfunc_t openGLProcAddress(const char* name) noexcept
{
    return reinterpret_cast<pl_voidfunc_t>(eglGetProcAddress(name));
}

class ExternalImageNormalizer {
public:
    ~ExternalImageNormalizer()
    {
        destroy();
    }

    GLuint texture() const noexcept
    {
        return texture_;
    }

    bool convert(
        const OpenGLExternalTextureFrame& source,
        int width,
        int height,
        std::string& error)
    {
        if (!source || !source.rawYcbcr || width <= 0 || height <= 0) {
            error = "The raw OpenGL external image is incomplete";
            return false;
        }
        if (!ensure(width, height, error)) {
            return false;
        }
        glBindFramebuffer(GL_FRAMEBUFFER, framebuffer_);
        glViewport(0, 0, width, height);
        glDisable(GL_BLEND);
        glDisable(GL_DEPTH_TEST);
        glDisable(GL_SCISSOR_TEST);
        glUseProgram(program_);
        glUniformMatrix4fv(
            transform_,
            1,
            GL_FALSE,
            source.transform.data());
        glActiveTexture(GL_TEXTURE0);
        glBindTexture(GL_TEXTURE_EXTERNAL_OES, source.texture);
        glBindVertexArray(vertexArray_);
        glDrawArrays(GL_TRIANGLES, 0, 3);
        return checkError("Raw Y/Cb/Cr normalization", error);
    }

    void destroy() noexcept
    {
        if (framebuffer_) {
            glDeleteFramebuffers(1, &framebuffer_);
            framebuffer_ = 0;
        }
        if (texture_) {
            glDeleteTextures(1, &texture_);
            texture_ = 0;
        }
        if (vertexArray_) {
            glDeleteVertexArrays(1, &vertexArray_);
            vertexArray_ = 0;
        }
        if (program_) {
            glDeleteProgram(program_);
            program_ = 0;
        }
        transform_ = -1;
        width_ = 0;
        height_ = 0;
    }

private:
    bool ensureProgram(std::string& error)
    {
        if (program_) {
            return true;
        }
        const char* extensions = reinterpret_cast<const char*>(
            glGetString(GL_EXTENSIONS));
        if (!hasExtension(extensions, "GL_EXT_YUV_target")
            || !hasExtension(
                extensions,
                "GL_OES_EGL_image_external_essl3")) {
            error =
                "Raw external Y/Cb/Cr normalization requires GL_EXT_YUV_target and GL_OES_EGL_image_external_essl3";
            return false;
        }
        if (!hasExtension(extensions, "GL_EXT_color_buffer_float")
            && !hasExtension(
                extensions,
                "GL_EXT_color_buffer_half_float")) {
            error =
                "Raw external Y/Cb/Cr normalization requires a renderable RGBA16F texture";
            return false;
        }
        std::string shaderError;
        const GLuint vertex = compileShader(
            GL_VERTEX_SHADER,
            NormalizeVertexShader,
            shaderError);
        if (!vertex) {
            error = std::move(shaderError);
            return false;
        }
        const GLuint fragment = compileShader(
            GL_FRAGMENT_SHADER,
            RawNormalizeFragmentShader,
            shaderError);
        if (!fragment) {
            glDeleteShader(vertex);
            error = std::move(shaderError);
            return false;
        }
        program_ = glCreateProgram();
        if (program_) {
            glAttachShader(program_, vertex);
            glAttachShader(program_, fragment);
            glLinkProgram(program_);
            GLint linked = GL_FALSE;
            glGetProgramiv(program_, GL_LINK_STATUS, &linked);
            if (linked != GL_TRUE) {
                error = "External-image normalization program link failed: "
                    + shaderLog(program_, true);
                glDeleteProgram(program_);
                program_ = 0;
            }
        } else {
            error = "glCreateProgram(external-image normalization) failed";
        }
        glDeleteShader(fragment);
        glDeleteShader(vertex);
        if (!program_) {
            return false;
        }
        glUseProgram(program_);
        const GLint sampler = glGetUniformLocation(
            program_,
            "externalImage");
        transform_ = glGetUniformLocation(
            program_,
            "externalTransform");
        if (sampler < 0 || transform_ < 0) {
            error = "External-image normalization uniforms are unavailable";
            destroy();
            return false;
        }
        glUniform1i(sampler, 0);
        glGenVertexArrays(1, &vertexArray_);
        if (!vertexArray_) {
            error = "External-image normalization VAO creation failed";
            destroy();
            return false;
        }
        if (!checkError(
                "External-image normalization program",
                error)) {
            destroy();
            return false;
        }
        return true;
    }

    bool ensure(int width, int height, std::string& error)
    {
        if (!ensureProgram(error)) {
            return false;
        }
        if (texture_ && width_ == width && height_ == height) {
            return true;
        }
        if (framebuffer_) {
            glDeleteFramebuffers(1, &framebuffer_);
            framebuffer_ = 0;
        }
        if (texture_) {
            glDeleteTextures(1, &texture_);
            texture_ = 0;
        }
        glGenTextures(1, &texture_);
        glBindTexture(GL_TEXTURE_2D, texture_);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);
        glTexImage2D(
            GL_TEXTURE_2D,
            0,
            GL_RGBA16F,
            width,
            height,
            0,
            GL_RGBA,
            GL_HALF_FLOAT,
            nullptr);
        glGenFramebuffers(1, &framebuffer_);
        glBindFramebuffer(GL_FRAMEBUFFER, framebuffer_);
        glFramebufferTexture2D(
            GL_FRAMEBUFFER,
            GL_COLOR_ATTACHMENT0,
            GL_TEXTURE_2D,
            texture_,
            0);
        if (!texture_ || !framebuffer_
            || glCheckFramebufferStatus(GL_FRAMEBUFFER)
                != GL_FRAMEBUFFER_COMPLETE) {
            error =
                "The raw Y/Cb/Cr RGBA16F normalization target is incomplete";
            if (framebuffer_) {
                glDeleteFramebuffers(1, &framebuffer_);
                framebuffer_ = 0;
            }
            if (texture_) {
                glDeleteTextures(1, &texture_);
                texture_ = 0;
            }
            return false;
        }
        width_ = width;
        height_ = height;
        return checkError("Raw Y/Cb/Cr normalization target", error);
    }

    GLuint program_ = 0;
    GLuint vertexArray_ = 0;
    GLuint texture_ = 0;
    GLuint framebuffer_ = 0;
    GLint transform_ = -1;
    int width_ = 0;
    int height_ = 0;
};

struct HardwareFrameKey {
    std::uintptr_t buffer = 0;
    std::uint32_t generation = 0;
    std::int64_t timestampMilliseconds = 0;

    bool operator==(const HardwareFrameKey& other) const noexcept
    {
        return buffer == other.buffer
            && generation == other.generation
            && timestampMilliseconds == other.timestampMilliseconds;
    }
};

HardwareFrameKey hardwareFrameKey(const VideoFrame& frame) noexcept
{
    HardwareFrameKey result;
    if (!frame || !frame.hasHardwareFrame()) {
        return result;
    }
    const NativeHandle output = frame.hardwareFrame().nativeHandle(
        HardwareHandleType::Frame);
    result.buffer = output.value;
    result.generation = output.subresource;
    result.timestampMilliseconds = frame.timestamp();
    return result;
}

class RendererEventState final {
public:
    void set(VideoRenderAPI::EventCallback callback)
    {
        std::lock_guard<std::mutex> lock(mutex_);
        callback_ = std::move(callback);
    }

    void notify(VideoRenderEventType type, std::string detail)
    {
        VideoRenderAPI::EventCallback callback;
        {
            std::lock_guard<std::mutex> lock(mutex_);
            callback = callback_;
        }
        if (callback) {
            callback({ type, std::move(detail) });
        }
    }

private:
    std::mutex mutex_;
    VideoRenderAPI::EventCallback callback_;
};

} // namespace

bool OpenGLRenderTarget::isHdr() const noexcept
{
    return isValid() && openGLColorSpaceIsHdr(colorSpace);
}

bool openGLColorSpaceIsHdr(OpenGLOutputColorSpace colorSpace) noexcept
{
    return colorSpace == OpenGLOutputColorSpace::HDR10PQ
        || colorSpace == OpenGLOutputColorSpace::HDR10HLG;
}

bool OpenGLHardwareFrameInterop::releaseFrame(
    const OpenGLExternalTextureFrame&,
    std::string&) noexcept
{
    return true;
}

bool OpenGLHardwareFrameInterop::initializeCurrentContext(
    std::string&)
{
    return true;
}

OpenGLHardwareFrameInterop::~OpenGLHardwareFrameInterop() = default;

class OpenGLVideoRenderer::Impl {
public:
    Impl(
        OpenGLCurrentTargetCallback currentTarget,
        std::shared_ptr<OpenGLHardwareFrameInterop> hardwareInterop)
        : currentTarget_(std::move(currentTarget))
        , hardwareInterop_(std::move(hardwareInterop))
        , eventState_(std::make_shared<RendererEventState>())
    {
        connectHardwareInterop();
    }

    ~Impl()
    {
        if (eventState_) {
            eventState_->set({});
        }
        if (hardwareInterop_) {
            hardwareInterop_->setFrameAvailableCallback({});
        }
        close();
    }

    static void logCallback(
        void* privateData,
        enum pl_log_level level,
        const char* message)
    {
        if (!privateData || !message || level > PL_LOG_ERR) {
            return;
        }
        auto* self = static_cast<Impl*>(privateData);
        std::lock_guard<std::mutex> lock(self->logMutex_);
        if (!self->lastLogError_.empty()) {
            self->lastLogError_ += " | ";
        }
        self->lastLogError_ += message;
    }

    std::string takeLogError(std::string fallback)
    {
        std::lock_guard<std::mutex> lock(logMutex_);
        if (!lastLogError_.empty()) {
            fallback += ": " + lastLogError_;
            lastLogError_.clear();
        }
        return fallback;
    }

    void notify(VideoRenderEventType type, std::string detail)
    {
        if (eventState_) {
            eventState_->notify(type, std::move(detail));
        }
    }

    void connectHardwareInterop()
    {
        if (!hardwareInterop_ || !eventState_) {
            return;
        }
        std::weak_ptr<RendererEventState> weak = eventState_;
        hardwareInterop_->setFrameAvailableCallback([weak] {
            if (const auto state = weak.lock()) {
                state->notify(VideoRenderEventType::RedrawRequested, {});
            }
        });
    }

    bool createResources(std::string& error)
    {
        const GLubyte* version = glGetString(GL_VERSION);
        GLint major = 0;
        glGetIntegerv(GL_MAJOR_VERSION, &major);
        if (!version || major < 3) {
            error =
                "The libplacebo OpenGL renderer requires a current OpenGL ES 3.x context";
            return false;
        }
        if (hardwareInterop_
            && !hardwareInterop_->initializeCurrentContext(error)) {
            hardwareInterop_->releaseCurrentContextResources();
            if (error.empty()) {
                error =
                    "The OpenGL ES hardware interop could not initialize its current-context resources";
            }
            return false;
        }
        pl_log_params logParams {};
        logParams.log_cb = &Impl::logCallback;
        logParams.log_priv = this;
        logParams.log_level = PL_LOG_ERR;
        log_ = pl_log_create(PL_API_VER, &logParams);

        pl_opengl_params openGLParams {};
        openGLParams.get_proc_addr = &openGLProcAddress;
        openGLParams.no_compute = true;
        openGLParams.egl_display = eglGetCurrentDisplay();
        openGLParams.egl_context = eglGetCurrentContext();
        openGL_ = pl_opengl_create(log_, &openGLParams);
        if (!openGL_) {
            error = takeLogError(
                "libplacebo could not attach to the current OpenGL ES context");
            destroyResources();
            return false;
        }
        renderer_ = pl_renderer_create(log_, openGL_->gpu);
        if (!renderer_) {
            error = takeLogError(
                "libplacebo could not create its OpenGL renderer");
            destroyResources();
            return false;
        }
        pl_opengl_swapchain_params swapchainParams {};
        swapchainParams.framebuffer.id = 0;
        swapchainParams.framebuffer.flipped = false;
        swapchainParams.max_swapchain_depth = 0;
        swapchain_ = pl_opengl_create_swapchain(
            openGL_,
            &swapchainParams);
        if (!swapchain_) {
            error = takeLogError(
                "libplacebo could not create its borrowed OpenGL framebuffer wrapper");
            destroyResources();
            return false;
        }
        return true;
    }

    void destroyResources() noexcept
    {
        if (openGL_) {
            pl_gpu_finish(openGL_->gpu);
        }
        if (hardwareInterop_) {
            hardwareInterop_->releaseCurrentContextResources();
        }
        preparedHardware_ = {};
        preparedKey_ = {};
        normalizer_.destroy();
        for (pl_tex& texture : uploadTextures_) {
            if (texture && openGL_) {
                pl_tex_destroy(openGL_->gpu, &texture);
            }
        }
        if (swapchain_) {
            pl_swapchain_destroy(&swapchain_);
        }
        if (renderer_) {
            pl_renderer_destroy(&renderer_);
        }
        if (openGL_) {
            pl_opengl_destroy(&openGL_);
        }
        if (log_) {
            pl_log_destroy(&log_);
        }
        targetGeneration_ = 0;
        targetFramebuffer_ = 0;
        targetSize_ = {};
    }

    void close() noexcept
    {
        std::lock_guard<std::mutex> renderLock(renderMutex_);
        std::lock_guard<std::mutex> stateLock(stateMutex_);
        if (open_ || openGL_) {
            destroyResources();
        }
        open_ = false;
        config_ = {};
    }

    bool beginTarget(
        const OpenGLRenderTarget& native,
        struct pl_swapchain_frame& swapchainFrame,
        struct pl_frame& target,
        std::string& error)
    {
        if (targetGeneration_ != native.generation
            || targetFramebuffer_ != native.framebuffer) {
            pl_opengl_framebuffer framebuffer {};
            framebuffer.id = static_cast<int>(native.framebuffer);
            framebuffer.flipped = false;
            pl_opengl_swapchain_update_fb(swapchain_, &framebuffer);
            targetGeneration_ = native.generation;
            targetFramebuffer_ = native.framebuffer;
            targetSize_ = {};
        }
        if (targetSize_.width != native.size.width
            || targetSize_.height != native.size.height) {
            int width = native.size.width;
            int height = native.size.height;
            if (!pl_swapchain_resize(swapchain_, &width, &height)
                || width != native.size.width
                || height != native.size.height) {
                error = takeLogError(
                    "libplacebo could not resize the borrowed OpenGL framebuffer");
                return false;
            }
            targetSize_ = native.size;
        }
        struct pl_color_space hint {};
        switch (native.colorSpace) {
        case OpenGLOutputColorSpace::HDR10PQ:
            hint = pl_color_space_hdr10;
            break;
        case OpenGLOutputColorSpace::HDR10HLG:
            hint = pl_color_space_bt2020_hlg;
            break;
        default:
            hint = pl_color_space_srgb;
            break;
        }
        pl_swapchain_colorspace_hint(swapchain_, &hint);
        if (!pl_swapchain_start_frame(swapchain_, &swapchainFrame)) {
            error = takeLogError(
                "libplacebo could not acquire the borrowed OpenGL framebuffer");
            return false;
        }
        pl_frame_from_swapchain(&target, &swapchainFrame);
        setTargetColor(native.colorSpace, target);
        return true;
    }

    bool wrapHardwareSource(
        const VideoFrame& source,
        const OpenGLExternalTextureFrame& external,
        pl_tex& texture,
        struct pl_frame& frame,
        struct pl_dovi_metadata& dovi,
        std::string& error)
    {
        const AVFrame* native =
            detail::FrameFactory::nativeVideoFrame(source);
        const int doviBitDepth = native
            ? qtav_pl_dovi_bit_depth(native)
            : 0;
        const bool hasDovi = doviBitDepth != 0;
        GLuint sourceTexture = external.texture;
        GLenum target = GL_TEXTURE_EXTERNAL_OES;
        GLint internalFormat = GL_RGBA8;
        if (external.rawYcbcr) {
            if (!normalizer_.convert(
                    external,
                    source.width(),
                    source.height(),
                    error)) {
                return false;
            }
            sourceTexture = normalizer_.texture();
            target = GL_TEXTURE_2D;
            internalFormat = GL_RGBA16F;
        } else if (hasDovi) {
            error =
                "Dolby Vision requires the raw external Y/Cb/Cr OpenGL import path";
            return false;
        }
        pl_opengl_wrap_params wrapParams {};
        wrapParams.texture = sourceTexture;
        wrapParams.width = source.width();
        wrapParams.height = source.height();
        wrapParams.target = target;
        wrapParams.iformat = internalFormat;
        texture = pl_opengl_wrap(openGL_->gpu, &wrapParams);
        if (!texture) {
            error = takeLogError(
                "libplacebo could not wrap the OpenGL hardware texture");
            return false;
        }
        frame.num_planes = 1;
        initializePlane(frame.planes[0], texture, 3, 0);
        frame.crop = {
            0.0F,
            0.0F,
            static_cast<float>(source.width()),
            static_cast<float>(source.height()),
        };
        setSourceColor(source, frame);
        if (hasDovi
            && !qtav_pl_map_dovi(&frame, &dovi, native)) {
            error =
                "FFmpeg Dolby Vision metadata changed during OpenGL frame mapping";
            return false;
        }
        if (external.rawYcbcr) {
            const int depth = hasDovi
                ? doviBitDepth
                : external.bitDepth > 0 ? external.bitDepth : 8;
            frame.repr.bits = { depth, depth, 0 };
        } else {
            frame.repr.sys = PL_COLOR_SYSTEM_RGB;
            frame.repr.levels = PL_COLOR_LEVELS_FULL;
            frame.repr.alpha = PL_ALPHA_NONE;
            frame.repr.bits = { 8, 8, 0 };
        }
        return true;
    }

    OpenGLHardwareImportStatus prepareHardwareFrame(
        const VideoFrame& frame,
        std::string& detail)
    {
        std::shared_ptr<OpenGLHardwareFrameInterop> interop;
        {
            std::lock_guard<std::mutex> stateLock(stateMutex_);
            if (!open_) {
                detail = "The OpenGL ES renderer is not open";
                return OpenGLHardwareImportStatus::Error;
            }
            interop = hardwareInterop_;
        }
        if (!frame || !frame.hasHardwareFrame()) {
            detail =
                "The frame is not an OpenGL ES-interoperable hardware frame";
            return OpenGLHardwareImportStatus::Unsupported;
        }
        if (!interop || !interop->supports(frame.hardwareFrame())) {
            detail =
                "The OpenGL ES renderer has no compatible interop for this hardware frame";
            return OpenGLHardwareImportStatus::Unsupported;
        }
        OpenGLHardwareImportResult imported = interop->prepareFrame(frame);
        if (imported.status == OpenGLHardwareImportStatus::Ready
            && !imported) {
            imported.status = OpenGLHardwareImportStatus::Error;
            if (imported.detail.empty()) {
                imported.detail =
                    "The OpenGL ES interop returned an invalid external texture";
            }
        }
        detail = imported.detail;
        if (imported.status == OpenGLHardwareImportStatus::Ready) {
            preparedKey_ = hardwareFrameKey(frame);
            preparedHardware_ = std::move(imported.texture);
        } else {
            preparedKey_ = {};
            preparedHardware_ = {};
        }
        return imported.status;
    }

    mutable std::mutex stateMutex_;
    std::mutex renderMutex_;
    std::mutex logMutex_;
    OpenGLCurrentTargetCallback currentTarget_;
    OpenGLPresentCallback present_;
    std::shared_ptr<OpenGLHardwareFrameInterop> hardwareInterop_;
    std::shared_ptr<RendererEventState> eventState_;
    VideoRenderConfig config_;
    bool open_ = false;
    std::string lastLogError_;
    pl_log log_ = nullptr;
    pl_opengl openGL_ = nullptr;
    pl_renderer renderer_ = nullptr;
    pl_swapchain swapchain_ = nullptr;
    std::array<pl_tex, 4> uploadTextures_ {};
    ExternalImageNormalizer normalizer_;
    std::uint64_t targetGeneration_ = 0;
    std::uint32_t targetFramebuffer_ = 0;
    VideoSize targetSize_;
    HardwareFrameKey preparedKey_;
    OpenGLExternalTextureFrame preparedHardware_;
};

OpenGLVideoRenderer::OpenGLVideoRenderer(
    OpenGLCurrentTargetCallback currentTarget,
    std::shared_ptr<OpenGLHardwareFrameInterop> hardwareInterop)
    : impl_(std::make_unique<Impl>(
          std::move(currentTarget),
          std::move(hardwareInterop)))
{
}

OpenGLVideoRenderer::~OpenGLVideoRenderer() = default;
OpenGLVideoRenderer::OpenGLVideoRenderer(OpenGLVideoRenderer&&) noexcept =
    default;
OpenGLVideoRenderer& OpenGLVideoRenderer::operator=(
    OpenGLVideoRenderer&&) noexcept = default;

VideoRenderCapabilities OpenGLVideoRenderer::capabilities() const
{
    VideoRenderCapabilities result;
    result.softwareFormats = {
        PixelFormat::YUV420P,
        PixelFormat::YUV422P,
        PixelFormat::YUV444P,
        PixelFormat::YUV420P10,
        PixelFormat::NV12,
        PixelFormat::NV21,
        PixelFormat::P010,
        PixelFormat::RGB24,
        PixelFormat::BGR24,
        PixelFormat::RGBA,
        PixelFormat::BGRA,
        PixelFormat::ARGB,
        PixelFormat::Gray8,
    };
    result.customViewport = true;
    result.rotation = true;
    if (impl_) {
        std::lock_guard<std::mutex> lock(impl_->stateMutex_);
        if (impl_->hardwareInterop_) {
            result.hardwareDevices =
                impl_->hardwareInterop_->capabilities().sourceDevices;
        }
    }
    return result;
}

void OpenGLVideoRenderer::setEventCallback(EventCallback callback)
{
    if (impl_) {
        impl_->eventState_->set(std::move(callback));
    }
}

bool OpenGLVideoRenderer::open(const VideoRenderConfig& config)
{
    if (!impl_) {
        return false;
    }
    std::string error;
    bool opened = false;
    {
        std::lock_guard<std::mutex> renderLock(impl_->renderMutex_);
        std::lock_guard<std::mutex> stateLock(impl_->stateMutex_);
        if (!impl_->currentTarget_) {
            error =
                "The OpenGL ES renderer requires a current-target callback";
        } else if (!supportedConfig(config)) {
            error =
                "The OpenGL ES renderer requires a valid borrowed surface configuration";
        } else if (impl_->open_) {
            impl_->config_ = config;
            opened = true;
        } else if (impl_->createResources(error)) {
            impl_->config_ = config;
            impl_->open_ = true;
            opened = true;
        }
    }
    if (!opened) {
        impl_->notify(VideoRenderEventType::Error, std::move(error));
    }
    return opened;
}

bool OpenGLVideoRenderer::configure(const VideoRenderConfig& config)
{
    if (!impl_) {
        return false;
    }
    bool configured = false;
    {
        std::lock_guard<std::mutex> lock(impl_->stateMutex_);
        configured = impl_->open_ && supportedConfig(config);
        if (configured) {
            impl_->config_ = config;
        }
    }
    if (configured) {
        impl_->notify(VideoRenderEventType::RedrawRequested, {});
    } else {
        impl_->notify(
            VideoRenderEventType::Error,
            "The OpenGL ES renderer is closed or the configuration is invalid");
    }
    return configured;
}

VideoRenderAttemptResult OpenGLVideoRenderer::renderDetailed(
    const VideoFrame& frame)
{
    if (!impl_ || !frame) {
        return {
            VideoRenderAttemptStatus::FatalError,
            0,
            "The OpenGL ES renderer received an invalid frame",
        };
    }
    std::lock_guard<std::mutex> renderLock(impl_->renderMutex_);
    VideoRenderConfig config;
    OpenGLCurrentTargetCallback currentTarget;
    OpenGLPresentCallback present;
    std::shared_ptr<OpenGLHardwareFrameInterop> hardwareInterop;
    {
        std::lock_guard<std::mutex> stateLock(impl_->stateMutex_);
        if (impl_->open_) {
            config = impl_->config_;
            currentTarget = impl_->currentTarget_;
            present = impl_->present_;
            hardwareInterop = impl_->hardwareInterop_;
        }
    }
    if (!config.surfaceSize.isValid()) {
        const std::string detail = "The OpenGL ES renderer is not open";
        impl_->notify(VideoRenderEventType::Error, detail);
        return { VideoRenderAttemptStatus::FatalError, 0, detail };
    }
    const OpenGLRenderTarget nativeTarget = currentTarget
        ? currentTarget()
        : OpenGLRenderTarget {};
    if (!nativeTarget.isValid()) {
        const std::string detail =
            "The current OpenGL ES target is unavailable";
        impl_->notify(
            VideoRenderEventType::SurfaceLost,
            detail);
        return { VideoRenderAttemptStatus::SurfaceLost, 0, detail };
    }
    if (nativeTarget.size.width != config.surfaceSize.width
        || nativeTarget.size.height != config.surfaceSize.height) {
        const std::string detail =
            "The current OpenGL ES target size does not match the configured surface";
        impl_->notify(VideoRenderEventType::Error, detail);
        return { VideoRenderAttemptStatus::FatalError, 0, detail };
    }

    std::string error;
    OpenGLExternalTextureFrame external;
    if (frame.hasHardwareFrame()) {
        const HardwareFrameKey key = hardwareFrameKey(frame);
        if (!(impl_->preparedKey_ == key) || !impl_->preparedHardware_) {
            const OpenGLHardwareImportStatus status =
                impl_->prepareHardwareFrame(frame, error);
            if (status == OpenGLHardwareImportStatus::Pending
                || status == OpenGLHardwareImportStatus::Stale) {
                return {
                    status == OpenGLHardwareImportStatus::Pending
                        ? VideoRenderAttemptStatus::DeferredUntilRedraw
                        : VideoRenderAttemptStatus::Discarded,
                    0,
                    error,
                };
            }
            if (status != OpenGLHardwareImportStatus::Ready) {
                const std::string detail = error.empty()
                    ? "The OpenGL ES hardware frame preparation failed"
                    : error;
                impl_->notify(VideoRenderEventType::Error, detail);
                return { VideoRenderAttemptStatus::FatalError, 0, detail };
            }
        }
        external = impl_->preparedHardware_;
    }

    struct pl_swapchain_frame swapchainFrame {};
    struct pl_frame target {};
    if (!impl_->beginTarget(
            nativeTarget,
            swapchainFrame,
            target,
            error)) {
        if (external && hardwareInterop) {
            std::string releaseError;
            hardwareInterop->releaseFrame(external, releaseError);
            impl_->preparedHardware_ = {};
            impl_->preparedKey_ = {};
            if (error.empty()) {
                error = std::move(releaseError);
            }
        }
        impl_->notify(VideoRenderEventType::Error, error);
        return { VideoRenderAttemptStatus::FatalError, 0, error };
    }

    struct pl_frame image {};
    struct pl_dovi_metadata dovi {};
    pl_tex hardwareTexture = nullptr;
    bool mappedSoftware = false;
    if (external) {
        impl_->wrapHardwareSource(
            frame,
            external,
            hardwareTexture,
            image,
            dovi,
            error);
    } else {
        const AVFrame* native =
            detail::FrameFactory::nativeVideoFrame(frame);
        if (!native
            || !qtav_pl_map_avframe(
                impl_->openGL_->gpu,
                &image,
                impl_->uploadTextures_.data(),
                native)) {
            error = impl_->takeLogError(
                "libplacebo could not map the decoded software frame for OpenGL");
        } else {
            mappedSoftware = true;
        }
    }

    bool rendered = error.empty();
    if (rendered) {
        applyGeometry(image, target, config);
        rendered = pl_render_image(
            impl_->renderer_,
            &image,
            &target,
            &pl_render_default_params);
        if (!rendered) {
            error = impl_->takeLogError(
                "libplacebo could not render the OpenGL video frame");
        }
    }
    if (mappedSoftware) {
        qtav_pl_unmap_avframe(impl_->openGL_->gpu, &image);
    }
    const bool submitted = pl_swapchain_submit_frame(impl_->swapchain_);
    if (!submitted && error.empty()) {
        error = impl_->takeLogError(
            "libplacebo could not submit the OpenGL framebuffer");
    }
    bool presented = submitted;
    if (rendered && submitted && error.empty()
        && present && !present(error)) {
        presented = false;
        if (error.empty()) {
            error = "The platform could not present the OpenGL framebuffer";
        }
    }
    if (hardwareTexture) {
        pl_tex_destroy(impl_->openGL_->gpu, &hardwareTexture);
    }
    if (external && hardwareInterop) {
        std::string releaseError;
        if (!hardwareInterop->releaseFrame(external, releaseError)
            && error.empty()) {
            error = releaseError.empty()
                ? "The OpenGL hardware image could not be returned to its producer"
                : std::move(releaseError);
        }
        impl_->preparedHardware_ = {};
        impl_->preparedKey_ = {};
    }
    if (!rendered || !submitted || !presented || !error.empty()) {
        const std::string detail = error.empty()
            ? "libplacebo OpenGL rendering failed"
            : error;
        impl_->notify(VideoRenderEventType::Error, detail);
        return { VideoRenderAttemptStatus::FatalError, 0, detail };
    }
    return { VideoRenderAttemptStatus::Presented, 0, {} };
}

bool OpenGLVideoRenderer::render(const VideoFrame& frame)
{
    return renderDetailed(frame).frameConsumed();
}

void OpenGLVideoRenderer::close() noexcept
{
    if (impl_) {
        impl_->close();
    }
}

void OpenGLVideoRenderer::setCurrentTargetCallback(
    OpenGLCurrentTargetCallback callback)
{
    if (!impl_) {
        return;
    }
    std::lock_guard<std::mutex> lock(impl_->stateMutex_);
    impl_->currentTarget_ = std::move(callback);
}

void OpenGLVideoRenderer::setPresentCallback(
    OpenGLPresentCallback callback)
{
    if (!impl_) {
        return;
    }
    std::lock_guard<std::mutex> lock(impl_->stateMutex_);
    impl_->present_ = std::move(callback);
}

OpenGLHardwareImportStatus OpenGLVideoRenderer::prepareHardwareFrame(
    const VideoFrame& frame,
    std::string* detail)
{
    if (!impl_) {
        if (detail) {
            *detail = "The OpenGL ES renderer object is empty";
        }
        return OpenGLHardwareImportStatus::Error;
    }
    std::lock_guard<std::mutex> renderLock(impl_->renderMutex_);
    std::string localDetail;
    const OpenGLHardwareImportStatus status =
        impl_->prepareHardwareFrame(frame, localDetail);
    if (detail) {
        *detail = std::move(localDetail);
    }
    return status;
}

void OpenGLVideoRenderer::setHardwareFrameInterop(
    std::shared_ptr<OpenGLHardwareFrameInterop> hardwareInterop)
{
    if (!impl_) {
        return;
    }
    std::string error;
    {
        std::lock_guard<std::mutex> renderLock(impl_->renderMutex_);
        std::shared_ptr<OpenGLHardwareFrameInterop> previous;
        bool rendererOpen = false;
        {
            std::lock_guard<std::mutex> stateLock(impl_->stateMutex_);
            previous = std::move(impl_->hardwareInterop_);
            rendererOpen = impl_->open_;
        }
        if (previous) {
            previous->setFrameAvailableCallback({});
            previous->releaseCurrentContextResources();
        }
        if (hardwareInterop && rendererOpen
            && !hardwareInterop->initializeCurrentContext(error)) {
            hardwareInterop->releaseCurrentContextResources();
            hardwareInterop.reset();
            if (error.empty()) {
                error =
                    "The OpenGL ES hardware interop could not initialize its current-context resources";
            }
        }
        {
            std::lock_guard<std::mutex> stateLock(impl_->stateMutex_);
            impl_->hardwareInterop_ = std::move(hardwareInterop);
        }
        impl_->preparedHardware_ = {};
        impl_->preparedKey_ = {};
        impl_->connectHardwareInterop();
    }
    if (!error.empty()) {
        impl_->notify(VideoRenderEventType::Error, std::move(error));
    }
}

std::shared_ptr<OpenGLHardwareFrameInterop>
OpenGLVideoRenderer::hardwareFrameInterop() const noexcept
{
    if (!impl_) {
        return {};
    }
    std::lock_guard<std::mutex> lock(impl_->stateMutex_);
    return impl_->hardwareInterop_;
}

} // namespace qtav
