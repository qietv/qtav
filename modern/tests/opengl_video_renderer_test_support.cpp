// SPDX-License-Identifier: LGPL-2.1-or-later

#include "opengl_video_renderer_test_support.h"

#include "frame_internal.h"

#include <qtav/opengl_video_renderer.h>

#include <EGL/egl.h>
#include <GLES3/gl3.h>

#include <algorithm>
#include <array>
#include <cmath>
#include <cstdint>
#include <string>
#include <vector>

extern "C" {
#include <libavutil/frame.h>
#include <libavutil/pixfmt.h>
}

namespace qtav::test {
namespace {

struct Pixel {
    std::uint8_t red = 0;
    std::uint8_t green = 0;
    std::uint8_t blue = 0;
    std::uint8_t alpha = 0;
};

bool isBlack(const Pixel& pixel) noexcept
{
    return pixel.red < 12 && pixel.green < 12 && pixel.blue < 12
        && pixel.alpha > 239;
}

class OffscreenContext {
public:
    ~OffscreenContext()
    {
        destroy();
    }

    bool create(int width, int height, std::string& error)
    {
        display_ = eglGetDisplay(EGL_DEFAULT_DISPLAY);
        if (display_ == EGL_NO_DISPLAY
            || eglInitialize(display_, nullptr, nullptr) != EGL_TRUE
            || eglBindAPI(EGL_OPENGL_ES_API) != EGL_TRUE) {
            error = "Could not initialize the offscreen EGL display";
            return false;
        }
        const EGLint configAttributes[] {
            EGL_SURFACE_TYPE, EGL_PBUFFER_BIT,
            EGL_RENDERABLE_TYPE, EGL_OPENGL_ES3_BIT,
            EGL_RED_SIZE, 8,
            EGL_GREEN_SIZE, 8,
            EGL_BLUE_SIZE, 8,
            EGL_ALPHA_SIZE, 8,
            EGL_NONE,
        };
        EGLConfig config = nullptr;
        EGLint count = 0;
        if (eglChooseConfig(
                display_,
                configAttributes,
                &config,
                1,
                &count) != EGL_TRUE
            || count != 1) {
            error = "No RGBA8 OpenGL ES 3 pbuffer config is available";
            return false;
        }
        const EGLint surfaceAttributes[] {
            EGL_WIDTH, width,
            EGL_HEIGHT, height,
            EGL_NONE,
        };
        surface_ = eglCreatePbufferSurface(
            display_, config, surfaceAttributes);
        const EGLint contextAttributes[] {
            EGL_CONTEXT_CLIENT_VERSION, 3,
            EGL_NONE,
        };
        context_ = eglCreateContext(
            display_,
            config,
            EGL_NO_CONTEXT,
            contextAttributes);
        if (surface_ == EGL_NO_SURFACE
            || context_ == EGL_NO_CONTEXT
            || eglMakeCurrent(
                   display_,
                   surface_,
                   surface_,
                   context_) != EGL_TRUE) {
            error =
                "Could not create the offscreen OpenGL ES 3 context";
            return false;
        }
        size_ = { width, height };
        return true;
    }

    void destroy() noexcept
    {
        if (display_ != EGL_NO_DISPLAY) {
            eglMakeCurrent(
                display_,
                EGL_NO_SURFACE,
                EGL_NO_SURFACE,
                EGL_NO_CONTEXT);
            if (context_ != EGL_NO_CONTEXT) {
                eglDestroyContext(display_, context_);
            }
            if (surface_ != EGL_NO_SURFACE) {
                eglDestroySurface(display_, surface_);
            }
            eglTerminate(display_);
        }
        display_ = EGL_NO_DISPLAY;
        context_ = EGL_NO_CONTEXT;
        surface_ = EGL_NO_SURFACE;
        size_ = {};
    }

    VideoSize size() const noexcept
    {
        return size_;
    }

private:
    EGLDisplay display_ = EGL_NO_DISPLAY;
    EGLContext context_ = EGL_NO_CONTEXT;
    EGLSurface surface_ = EGL_NO_SURFACE;
    VideoSize size_;
};

bool readPixels(
    VideoSize size,
    std::vector<Pixel>& pixels,
    std::string& error)
{
    pixels.resize(
        static_cast<std::size_t>(size.width)
        * static_cast<std::size_t>(size.height));
    glFinish();
    glReadPixels(
        0,
        0,
        size.width,
        size.height,
        GL_RGBA,
        GL_UNSIGNED_BYTE,
        pixels.data());
    const GLenum code = glGetError();
    if (code != GL_NO_ERROR) {
        error = "OpenGL ES readback failed ("
            + std::to_string(code) + ')';
        return false;
    }
    return true;
}

bool containsVisiblePixel(const std::vector<Pixel>& pixels) noexcept
{
    return std::count_if(
               pixels.begin(),
               pixels.end(),
               [](const Pixel& pixel) { return !isBlack(pixel); })
        > 16;
}

bool pixelsClose(
    const std::vector<Pixel>& left,
    const std::vector<Pixel>& right,
    int tolerance = 2) noexcept
{
    if (left.size() != right.size()) {
        return false;
    }
    return std::equal(
        left.begin(),
        left.end(),
        right.begin(),
        [tolerance](const Pixel& first, const Pixel& second) {
            return std::abs(
                       static_cast<int>(first.red)
                       - static_cast<int>(second.red))
                    <= tolerance
                && std::abs(
                       static_cast<int>(first.green)
                       - static_cast<int>(second.green))
                    <= tolerance
                && std::abs(
                       static_cast<int>(first.blue)
                       - static_cast<int>(second.blue))
                    <= tolerance
                && std::abs(
                       static_cast<int>(first.alpha)
                       - static_cast<int>(second.alpha))
                    <= tolerance;
        });
}

class SrgbFramebuffer {
public:
    ~SrgbFramebuffer()
    {
        glBindFramebuffer(GL_FRAMEBUFFER, 0);
        if (framebuffer_ != 0) {
            glDeleteFramebuffers(1, &framebuffer_);
        }
        if (texture_ != 0) {
            glDeleteTextures(1, &texture_);
        }
    }

    bool create(int width, int height, std::string& error)
    {
        glGenTextures(1, &texture_);
        glBindTexture(GL_TEXTURE_2D, texture_);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_NEAREST);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_NEAREST);
        glTexImage2D(
            GL_TEXTURE_2D,
            0,
            GL_SRGB8_ALPHA8,
            width,
            height,
            0,
            GL_RGBA,
            GL_UNSIGNED_BYTE,
            nullptr);
        glGenFramebuffers(1, &framebuffer_);
        glBindFramebuffer(GL_FRAMEBUFFER, framebuffer_);
        glFramebufferTexture2D(
            GL_FRAMEBUFFER,
            GL_COLOR_ATTACHMENT0,
            GL_TEXTURE_2D,
            texture_,
            0);
        if (glCheckFramebufferStatus(GL_FRAMEBUFFER)
                != GL_FRAMEBUFFER_COMPLETE
            || glGetError() != GL_NO_ERROR) {
            error = "The OpenGL ES sRGB test framebuffer is unavailable";
            return false;
        }
        glBindFramebuffer(GL_FRAMEBUFFER, 0);
        return true;
    }

    GLuint handle() const noexcept
    {
        return framebuffer_;
    }

private:
    GLuint framebuffer_ = 0;
    GLuint texture_ = 0;
};

double pqSignalForNits(double nits) noexcept
{
    constexpr double M1 = 2610.0 / 16384.0;
    constexpr double M2 = 2523.0 / 32.0;
    constexpr double C1 = 3424.0 / 4096.0;
    constexpr double C2 = 2413.0 / 128.0;
    constexpr double C3 = 2392.0 / 128.0;
    const double luminance =
        std::clamp(nits / 10000.0, 0.0, 1.0);
    const double power = std::pow(luminance, M1);
    return std::pow(
        (C1 + C2 * power) / (1.0 + C3 * power),
        M2);
}

bool closeToByte(
    std::uint8_t actual,
    double expectedSignal) noexcept
{
    const int expected = static_cast<int>(
        std::lround(std::clamp(expectedSignal, 0.0, 1.0) * 255.0));
    return std::abs(static_cast<int>(actual) - expected) <= 8;
}

VideoFrame makeUploadFrame(AVPixelFormat format)
{
    AVFrame* native = av_frame_alloc();
    if (!native) {
        return {};
    }
    native->width = 4;
    native->height = 4;
    native->format = format;
    if (av_frame_get_buffer(native, 32) < 0) {
        av_frame_free(&native);
        return {};
    }
    switch (format) {
    case AV_PIX_FMT_YUV422P:
    case AV_PIX_FMT_YUV444P:
        std::fill_n(
            native->data[0],
            static_cast<std::size_t>(native->linesize[0]) * 4U,
            std::uint8_t { 81 });
        std::fill_n(
            native->data[1],
            static_cast<std::size_t>(native->linesize[1]) * 4U,
            std::uint8_t { 90 });
        std::fill_n(
            native->data[2],
            static_cast<std::size_t>(native->linesize[2]) * 4U,
            std::uint8_t { 240 });
        native->color_range = AVCOL_RANGE_MPEG;
        native->colorspace = AVCOL_SPC_SMPTE170M;
        break;
    case AV_PIX_FMT_NV12:
    case AV_PIX_FMT_NV21:
        std::fill_n(
            native->data[0],
            static_cast<std::size_t>(native->linesize[0]) * 4U,
            std::uint8_t { 81 });
        for (int row = 0; row < 2; ++row) {
            for (int column = 0; column < 2; ++column) {
                auto* chroma = native->data[1]
                    + row * native->linesize[1] + column * 2;
                chroma[format == AV_PIX_FMT_NV12 ? 0 : 1] = 90;
                chroma[format == AV_PIX_FMT_NV12 ? 1 : 0] = 240;
            }
        }
        native->color_range = AVCOL_RANGE_MPEG;
        native->colorspace = AVCOL_SPC_SMPTE170M;
        break;
    case AV_PIX_FMT_RGB24:
    case AV_PIX_FMT_BGR24:
        for (int row = 0; row < 4; ++row) {
            for (int column = 0; column < 4; ++column) {
                auto* pixel = native->data[0]
                    + row * native->linesize[0] + column * 3;
                pixel[0] = 0;
                pixel[1] = 0;
                pixel[2] = 0;
                pixel[format == AV_PIX_FMT_RGB24 ? 0 : 2] = 255;
            }
        }
        native->color_range = AVCOL_RANGE_JPEG;
        native->colorspace = AVCOL_SPC_RGB;
        break;
    case AV_PIX_FMT_RGBA:
    case AV_PIX_FMT_BGRA:
    case AV_PIX_FMT_ARGB:
        for (int row = 0; row < 4; ++row) {
            for (int column = 0; column < 4; ++column) {
                auto* pixel = native->data[0]
                    + row * native->linesize[0] + column * 4;
                pixel[0] = 0;
                pixel[1] = 0;
                pixel[2] = 0;
                pixel[3] = 0;
                if (format == AV_PIX_FMT_RGBA) {
                    pixel[0] = 255;
                    pixel[3] = 255;
                } else if (format == AV_PIX_FMT_BGRA) {
                    pixel[2] = 255;
                    pixel[3] = 255;
                } else {
                    pixel[0] = 255;
                    pixel[1] = 255;
                }
            }
        }
        native->color_range = AVCOL_RANGE_JPEG;
        native->colorspace = AVCOL_SPC_RGB;
        break;
    case AV_PIX_FMT_GRAY8:
        std::fill_n(
            native->data[0],
            static_cast<std::size_t>(native->linesize[0]) * 4U,
            std::uint8_t { 180 });
        native->color_range = AVCOL_RANGE_JPEG;
        native->colorspace = AVCOL_SPC_RGB;
        break;
    default:
        av_frame_free(&native);
        return {};
    }
    native->color_primaries = AVCOL_PRI_BT709;
    native->color_trc = AVCOL_TRC_BT709;
    VideoFrame result = detail::FrameFactory::video(native, 0, 0);
    av_frame_free(&native);
    return result;
}

} // namespace

bool runOpenGLOffscreenRendererChecks(
    const VideoFrame& softwareFrame,
    const VideoFrame& hdrFrame,
    std::string& error)
{
    if (!softwareFrame || !hdrFrame) {
        error = "The OpenGL ES test frames are invalid";
        return false;
    }
    constexpr int Width = 64;
    constexpr int Height = 48;
    OffscreenContext context;
    if (!context.create(Width, Height, error)) {
        return false;
    }

    std::uint64_t generation = 1;
    GLuint framebuffer = 0;
    OpenGLOutputColorSpace outputColorSpace =
        OpenGLOutputColorSpace::SdrSrgb;
    OpenGLVideoRenderer renderer([&] {
        return OpenGLRenderTarget {
            framebuffer,
            context.size(),
            generation,
            outputColorSpace,
        };
    });
    std::string rendererError;
    renderer.setEventCallback(
        [&rendererError](const VideoRenderEvent& event) {
            if (event.type != VideoRenderEventType::RedrawRequested) {
                rendererError = event.detail;
            }
        });
    std::uint64_t presentCalls = 0;
    renderer.setPresentCallback(
        [&presentCalls](std::string&) {
            ++presentCalls;
            return true;
        });

    const VideoRenderCapabilities capabilities =
        renderer.capabilities();
    const std::array<PixelFormat, 12> requiredFormats {
        PixelFormat::YUV420P,
        PixelFormat::YUV422P,
        PixelFormat::YUV444P,
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
    const bool formatsPresent = std::all_of(
        requiredFormats.begin(),
        requiredFormats.end(),
        [&capabilities](PixelFormat format) {
            return std::find(
                       capabilities.softwareFormats.begin(),
                       capabilities.softwareFormats.end(),
                       format)
                != capabilities.softwareFormats.end();
        });
    if (!formatsPresent || !capabilities.customViewport
        || !capabilities.rotation) {
        error =
            "The OpenGL ES renderer did not report the required capabilities";
        return false;
    }

    VideoRenderConfig config;
    config.surfaceSize = context.size();
    config.aspectRatio = VideoAspectRatioMode::Fit;
    if (!renderer.open(config)
        || !renderer.render(softwareFrame)
        || presentCalls != 1) {
        error = rendererError.empty()
            ? "The OpenGL ES renderer did not draw and present the software frame"
            : rendererError;
        renderer.close();
        return false;
    }
    std::vector<Pixel> pixels;
    if (!readPixels(context.size(), pixels, error)
        || !containsVisiblePixel(pixels)) {
        if (error.empty()) {
            error =
                "The OpenGL ES software-frame readback was black";
        }
        renderer.close();
        return false;
    }

    const std::array<AVPixelFormat, 9> uploadFormats {
        AV_PIX_FMT_YUV422P,
        AV_PIX_FMT_YUV444P,
        AV_PIX_FMT_NV12,
        AV_PIX_FMT_NV21,
        AV_PIX_FMT_RGB24,
        AV_PIX_FMT_BGR24,
        AV_PIX_FMT_RGBA,
        AV_PIX_FMT_BGRA,
        AV_PIX_FMT_ARGB,
    };
    for (const AVPixelFormat format : uploadFormats) {
        const VideoFrame uploadFrame = makeUploadFrame(format);
        if (!uploadFrame
            || !renderer.render(uploadFrame)
            || !readPixels(context.size(), pixels, error)
            || !containsVisiblePixel(pixels)) {
            if (error.empty()) {
                error =
                    "An OpenGL ES software upload format failed";
            }
            renderer.close();
            return false;
        }
    }
    const VideoFrame grayFrame = makeUploadFrame(AV_PIX_FMT_GRAY8);
    if (!grayFrame || !renderer.render(grayFrame)
        || !readPixels(context.size(), pixels, error)
        || !containsVisiblePixel(pixels)) {
        if (error.empty()) {
            error = "The OpenGL ES Gray8 upload check failed";
        }
        renderer.close();
        return false;
    }

    config.viewport = { 8, 6, 40, 24 };
    config.aspectRatio = VideoAspectRatioMode::Stretch;
    config.rotation = VideoRotation::Rotate180;
    ++generation;
    if (!renderer.configure(config)
        || !renderer.render(softwareFrame)
        || !readPixels(context.size(), pixels, error)
        || !isBlack(pixels.front())
        || !containsVisiblePixel(pixels)) {
        if (error.empty()) {
            error =
                "The OpenGL ES viewport, rotation, or generation check failed";
        }
        renderer.close();
        return false;
    }

    config.viewport = {};
    config.aspectRatio = VideoAspectRatioMode::Fit;
    config.rotation = VideoRotation::Rotate0;
    ++generation;
    if (!renderer.configure(config)
        || !renderer.render(hdrFrame)
        || !readPixels(context.size(), pixels, error)
        || !containsVisiblePixel(pixels)) {
        if (error.empty()) {
            error =
                "The OpenGL ES P010/PQ-to-SDR readback check failed";
        }
        renderer.close();
        return false;
    }

    const std::vector<Pixel> explicitSrgbPixels = pixels;
    {
        SrgbFramebuffer srgbFramebuffer;
        if (!srgbFramebuffer.create(Width, Height, error)) {
            renderer.close();
            return false;
        }
        framebuffer = srgbFramebuffer.handle();
        ++generation;
        if (!renderer.render(hdrFrame)
            || !readPixels(context.size(), pixels, error)
            || !pixelsClose(explicitSrgbPixels, pixels)) {
            if (error.empty()) {
                error =
                    "The OpenGL ES sRGB framebuffer encoded SDR output twice";
            }
            framebuffer = 0;
            renderer.close();
            return false;
        }
        framebuffer = 0;
        ++generation;
    }

    const auto validateHdrOutput =
        [&](OpenGLOutputColorSpace colorSpace, const char* name) {
            outputColorSpace = colorSpace;
            ++generation;
            const OpenGLRenderTarget target {
                0,
                context.size(),
                generation,
                outputColorSpace,
            };
            if (!target.isValid() || !target.isHdr()
                || !renderer.render(hdrFrame)
                || !readPixels(context.size(), pixels, error)) {
                if (error.empty()) {
                    error = std::string(
                        "The OpenGL ES native HDR target failed for ")
                        + name;
                }
                return false;
            }
            constexpr std::array<double, 4> SampleNits {
                0.0,
                10.0,
                100.0,
                400.0,
            };
            constexpr std::array<int, 4> SampleX {
                Width / 8,
                Width * 3 / 8,
                Width * 5 / 8,
                Width * 7 / 8,
            };
            int previousSignal = -1;
            for (std::size_t index = 0;
                 index < SampleNits.size();
                 ++index) {
                const Pixel& actual =
                    pixels[static_cast<std::size_t>(Height / 2) * Width
                        + static_cast<std::size_t>(SampleX[index])];
                const bool neutral =
                    std::abs(
                        static_cast<int>(actual.red)
                        - static_cast<int>(actual.green)) <= 3
                    && std::abs(
                        static_cast<int>(actual.red)
                        - static_cast<int>(actual.blue)) <= 3;
                const bool increasing =
                    static_cast<int>(actual.red) > previousSignal;
                bool encodingValid = neutral && increasing
                    && actual.alpha >= 239;
                if (colorSpace == OpenGLOutputColorSpace::HDR10PQ) {
                    encodingValid = encodingValid
                        && closeToByte(
                            actual.red,
                            pqSignalForNits(SampleNits[index]))
                        && closeToByte(
                            actual.green,
                            pqSignalForNits(SampleNits[index]))
                        && closeToByte(
                            actual.blue,
                            pqSignalForNits(SampleNits[index]));
                } else {
                    // libplacebo applies the HLG OOTF/system gamma as part of
                    // its display-referred conversion. Verify the native HLG
                    // target remains neutral and monotonic without duplicating
                    // libplacebo's color pipeline in this test.
                    encodingValid = encodingValid
                        && (index != 0 || actual.red <= 8)
                        && (index != SampleNits.size() - 1
                            || actual.red >= 128);
                }
                if (!encodingValid) {
                    error =
                        std::string(
                            "The libplacebo OpenGL HDR output check failed for ")
                        + name + " sample " + std::to_string(index)
                        + ": got RGB("
                        + std::to_string(actual.red) + ','
                        + std::to_string(actual.green) + ','
                        + std::to_string(actual.blue) + ')';
                    return false;
                }
                previousSignal = actual.red;
            }
            return true;
        };
    if (!validateHdrOutput(
            OpenGLOutputColorSpace::HDR10PQ,
            "BT.2020/PQ")
        || !validateHdrOutput(
            OpenGLOutputColorSpace::HDR10HLG,
            "BT.2020/HLG")) {
        renderer.close();
        return false;
    }
    renderer.close();
    return true;
}

} // namespace qtav::test
