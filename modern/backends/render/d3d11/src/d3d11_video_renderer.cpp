// SPDX-License-Identifier: LGPL-2.1-or-later

#if !defined(NOMINMAX)
#  define NOMINMAX
#endif

#include <qtav/d3d11_video_renderer.h>

#include <libplacebo/d3d11.h>
#include <libplacebo/log.h>
#include <libplacebo/renderer.h>
#include <libplacebo/shaders/custom.h>

#include <dxgi1_6.h>
#include <wrl/client.h>

extern "C" {
#include <libavutil/frame.h>
#include <libavutil/pixfmt.h>
}

#include "d3d11_video_renderer_p.h"
#include "frame_internal.h"
#include "qtav_libplacebo_ffmpeg_bridge.h"

#include <algorithm>
#include <array>
#include <atomic>
#include <chrono>
#include <cmath>
#include <cstdint>
#include <cwchar>
#include <deque>
#include <iomanip>
#include <memory>
#include <mutex>
#include <sstream>
#include <string>
#include <utility>
#include <vector>

namespace qtav {
namespace {

using Microsoft::WRL::ComPtr;

std::int64_t steadyMicroseconds() noexcept
{
    return std::chrono::duration_cast<std::chrono::microseconds>(
               std::chrono::steady_clock::now().time_since_epoch())
        .count();
}

void updateMaximum(
    std::atomic<std::int64_t>& destination,
    std::int64_t value) noexcept
{
    auto current = destination.load(std::memory_order_relaxed);
    while (current < value
           && !destination.compare_exchange_weak(
               current,
               value,
               std::memory_order_relaxed,
               std::memory_order_relaxed)) {
    }
}

bool isSupportedConfig(const VideoRenderConfig& config) noexcept
{
    if (!config.surfaceSize.isValid()
        || config.deviceOwnership != NativeResourceOwnership::Borrowed
        || config.contextOwnership != NativeResourceOwnership::Borrowed
        || config.surfaceOwnership != NativeResourceOwnership::Borrowed) {
        return false;
    }
    if (!config.viewport.isValid()) {
        return config.viewport.x == 0 && config.viewport.y == 0
            && config.viewport.width == 0 && config.viewport.height == 0;
    }
    const auto& viewport = config.viewport;
    return viewport.x >= 0 && viewport.y >= 0
        && viewport.x <= config.surfaceSize.width - viewport.width
        && viewport.y <= config.surfaceSize.height - viewport.height;
}

bool isSupportedTargetFormat(DXGI_FORMAT format) noexcept
{
    switch (format) {
    case DXGI_FORMAT_B8G8R8A8_UNORM:
    case DXGI_FORMAT_R8G8B8A8_UNORM:
    case DXGI_FORMAT_R16G16B16A16_FLOAT:
    case DXGI_FORMAT_R10G10B10A2_UNORM:
        return true;
    default:
        return false;
    }
}

std::string hresultText(const char* operation, HRESULT result)
{
    std::ostringstream stream;
    stream << operation << " failed with HRESULT 0x" << std::hex
           << std::uppercase << static_cast<unsigned long>(result);
    return stream.str();
}

bool sameAdvancedColorInfo(
    const D3D11AdvancedColorInfo& left,
    const D3D11AdvancedColorInfo& right) noexcept
{
    return left.outputColorSpace == right.outputColorSpace
        && left.swapChainColorSpace == right.swapChainColorSpace
        && left.displayColorSpace == right.displayColorSpace
        && left.monitor == right.monitor
        && left.bitsPerColor == right.bitsPerColor
        && left.sdrWhiteLevelNits == right.sdrWhiteLevelNits
        && left.minimumLuminanceNits == right.minimumLuminanceNits
        && left.maximumLuminanceNits == right.maximumLuminanceNits
        && left.maximumFullFrameLuminanceNits
            == right.maximumFullFrameLuminanceNits
        && left.displayDetected == right.displayDetected
        && left.advancedColorActive == right.advancedColorActive
        && left.swapChainColorSpaceConfigured
            == right.swapChainColorSpaceConfigured
        && left.sdrWhiteLevelFromSystem
            == right.sdrWhiteLevelFromSystem;
}

float querySdrWhiteLevelNits(const wchar_t* deviceName) noexcept
{
    if (!deviceName || !*deviceName) {
        return 0.0F;
    }

    UINT32 pathCount = 0;
    UINT32 modeCount = 0;
    if (GetDisplayConfigBufferSizes(
            QDC_ONLY_ACTIVE_PATHS,
            &pathCount,
            &modeCount)
        != ERROR_SUCCESS) {
        return 0.0F;
    }

    std::vector<DISPLAYCONFIG_PATH_INFO> paths(pathCount);
    std::vector<DISPLAYCONFIG_MODE_INFO> modes(modeCount);
    const LONG status = QueryDisplayConfig(
        QDC_ONLY_ACTIVE_PATHS,
        &pathCount,
        paths.data(),
        &modeCount,
        modes.data(),
        nullptr);
    if (status != ERROR_SUCCESS) {
        return 0.0F;
    }
    paths.resize(pathCount);

    for (const auto& path : paths) {
        DISPLAYCONFIG_SOURCE_DEVICE_NAME source {};
        source.header.type = DISPLAYCONFIG_DEVICE_INFO_GET_SOURCE_NAME;
        source.header.size = sizeof(source);
        source.header.adapterId = path.sourceInfo.adapterId;
        source.header.id = path.sourceInfo.id;
        if (DisplayConfigGetDeviceInfo(&source.header) != ERROR_SUCCESS
            || std::wcscmp(source.viewGdiDeviceName, deviceName) != 0) {
            continue;
        }

        DISPLAYCONFIG_SDR_WHITE_LEVEL white {};
        white.header.type = DISPLAYCONFIG_DEVICE_INFO_GET_SDR_WHITE_LEVEL;
        white.header.size = sizeof(white);
        white.header.adapterId = path.targetInfo.adapterId;
        white.header.id = path.targetInfo.id;
        if (DisplayConfigGetDeviceInfo(&white.header) == ERROR_SUCCESS
            && white.SDRWhiteLevel > 0) {
            return static_cast<float>(white.SDRWhiteLevel)
                * (80.0F / 1000.0F);
        }
    }
    return 0.0F;
}

HRESULT findOutputForMonitor(
    ID3D11Device* device,
    HMONITOR monitor,
    ComPtr<IDXGIOutput>& output)
{
    if (!device || !monitor) {
        return E_INVALIDARG;
    }

    ComPtr<IDXGIDevice> dxgiDevice;
    HRESULT result = device->QueryInterface(IID_PPV_ARGS(&dxgiDevice));
    if (FAILED(result)) {
        return result;
    }

    ComPtr<IDXGIAdapter> deviceAdapter;
    result = dxgiDevice->GetAdapter(&deviceAdapter);
    if (FAILED(result)) {
        return result;
    }

    ComPtr<IDXGIFactory1> factory;
    result = deviceAdapter->GetParent(IID_PPV_ARGS(&factory));
    if (FAILED(result)) {
        return result;
    }

    for (UINT adapterIndex = 0;; ++adapterIndex) {
        ComPtr<IDXGIAdapter1> adapter;
        result = factory->EnumAdapters1(adapterIndex, &adapter);
        if (result == DXGI_ERROR_NOT_FOUND) {
            return DXGI_ERROR_NOT_FOUND;
        }
        if (FAILED(result)) {
            return result;
        }

        for (UINT outputIndex = 0;; ++outputIndex) {
            ComPtr<IDXGIOutput> candidate;
            result = adapter->EnumOutputs(outputIndex, &candidate);
            if (result == DXGI_ERROR_NOT_FOUND) {
                break;
            }
            if (FAILED(result)) {
                return result;
            }

            DXGI_OUTPUT_DESC description {};
            if (FAILED(candidate->GetDesc(&description))) {
                continue;
            }
            if (description.Monitor == monitor) {
                output = std::move(candidate);
                return S_OK;
            }
        }
    }
}

bool configureAdvancedColor(
    ID3D11Device* expectedDevice,
    const D3D11RenderTarget& target,
    DXGI_FORMAT targetFormat,
    D3D11AdvancedColorInfo& info,
    std::string& error)
{
    info = {};
    if (targetFormat == DXGI_FORMAT_R16G16B16A16_FLOAT) {
        info.outputColorSpace = D3D11OutputColorSpace::ScRGB;
        info.swapChainColorSpace = DXGI_COLOR_SPACE_RGB_FULL_G10_NONE_P709;
        info.bitsPerColor = 16;
        info.maximumLuminanceNits = 1000.0F;
        info.maximumFullFrameLuminanceNits = 1000.0F;
    } else if (targetFormat == DXGI_FORMAT_R10G10B10A2_UNORM) {
        info.outputColorSpace = D3D11OutputColorSpace::HDR10;
        info.swapChainColorSpace =
            DXGI_COLOR_SPACE_RGB_FULL_G2084_NONE_P2020;
        info.bitsPerColor = 10;
        info.maximumLuminanceNits = 1000.0F;
        info.maximumFullFrameLuminanceNits = 1000.0F;
    }

    if (!target.swapChain) {
        return true;
    }

    ComPtr<ID3D11Device> swapChainDevice;
    HRESULT result = target.swapChain->GetDevice(
        IID_PPV_ARGS(&swapChainDevice));
    if (FAILED(result) || swapChainDevice.Get() != expectedDevice) {
        error = "The current D3D11 swap chain belongs to another device";
        return false;
    }

    DXGI_SWAP_CHAIN_DESC1 swapDescription {};
    result = target.swapChain->GetDesc1(&swapDescription);
    if (FAILED(result)) {
        error = hresultText("IDXGISwapChain1::GetDesc1", result);
        return false;
    }
    if (swapDescription.Format != targetFormat
        || (swapDescription.SwapEffect != DXGI_SWAP_EFFECT_FLIP_DISCARD
            && swapDescription.SwapEffect
                != DXGI_SWAP_EFFECT_FLIP_SEQUENTIAL)) {
        error =
            "Advanced Color requires a matching flip-model D3D11 swap chain";
        return false;
    }

    ComPtr<IDXGIOutput> output;
    result = target.monitor
        ? findOutputForMonitor(expectedDevice, target.monitor, output)
        : target.swapChain->GetContainingOutput(&output);
    if (FAILED(result) || !output) {
        error = target.monitor
            ? hresultText(
                  "DXGI output lookup for the current monitor",
                  result)
            : hresultText(
                  "IDXGISwapChain::GetContainingOutput",
                  result);
        return false;
    }

    ComPtr<IDXGIOutput6> output6;
    result = output.As(&output6);
    if (FAILED(result) || !output6) {
        error =
            "The current display does not expose IDXGIOutput6 Advanced Color information";
        return false;
    }

    DXGI_OUTPUT_DESC1 outputDescription {};
    result = output6->GetDesc1(&outputDescription);
    if (FAILED(result)) {
        error = hresultText("IDXGIOutput6::GetDesc1", result);
        return false;
    }

    info.displayDetected = true;
    info.monitor = outputDescription.Monitor;
    info.bitsPerColor = static_cast<int>(outputDescription.BitsPerColor);
    info.displayColorSpace = outputDescription.ColorSpace;
    info.minimumLuminanceNits = outputDescription.MinLuminance;
    info.maximumLuminanceNits = outputDescription.MaxLuminance;
    info.maximumFullFrameLuminanceNits =
        outputDescription.MaxFullFrameLuminance;
    info.advancedColorActive =
        outputDescription.ColorSpace
        == DXGI_COLOR_SPACE_RGB_FULL_G2084_NONE_P2020;

    const float queriedSdrWhite =
        querySdrWhiteLevelNits(outputDescription.DeviceName);
    if (queriedSdrWhite > 0.0F) {
        info.sdrWhiteLevelNits = queriedSdrWhite;
        info.sdrWhiteLevelFromSystem = true;
    }

    if (targetFormat == DXGI_FORMAT_R10G10B10A2_UNORM
        && !info.advancedColorActive) {
        info.outputColorSpace = D3D11OutputColorSpace::SDR;
        info.swapChainColorSpace =
            DXGI_COLOR_SPACE_RGB_FULL_G22_NONE_P709;
    }
    if (!info.advancedColorActive
        || info.outputColorSpace == D3D11OutputColorSpace::SDR) {
        info.maximumLuminanceNits = info.sdrWhiteLevelNits;
        info.maximumFullFrameLuminanceNits = info.sdrWhiteLevelNits;
    } else {
        if (!(info.maximumLuminanceNits > 0.0F)) {
            info.maximumLuminanceNits = 1000.0F;
        }
        if (!(info.maximumFullFrameLuminanceNits > 0.0F)) {
            info.maximumFullFrameLuminanceNits =
                info.maximumLuminanceNits;
        }
    }

    UINT colorSpaceSupport = 0;
    result = target.swapChain->CheckColorSpaceSupport(
        info.swapChainColorSpace,
        &colorSpaceSupport);
    if (FAILED(result)
        || !(colorSpaceSupport
            & DXGI_SWAP_CHAIN_COLOR_SPACE_SUPPORT_FLAG_PRESENT)) {
        error =
            "The current flip-model swap chain cannot present the required Advanced Color space";
        return false;
    }
    result = target.swapChain->SetColorSpace1(info.swapChainColorSpace);
    if (FAILED(result)) {
        error = hresultText("IDXGISwapChain3::SetColorSpace1", result);
        return false;
    }
    info.swapChainColorSpaceConfigured = true;
    return true;
}

bool targetDescription(
    ID3D11Device* expectedDevice,
    ID3D11RenderTargetView* view,
    ComPtr<ID3D11Texture2D>& texture,
    D3D11_TEXTURE2D_DESC& description,
    DXGI_FORMAT& viewFormat,
    std::string& error)
{
    if (!view) {
        error = "The current D3D11 render target is unavailable";
        return false;
    }
    ComPtr<ID3D11Resource> resource;
    view->GetResource(&resource);
    if (!resource || FAILED(resource.As(&texture)) || !texture) {
        error = "The current D3D11 render target is not a 2D texture";
        return false;
    }
    ComPtr<ID3D11Device> targetDevice;
    texture->GetDevice(&targetDevice);
    if (targetDevice.Get() != expectedDevice) {
        error = "The current D3D11 render target belongs to another device";
        return false;
    }
    D3D11_RENDER_TARGET_VIEW_DESC viewDescription {};
    view->GetDesc(&viewDescription);
    if (viewDescription.ViewDimension != D3D11_RTV_DIMENSION_TEXTURE2D) {
        error = "The current D3D11 render target is not a 2D texture view";
        return false;
    }
    texture->GetDesc(&description);
    viewFormat = viewDescription.Format == DXGI_FORMAT_UNKNOWN
        ? description.Format
        : viewDescription.Format;
    return true;
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

pl_color_levels levels(ColorRange value) noexcept
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

pl_color_system system(ColorMatrix value) noexcept
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

pl_color_primaries primaries(ColorPrimaries value) noexcept
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

pl_color_transfer transfer(ColorTransfer value) noexcept
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

pl_chroma_location chromaLocation(ChromaLocation value) noexcept
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
    pl_hdr_metadata& destination) noexcept
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
        destination.max_luma =
            static_cast<float>(mastering.maximumLuminance);
    }
    const ContentLightMetadata light = frame.contentLightMetadata();
    destination.max_cll =
        static_cast<float>(light.maximumContentLightLevel);
    destination.max_fall =
        static_cast<float>(light.maximumFrameAverageLightLevel);
}

void setSourceColor(const VideoFrame& source, pl_frame& frame) noexcept
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
    DXGI_FORMAT format,
    const D3D11AdvancedColorInfo& info,
    pl_frame& target) noexcept
{
    target.repr.sys = PL_COLOR_SYSTEM_RGB;
    target.repr.levels = PL_COLOR_LEVELS_FULL;
    target.repr.alpha = PL_ALPHA_NONE;

    switch (info.outputColorSpace) {
    case D3D11OutputColorSpace::ScRGB:
        target.color.primaries = PL_COLOR_PRIM_BT_709;
        target.color.transfer = PL_COLOR_TRC_LINEAR;
        break;
    case D3D11OutputColorSpace::HDR10:
        target.color = pl_color_space_hdr10;
        break;
    case D3D11OutputColorSpace::SDR:
    default:
        target.color = pl_color_space_srgb;
        break;
    }

    target.color.hdr.min_luma =
        std::max(info.minimumLuminanceNits, 0.0F);
    target.color.hdr.max_luma =
        std::max(info.maximumLuminanceNits, 1.0F);
    target.color.hdr.max_cll = target.color.hdr.max_luma;
    target.color.hdr.max_fall = std::max(
        info.maximumFullFrameLuminanceNits,
        1.0F);

    if (format == DXGI_FORMAT_R16G16B16A16_FLOAT) {
        target.repr.bits = { 16, 16, 0 };
    } else if (format == DXGI_FORMAT_R10G10B10A2_UNORM) {
        target.repr.bits = { 10, 10, 0 };
    } else {
        target.repr.bits = { 8, 8, 0 };
    }
}

void applyGeometry(
    pl_frame& image,
    pl_frame& target,
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
    pl_plane& plane,
    pl_tex texture,
    int components,
    int firstComponent) noexcept
{
    plane.texture = texture;
    plane.components = components;
    std::fill(std::begin(plane.component_mapping),
              std::end(plane.component_mapping),
              PL_CHANNEL_NONE);
    for (int index = 0; index < components; ++index) {
        plane.component_mapping[index] = firstComponent + index;
    }
}

AVPixelFormat avPixelFormat(PixelFormat format) noexcept
{
    switch (format) {
    case PixelFormat::YUV420P:
        return AV_PIX_FMT_YUV420P;
    case PixelFormat::YUV422P:
        return AV_PIX_FMT_YUV422P;
    case PixelFormat::YUV444P:
        return AV_PIX_FMT_YUV444P;
    case PixelFormat::NV12:
        return AV_PIX_FMT_NV12;
    case PixelFormat::NV21:
        return AV_PIX_FMT_NV21;
    case PixelFormat::P010:
        return AV_PIX_FMT_P010LE;
    case PixelFormat::RGB24:
        return AV_PIX_FMT_RGB24;
    case PixelFormat::BGR24:
        return AV_PIX_FMT_BGR24;
    case PixelFormat::RGBA:
        return AV_PIX_FMT_RGBA;
    case PixelFormat::BGRA:
        return AV_PIX_FMT_BGRA;
    case PixelFormat::ARGB:
        return AV_PIX_FMT_ARGB;
    case PixelFormat::Gray8:
        return AV_PIX_FMT_GRAY8;
    default:
        return AV_PIX_FMT_NONE;
    }
}

struct AVFrameDeleter {
    void operator()(AVFrame* frame) const noexcept
    {
        av_frame_free(&frame);
    }
};

std::unique_ptr<AVFrame, AVFrameDeleter> mappedAvFrame(
    const HardwareFrameMapping& mapping) noexcept
{
    const AVPixelFormat format = avPixelFormat(mapping.format());
    if (format == AV_PIX_FMT_NONE || mapping.width() <= 0
        || mapping.height() <= 0 || mapping.planeCount() <= 0
        || mapping.planeCount() > AV_NUM_DATA_POINTERS) {
        return {};
    }
    std::unique_ptr<AVFrame, AVFrameDeleter> frame(av_frame_alloc());
    if (!frame) {
        return {};
    }
    frame->format = format;
    frame->width = mapping.width();
    frame->height = mapping.height();
    for (int plane = 0; plane < mapping.planeCount(); ++plane) {
        frame->data[plane] = const_cast<std::uint8_t*>(mapping.data(plane));
        frame->linesize[plane] = mapping.lineSize(plane);
        if (!frame->data[plane] || frame->linesize[plane] == 0) {
            return {};
        }
    }
    return frame;
}

} // namespace

bool D3D11AdvancedColorInfo::isHdrOutput() const noexcept
{
    return advancedColorActive
        && outputColorSpace != D3D11OutputColorSpace::SDR;
}

bool D3D11RenderTarget::isValid() const noexcept
{
    return view != nullptr;
}

D3D11TextureFrame::~D3D11TextureFrame() = default;

UINT D3D11TextureFrame::arraySlice() const noexcept
{
    return 0;
}

DXGI_FORMAT D3D11TextureFrame::dxgiFormat() const noexcept
{
    return format() == PixelFormat::BGRA
        ? DXGI_FORMAT_B8G8R8A8_UNORM
        : format() == PixelFormat::RGBA
        ? DXGI_FORMAT_R8G8B8A8_UNORM
        : format() == PixelFormat::NV12
        ? DXGI_FORMAT_NV12
        : format() == PixelFormat::P010
        ? DXGI_FORMAT_P010
        : DXGI_FORMAT_UNKNOWN;
}

DXGI_COLOR_SPACE_TYPE D3D11TextureFrame::colorSpace() const noexcept
{
    return DXGI_COLOR_SPACE_RGB_FULL_G22_NONE_P709;
}

D3D11HardwareFrameInterop::~D3D11HardwareFrameInterop() = default;

std::shared_ptr<D3D11TextureFrame>
D3D11HardwareFrameInterop::importFrame(
    const HardwareFrame& frame,
    const VideoColorSpace&)
{
    return importFrame(frame);
}

class D3D11VideoRenderer::Impl {
public:
    struct InFlightRender {
        ComPtr<ID3D11Query> completion;
        ComPtr<ID3D11Texture2D> target;
        VideoFrame source;
        std::shared_ptr<D3D11TextureFrame> imported;
        std::array<pl_tex, 2> sourceTextures {};
        pl_tex targetTexture = nullptr;
    };

    struct RenderPassProbe {
        std::uint64_t graphHash = 14695981039346656037ULL;
        std::uint64_t passCount = 0;
        std::uint64_t gpuFrameNanoseconds = 0;
        std::uint64_t maximumGpuPassNanoseconds = 0;
        std::int64_t cpuStartedMicroseconds = 0;
        std::int64_t firstCallbackMicroseconds = 0;
        std::int64_t lastCallbackMicroseconds = 0;
    };

    static constexpr std::size_t maximumInFlightRenders = 3;

    Impl(
        std::shared_ptr<D3D11DeviceAccess> deviceAccess,
        D3D11CurrentTargetCallback currentTarget)
        : deviceAccess_(std::move(deviceAccess))
        , device_(deviceAccess_ ? deviceAccess_->device()
                                : BorrowedD3D11Device {})
        , context_(deviceAccess_ ? deviceAccess_->immediateContext()
                                 : BorrowedD3D11DeviceContext {})
        , currentTarget_(std::move(currentTarget))
    {
        scRgbOutputHook_.stages = PL_HOOK_OUTPUT;
        scRgbOutputHook_.input = PL_HOOK_SIG_COLOR;
        scRgbOutputHook_.hook = &Impl::scaleScRgbOutput;
        scRgbOutputHook_.signature = 0x7174617653435247ULL;
    }

    ~Impl()
    {
        close();
    }

    void notify(VideoRenderEventType type, std::string detail)
    {
        EventCallback callback;
        {
            std::lock_guard<std::mutex> lock(stateMutex_);
            callback = eventCallback_;
        }
        if (callback) {
            callback({ type, std::move(detail) });
        }
    }

    static void logCallback(
        void* privateData,
        pl_log_level level,
        const char* message)
    {
        if (!privateData || !message || level > PL_LOG_ERR) {
            return;
        }
        auto* self = static_cast<Impl*>(privateData);
        std::lock_guard<std::mutex> lock(self->logMutex_);
        self->lastLogError_ = message;
    }

    static void renderInfoCallback(
        void* privateData,
        const pl_render_info* info) noexcept
    {
        if (!privateData || !info || !info->pass) {
            return;
        }
        auto* probe = static_cast<RenderPassProbe*>(privateData);
        const auto callbackMicroseconds = steadyMicroseconds();
        if (probe->firstCallbackMicroseconds == 0) {
            probe->firstCallbackMicroseconds = callbackMicroseconds;
        }
        probe->lastCallbackMicroseconds = callbackMicroseconds;
        const auto mixHash = [&probe](std::uint64_t value) {
            probe->graphHash ^= value;
            probe->graphHash *= 1099511628211ULL;
        };
        mixHash(info->pass->signature);
        mixHash(static_cast<std::uint64_t>(info->stage));
        mixHash(static_cast<std::uint64_t>(info->index));
        mixHash(static_cast<std::uint64_t>(info->count));
        ++probe->passCount;
        probe->gpuFrameNanoseconds += info->pass->last;
        probe->maximumGpuPassNanoseconds = std::max(
            probe->maximumGpuPassNanoseconds,
            info->pass->last);
    }

    static pl_hook_res scaleScRgbOutput(
        void*,
        const pl_hook_params* params)
    {
        pl_hook_res result {};
        if (!params || !params->sh) {
            result.failed = true;
            return result;
        }

        // libplacebo's normalized linear-light convention uses 203 nits for
        // 1.0, while Windows scRGB defines 1.0 as 80 nits. Convert between
        // those physical encodings after libplacebo's color management.
        const float scale = PL_COLOR_SDR_WHITE / 80.0F;
        const pl_shader_var variable {
            pl_var_float("qtav_sc_rgb_scale"),
            &scale,
            false,
        };
        pl_custom_shader shader {};
        shader.description = "Windows scRGB absolute-luminance scale";
        shader.body = "color.rgb *= qtav_sc_rgb_scale;";
        shader.input = PL_SHADER_SIG_COLOR;
        shader.output = PL_SHADER_SIG_COLOR;
        shader.variables = &variable;
        shader.num_variables = 1;
        if (!pl_shader_custom(params->sh, &shader)) {
            result.failed = true;
            return result;
        }
        result.output = PL_HOOK_SIG_COLOR;
        result.sh = params->sh;
        result.repr = params->repr;
        result.color = params->color;
        result.components = params->components;
        result.rect = params->rect;
        return result;
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

    bool createCommon(std::string& error)
    {
        pl_log_params logParams {};
        logParams.log_cb = &Impl::logCallback;
        logParams.log_priv = this;
        logParams.log_level = PL_LOG_ERR;
        log_ = pl_log_create(PL_API_VER, &logParams);

        pl_d3d11_params d3d11Params {};
        d3d11Params.device = device_.get();
        d3d11_ = pl_d3d11_create(log_, &d3d11Params);
        if (!d3d11_) {
            error = takeLogError(
                "libplacebo could not import the borrowed D3D11 device");
            destroyCommon();
            return false;
        }
        renderer_ = pl_renderer_create(log_, d3d11_->gpu);
        if (!renderer_) {
            error = takeLogError(
                "libplacebo could not create its D3D11 renderer");
            destroyCommon();
            return false;
        }
        return true;
    }

    void destroyInFlightRender(InFlightRender& render) noexcept
    {
        for (pl_tex& texture : render.sourceTextures) {
            if (texture && d3d11_) {
                pl_tex_destroy(d3d11_->gpu, &texture);
            }
        }
        if (render.targetTexture && d3d11_) {
            pl_tex_destroy(d3d11_->gpu, &render.targetTexture);
        }
        render.imported.reset();
        render.source = {};
        render.target.Reset();
    }

    void destroyInFlightRenders() noexcept
    {
        for (InFlightRender& render : inFlightRenders_) {
            destroyInFlightRender(render);
        }
        inFlightRenders_.clear();
        availableCompletionQueries_.clear();
    }

    bool retireCompletedRenders(std::string& error)
    {
        const auto retire = [&] {
            while (!inFlightRenders_.empty()) {
                InFlightRender& render = inFlightRenders_.front();
                BOOL complete = FALSE;
                const HRESULT result = context_.get()->GetData(
                    render.completion.Get(),
                    &complete,
                    sizeof(complete),
                    D3D11_ASYNC_GETDATA_DONOTFLUSH);
                if (result == S_FALSE || (result == S_OK && !complete)) {
                    break;
                }
                if (FAILED(result)) {
                    error = hresultText(
                        "Polling D3D11 render completion",
                        result);
                    return false;
                }
                ComPtr<ID3D11Query> completion =
                    std::move(render.completion);
                destroyInFlightRender(render);
                inFlightRenders_.pop_front();
                availableCompletionQueries_.push_back(
                    std::move(completion));
            }
            return true;
        };

        if (!retire()) {
            return false;
        }
        if (inFlightRenders_.size() >= maximumInFlightRenders) {
            // Present normally submits the immediate-context command stream.
            // A non-blocking Present can be rejected while the queue is full,
            // though, so force submission before applying backpressure. This
            // does not wait for GPU completion.
            context_.get()->Flush();
            return retire();
        }
        return true;
    }

    bool acquireCompletionQuery(
        ComPtr<ID3D11Query>& completion,
        std::string& error)
    {
        if (!availableCompletionQueries_.empty()) {
            completion = std::move(availableCompletionQueries_.back());
            availableCompletionQueries_.pop_back();
            return true;
        }
        D3D11_QUERY_DESC description {};
        description.Query = D3D11_QUERY_EVENT;
        const HRESULT result = device_.get()->CreateQuery(
            &description,
            &completion);
        if (FAILED(result)) {
            error = hresultText(
                "Creating a D3D11 render-completion query",
                result);
            return false;
        }
        return true;
    }

    void destroyCommon() noexcept
    {
        if (d3d11_) {
            pl_gpu_finish(d3d11_->gpu);
        }
        destroyInFlightRenders();
        for (pl_tex& texture : uploadTextures_) {
            if (texture && d3d11_) {
                pl_tex_destroy(d3d11_->gpu, &texture);
            }
        }
        if (renderer_) {
            pl_renderer_destroy(&renderer_);
        }
        if (d3d11_) {
            pl_d3d11_destroy(&d3d11_);
        }
        if (log_) {
            pl_log_destroy(&log_);
        }
    }

    void close() noexcept
    {
        {
            std::lock_guard<std::mutex> stateLock(stateMutex_);
            open_ = false;
            config_ = {};
        }
        std::lock_guard<std::mutex> renderLock(renderMutex_);
        if (!deviceAccess_) {
            destroyCommon();
            return;
        }
        auto contextGuard = deviceAccess_->contextGuard();
        (void)contextGuard;
        destroyCommon();
    }

    bool wrapTarget(
        ID3D11Texture2D* native,
        DXGI_FORMAT format,
        int width,
        int height,
        const D3D11AdvancedColorInfo& color,
        pl_tex& texture,
        pl_frame& frame,
        std::string& error)
    {
        pl_d3d11_wrap_params params {};
        params.tex = native;
        params.array_slice = 0;
        params.fmt = format;
        params.w = width;
        params.h = height;
        texture = pl_d3d11_wrap(d3d11_->gpu, &params);
        if (!texture || !texture->params.renderable) {
            error = takeLogError(
                "libplacebo cannot wrap the D3D11 render target");
            return false;
        }
        frame.num_planes = 1;
        initializePlane(frame.planes[0], texture, 4, 0);
        frame.crop = {
            0.0F,
            0.0F,
            static_cast<float>(width),
            static_cast<float>(height),
        };
        setTargetColor(format, color, frame);
        return true;
    }

    bool wrapHardwareSource(
        const VideoFrame& source,
        const std::shared_ptr<D3D11TextureFrame>& imported,
        std::array<pl_tex, 2>& textures,
        pl_frame& frame,
        pl_dovi_metadata& dovi,
        std::string& error)
    {
        if (!imported || !imported->texture()
            || imported->width() != source.width()
            || imported->height() != source.height()) {
            error =
                "The imported D3D11 texture does not match the decoded frame";
            return false;
        }

        D3D11_TEXTURE2D_DESC description {};
        imported->texture()->GetDesc(&description);
        if (description.Usage != D3D11_USAGE_DEFAULT
            || description.MipLevels != 1
            || description.SampleDesc.Count != 1
            || imported->arraySlice() >= description.ArraySize
            || static_cast<UINT>(source.width()) > description.Width
            || static_cast<UINT>(source.height()) > description.Height
            || !(description.BindFlags & D3D11_BIND_SHADER_RESOURCE)) {
            error =
                "The imported D3D11 texture is not a shader-readable default resource";
            return false;
        }

        const PixelFormat format = imported->format();
        const bool rawYuv = format == PixelFormat::NV12
            || format == PixelFormat::P010;
        if (source.hasDolbyVisionMetadata() && !rawYuv) {
            error =
                "Dolby Vision requires raw NV12/P010 D3D11 plane sampling";
            return false;
        }

        if (rawYuv) {
            const bool tenBit = format == PixelFormat::P010;
            if (description.Format
                != (tenBit ? DXGI_FORMAT_P010 : DXGI_FORMAT_NV12)) {
                error =
                    "The imported D3D11 decoder texture format is inconsistent";
                return false;
            }
            const std::array<DXGI_FORMAT, 2> planeFormats {
                tenBit ? DXGI_FORMAT_R16_UNORM : DXGI_FORMAT_R8_UNORM,
                tenBit ? DXGI_FORMAT_R16G16_UNORM
                       : DXGI_FORMAT_R8G8_UNORM,
            };
            const std::array<int, 2> widths {
                static_cast<int>(description.Width),
                static_cast<int>((description.Width + 1U) / 2U),
            };
            const std::array<int, 2> heights {
                static_cast<int>(description.Height),
                static_cast<int>((description.Height + 1U) / 2U),
            };
            for (std::size_t plane = 0; plane < textures.size(); ++plane) {
                pl_d3d11_wrap_params params {};
                params.tex = imported->texture();
                params.array_slice =
                    static_cast<int>(imported->arraySlice());
                params.fmt = planeFormats[plane];
                params.w = widths[plane];
                params.h = heights[plane];
                textures[plane] = pl_d3d11_wrap(d3d11_->gpu, &params);
                if (!textures[plane] || !textures[plane]->params.sampleable) {
                    error = takeLogError(
                        "libplacebo cannot wrap a D3D11 decoder plane");
                    return false;
                }
            }
            frame.num_planes = 2;
            initializePlane(frame.planes[0], textures[0], 1, 0);
            initializePlane(frame.planes[1], textures[1], 2, 1);
            frame.crop = {
                0.0F,
                0.0F,
                static_cast<float>(source.width()),
                static_cast<float>(source.height()),
            };
            setSourceColor(source, frame);
            frame.repr.bits = tenBit
                ? pl_bit_encoding { 16, 10, 6 }
                : pl_bit_encoding { 8, 8, 0 };

            const AVFrame* native =
                detail::FrameFactory::nativeVideoFrame(source);
            if (source.hasDolbyVisionMetadata()
                && (!native
                    || !qtav_pl_map_dovi(&frame, &dovi, native))) {
                error =
                    "FFmpeg Dolby Vision metadata changed during D3D11 mapping";
                return false;
            }
        } else {
            pl_d3d11_wrap_params params {};
            params.tex = imported->texture();
            params.array_slice = static_cast<int>(imported->arraySlice());
            params.fmt = imported->dxgiFormat();
            params.w = source.width();
            params.h = source.height();
            textures[0] = pl_d3d11_wrap(d3d11_->gpu, &params);
            if (!textures[0] || !textures[0]->params.sampleable) {
                error = takeLogError(
                    "libplacebo cannot wrap the imported D3D11 texture");
                return false;
            }
            frame.num_planes = 1;
            initializePlane(
                frame.planes[0],
                textures[0],
                std::min(textures[0]->params.format->num_components, 4),
                0);
            if (imported->format() == PixelFormat::BGRA) {
                frame.planes[0].component_mapping[0] = 2;
                frame.planes[0].component_mapping[1] = 1;
                frame.planes[0].component_mapping[2] = 0;
                frame.planes[0].component_mapping[3] = 3;
            }
            frame.repr.sys = PL_COLOR_SYSTEM_RGB;
            frame.repr.levels = PL_COLOR_LEVELS_FULL;
            frame.repr.alpha = PL_ALPHA_NONE;
            switch (imported->colorSpace()) {
            case DXGI_COLOR_SPACE_RGB_FULL_G10_NONE_P709:
                frame.color.primaries = PL_COLOR_PRIM_BT_709;
                frame.color.transfer = PL_COLOR_TRC_LINEAR;
                break;
            case DXGI_COLOR_SPACE_RGB_FULL_G2084_NONE_P2020:
                frame.color = pl_color_space_hdr10;
                break;
            default:
                frame.color = pl_color_space_srgb;
                break;
            }
        }

        frame.crop = {
            0.0F,
            0.0F,
            static_cast<float>(source.width()),
            static_cast<float>(source.height()),
        };
        return true;
    }

    mutable std::mutex stateMutex_;
    std::mutex renderMutex_;
    std::mutex logMutex_;
    std::shared_ptr<D3D11DeviceAccess> deviceAccess_;
    BorrowedD3D11Device device_;
    BorrowedD3D11DeviceContext context_;
    D3D11CurrentTargetCallback currentTarget_;
    std::shared_ptr<D3D11HardwareFrameInterop> hardwareInterop_;
    EventCallback eventCallback_;
    VideoRenderConfig config_;
    D3D11AdvancedColorInfo advancedColorInfo_;
    bool allowSoftwareMappingFallback_ = false;
    bool open_ = false;

    pl_log log_ = nullptr;
    pl_d3d11 d3d11_ = nullptr;
    pl_renderer renderer_ = nullptr;
    pl_hook scRgbOutputHook_ {};
    std::array<pl_tex, 4> uploadTextures_ {};
    std::deque<InFlightRender> inFlightRenders_;
    std::vector<ComPtr<ID3D11Query>> availableCompletionQueries_;
    std::string lastLogError_;

    std::atomic<std::uint64_t> decoderSurfaceCopies_ { 0 };
    std::atomic<std::uint64_t> stateBusyRenderAttempts_ { 0 };
    std::atomic<std::uint64_t> serializationBusyRenderAttempts_ { 0 };
    std::atomic<std::uint64_t> deviceContextBusyRenderAttempts_ { 0 };
    std::atomic<std::uint64_t>
        reservationAwareContextBusyRenderAttempts_ { 0 };
    std::atomic<std::uint64_t> unreservedContextBusyRenderAttempts_ { 0 };
    std::atomic<std::uint64_t> inFlightBusyRenderAttempts_ { 0 };
    std::atomic<std::int64_t> maximumColorSetupMicroseconds_ { 0 };
    std::atomic<std::int64_t> maximumInteropMicroseconds_ { 0 };
    std::atomic<std::int64_t> maximumBufferUpdateMicroseconds_ { 0 };
    std::atomic<std::int64_t> maximumDrawMicroseconds_ { 0 };
    std::atomic<std::int64_t> maximumRetireCompletedMicroseconds_ { 0 };
    std::atomic<std::int64_t>
        maximumCompletionQueryAcquireMicroseconds_ { 0 };
    std::atomic<std::int64_t> maximumClearMicroseconds_ { 0 };
    std::atomic<std::int64_t> maximumPlRenderImageMicroseconds_ { 0 };
    std::atomic<std::int64_t>
        maximumCompletionQueryEndMicroseconds_ { 0 };
    std::atomic<std::int64_t> maximumInFlightRetentionMicroseconds_ { 0 };
    std::atomic<std::int64_t> maximumLibplaceboPassesPerRender_ { 0 };
    std::atomic<std::uint64_t> libplaceboPassGraphChanges_ { 0 };
    std::atomic<std::int64_t>
        maximumLibplaceboGpuFrameMicroseconds_ { 0 };
    std::atomic<std::int64_t>
        maximumLibplaceboGpuPassMicroseconds_ { 0 };
    std::atomic<std::int64_t>
        maximumLibplaceboCallbackArrivalMicroseconds_ { 0 };
    std::atomic<std::int64_t>
        maximumLibplaceboPostCallbackMicroseconds_ { 0 };
    std::uint64_t previousLibplaceboPassGraphHash_ = 0;
    std::uint64_t previousLibplaceboPassCount_ = 0;
    bool havePreviousLibplaceboPassGraph_ = false;
};

D3D11VideoRenderer::D3D11VideoRenderer(
    BorrowedD3D11Device device,
    BorrowedD3D11DeviceContext context,
    D3D11CurrentTargetCallback currentTarget)
    : impl_(std::make_unique<Impl>(
          D3D11DeviceAccess::create(device, context),
          std::move(currentTarget)))
{
}

D3D11VideoRenderer::D3D11VideoRenderer(
    std::shared_ptr<D3D11DeviceAccess> deviceAccess,
    D3D11CurrentTargetCallback currentTarget)
    : impl_(std::make_unique<Impl>(
          std::move(deviceAccess),
          std::move(currentTarget)))
{
}

D3D11VideoRenderer::~D3D11VideoRenderer() = default;
D3D11VideoRenderer::D3D11VideoRenderer(D3D11VideoRenderer&&) noexcept =
    default;
D3D11VideoRenderer& D3D11VideoRenderer::operator=(
    D3D11VideoRenderer&&) noexcept = default;

VideoRenderCapabilities D3D11VideoRenderer::capabilities() const
{
    VideoRenderCapabilities result;
    result.softwareFormats = {
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
    std::shared_ptr<D3D11HardwareFrameInterop> hardwareInterop;
    if (impl_) {
        std::lock_guard<std::mutex> lock(impl_->stateMutex_);
        hardwareInterop = impl_->hardwareInterop_;
    }
    if (hardwareInterop && impl_
        && hardwareInterop->deviceAccess() == impl_->deviceAccess_) {
        result.hardwareDevices =
            hardwareInterop->capabilities().sourceDevices;
    }
    result.customViewport = true;
    result.rotation = true;
    return result;
}

void D3D11VideoRenderer::setEventCallback(EventCallback callback)
{
    if (!impl_) {
        return;
    }
    std::lock_guard<std::mutex> lock(impl_->stateMutex_);
    impl_->eventCallback_ = std::move(callback);
}

bool D3D11VideoRenderer::open(const VideoRenderConfig& config)
{
    if (!impl_) {
        return false;
    }

    std::string error;
    if (!impl_->device_ || !impl_->context_) {
        error = "The D3D11 renderer requires a borrowed device and context";
    } else if (!isSupportedConfig(config)) {
        error =
            "The D3D11 renderer requires a valid borrowed surface configuration";
    } else {
        ComPtr<ID3D11Device> contextDevice;
        impl_->context_.get()->GetDevice(&contextDevice);
        if (contextDevice.Get() != impl_->device_.get()) {
            error = "The borrowed D3D11 context belongs to another device";
        }
    }
    {
        std::lock_guard<std::mutex> lock(impl_->stateMutex_);
        if (error.empty() && !impl_->currentTarget_) {
            error = "The D3D11 renderer requires a current-target callback";
        } else if (error.empty() && impl_->hardwareInterop_
                   && impl_->hardwareInterop_->deviceAccess()
                       != impl_->deviceAccess_) {
            error =
                "The D3D11 hardware interop belongs to another device access";
        }
    }
    if (!error.empty()) {
        impl_->notify(VideoRenderEventType::Error, std::move(error));
        return false;
    }

    {
        std::lock_guard<std::mutex> renderLock(impl_->renderMutex_);
        auto contextGuard = impl_->deviceAccess_->contextGuard();
        (void)contextGuard;
        impl_->destroyCommon();
        impl_->createCommon(error);
    }
    if (!error.empty()) {
        impl_->notify(
            detail::d3d11FailureEvent(
                impl_->device_.get()->GetDeviceRemovedReason()),
            std::move(error));
        return false;
    }
    {
        std::lock_guard<std::mutex> lock(impl_->stateMutex_);
        impl_->config_ = config;
        impl_->open_ = true;
    }
    return true;
}

bool D3D11VideoRenderer::configure(const VideoRenderConfig& config)
{
    if (!impl_) {
        return false;
    }
    bool configured = false;
    {
        std::lock_guard<std::mutex> lock(impl_->stateMutex_);
        configured = impl_->open_ && isSupportedConfig(config);
        if (configured) {
            impl_->config_ = config;
        }
    }
    if (!configured) {
        impl_->notify(
            VideoRenderEventType::Error,
            "The D3D11 renderer is closed or the configuration is invalid");
    } else {
        impl_->notify(VideoRenderEventType::RedrawRequested, {});
    }
    return configured;
}

bool D3D11VideoRenderer::render(const VideoFrame& frame)
{
    if (!impl_ || !frame) {
        return false;
    }

    VideoRenderConfig config;
    D3D11CurrentTargetCallback currentTarget;
    std::shared_ptr<D3D11HardwareFrameInterop> hardwareInterop;
    bool allowSoftwareMappingFallback = false;
    {
        std::unique_lock<std::mutex> lock(
            impl_->stateMutex_,
            std::try_to_lock);
        if (!lock.owns_lock()) {
            impl_->stateBusyRenderAttempts_.fetch_add(
                1,
                std::memory_order_relaxed);
            return false;
        }
        if (impl_->open_) {
            config = impl_->config_;
            currentTarget = impl_->currentTarget_;
            hardwareInterop = impl_->hardwareInterop_;
            allowSoftwareMappingFallback =
                impl_->allowSoftwareMappingFallback_;
        }
    }
    if (!currentTarget) {
        impl_->notify(
            VideoRenderEventType::Error,
            "The D3D11 renderer is not open");
        return false;
    }

    const D3D11RenderTarget target = currentTarget();
    if (!target.isValid()) {
        impl_->notify(
            VideoRenderEventType::SurfaceLost,
            "The current D3D11 render target is unavailable");
        return false;
    }

    std::unique_lock<std::mutex> renderLock(
        impl_->renderMutex_,
        std::try_to_lock);
    if (!renderLock.owns_lock()) {
        impl_->serializationBusyRenderAttempts_.fetch_add(
            1,
            std::memory_order_relaxed);
        return false;
    }
    auto contextGuard = impl_->deviceAccess_->tryContextGuard();
    if (!contextGuard) {
        impl_->deviceContextBusyRenderAttempts_.fetch_add(
            1,
            std::memory_order_relaxed);
        auto& detailedCounter =
            contextGuard.contendedByReservationAwareOwner()
            ? impl_->reservationAwareContextBusyRenderAttempts_
            : impl_->unreservedContextBusyRenderAttempts_;
        detailedCounter.fetch_add(1, std::memory_order_relaxed);
        return false;
    }

    std::string error;
    std::string fallbackDetail;
    VideoRenderEventType errorType = VideoRenderEventType::Error;
    bool rendered = false;
    D3D11AdvancedColorInfo advancedColorInfo;
    bool advancedColorInfoResolved = false;

    const HRESULT removedReason =
        impl_->device_.get()->GetDeviceRemovedReason();
    if (detail::d3d11FailureEvent(removedReason)
        == VideoRenderEventType::SurfaceLost) {
        errorType = VideoRenderEventType::SurfaceLost;
        error = hresultText("The D3D11 device was removed", removedReason);
    }
    if (error.empty()) {
        const auto retireStarted = steadyMicroseconds();
        const bool retired = impl_->retireCompletedRenders(error);
        updateMaximum(
            impl_->maximumRetireCompletedMicroseconds_,
            steadyMicroseconds() - retireStarted);
        if (!retired) {
            errorType = detail::d3d11FailureEvent(
                impl_->device_.get()->GetDeviceRemovedReason());
        }
    }
    if (error.empty()
        && impl_->inFlightRenders_.size()
            >= impl_->maximumInFlightRenders) {
        impl_->inFlightBusyRenderAttempts_.fetch_add(
            1,
            std::memory_order_relaxed);
        return false;
    }

    const auto colorSetupStarted = steadyMicroseconds();
    ComPtr<ID3D11Texture2D> targetTextureNative;
    D3D11_TEXTURE2D_DESC targetDescriptor {};
    DXGI_FORMAT targetFormat = DXGI_FORMAT_UNKNOWN;
    if (error.empty()
        && (!targetDescription(
                impl_->device_.get(),
                target.view,
                targetTextureNative,
                targetDescriptor,
                targetFormat,
                error)
            || targetDescriptor.Width
                != static_cast<UINT>(config.surfaceSize.width)
            || targetDescriptor.Height
                != static_cast<UINT>(config.surfaceSize.height)
            || targetDescriptor.SampleDesc.Count != 1
            || !isSupportedTargetFormat(targetFormat))) {
        if (error.empty()) {
            error =
                "The current D3D11 target size, sample count, or pixel format is unsupported";
        }
    }
    if (error.empty()) {
        advancedColorInfoResolved = configureAdvancedColor(
            impl_->device_.get(),
            target,
            targetFormat,
            advancedColorInfo,
            error);
    }
    updateMaximum(
        impl_->maximumColorSetupMicroseconds_,
        steadyMicroseconds() - colorSetupStarted);

    pl_tex targetTexture = nullptr;
    pl_frame targetFrame {};
    if (error.empty()
        && !impl_->wrapTarget(
            targetTextureNative.Get(),
            targetFormat,
            config.surfaceSize.width,
            config.surfaceSize.height,
            advancedColorInfo,
            targetTexture,
            targetFrame,
            error)) {
        // Error populated by wrapTarget().
    }

    pl_frame image {};
    std::array<pl_tex, 2> hardwareTextures {};
    std::shared_ptr<D3D11TextureFrame> imported;
    std::shared_ptr<HardwareFrameMapping> mappedFrame;
    std::unique_ptr<AVFrame, AVFrameDeleter> mappedNative;
    pl_dovi_metadata dovi {};
    bool mappedSoftware = false;
    ComPtr<ID3D11Query> completion;

    const auto sourceStarted = steadyMicroseconds();
    if (error.empty() && frame.hasHardwareFrame()) {
        const HardwareFrame hardwareFrame = frame.hardwareFrame();
        const bool compatibleInterop = hardwareInterop
            && hardwareInterop->deviceAccess() == impl_->deviceAccess_
            && hardwareInterop->supports(hardwareFrame);
        if (compatibleInterop) {
            const auto interopStarted = steadyMicroseconds();
            imported = hardwareInterop->importFrame(
                hardwareFrame,
                frame.colorSpaceInfo());
            updateMaximum(
                impl_->maximumInteropMicroseconds_,
                steadyMicroseconds() - interopStarted);
            if (!imported
                || !impl_->wrapHardwareSource(
                    frame,
                    imported,
                    hardwareTextures,
                    image,
                    dovi,
                    error)) {
                if (error.empty()) {
                    error = "D3D11 hardware-frame import failed";
                }
            }
        } else {
            error =
                "The D3D11 renderer has no compatible interop for this hardware frame";
        }

        if (!error.empty() && allowSoftwareMappingFallback) {
            fallbackDetail = error
                + "; using explicit software-mapping fallback";
            error.clear();
            if (!hardwareFrame.isMappable(HardwareMapMode::Read)
                || !(mappedFrame =
                    hardwareFrame.map(HardwareMapMode::Read))
                || !(mappedNative = mappedAvFrame(*mappedFrame))
                || !qtav_pl_map_avframe(
                    impl_->d3d11_->gpu,
                    &image,
                    impl_->uploadTextures_.data(),
                    mappedNative.get())) {
                error = impl_->takeLogError(
                    "The D3D11 hardware frame could not be mapped for software fallback");
            } else {
                mappedSoftware = true;
            }
        }
    } else if (error.empty()) {
        const AVFrame* native = detail::FrameFactory::nativeVideoFrame(frame);
        if (!native
            || !qtav_pl_map_avframe(
                impl_->d3d11_->gpu,
                &image,
                impl_->uploadTextures_.data(),
                native)) {
            error = impl_->takeLogError(
                "libplacebo could not map the decoded software frame for D3D11");
        } else {
            mappedSoftware = true;
        }
    }
    updateMaximum(
        impl_->maximumBufferUpdateMicroseconds_,
        steadyMicroseconds() - sourceStarted);

    if (error.empty()) {
        const auto queryStarted = steadyMicroseconds();
        const bool acquired =
            impl_->acquireCompletionQuery(completion, error);
        updateMaximum(
            impl_->maximumCompletionQueryAcquireMicroseconds_,
            steadyMicroseconds() - queryStarted);
        if (!acquired) {
            errorType = detail::d3d11FailureEvent(
                impl_->device_.get()->GetDeviceRemovedReason());
        }
    }

    if (error.empty()) {
        applyGeometry(image, targetFrame, config);
        const auto drawStarted = steadyMicroseconds();
        constexpr float clearColor[] { 0.0F, 0.0F, 0.0F, 1.0F };
        const auto clearStarted = steadyMicroseconds();
        impl_->context_.get()->ClearRenderTargetView(
            target.view,
            clearColor);
        updateMaximum(
            impl_->maximumClearMicroseconds_,
            steadyMicroseconds() - clearStarted);
        const bool useHardwareImportFastParams =
            detail::d3d11ShouldUseHardwareImportFastParams(
                static_cast<bool>(imported));
        pl_render_params renderParams = useHardwareImportFastParams
            ? pl_render_fast_params
            : pl_render_default_params;
        const pl_hook* outputHooks[] { &impl_->scRgbOutputHook_ };
        if (targetFormat == DXGI_FORMAT_R16G16B16A16_FLOAT) {
            renderParams.hooks = outputHooks;
            renderParams.num_hooks = 1;
        }
        Impl::RenderPassProbe passProbe;
        renderParams.info_callback = &Impl::renderInfoCallback;
        renderParams.info_priv = &passProbe;
        const auto plRenderStarted = steadyMicroseconds();
        passProbe.cpuStartedMicroseconds = plRenderStarted;
        rendered = pl_render_image(
            impl_->renderer_,
            &image,
            &targetFrame,
            &renderParams);
        const auto plRenderFinished = steadyMicroseconds();
        updateMaximum(
            impl_->maximumPlRenderImageMicroseconds_,
            plRenderFinished - plRenderStarted);
        if (rendered) {
            updateMaximum(
                impl_->maximumLibplaceboPassesPerRender_,
                static_cast<std::int64_t>(passProbe.passCount));
            updateMaximum(
                impl_->maximumLibplaceboGpuFrameMicroseconds_,
                static_cast<std::int64_t>(
                    passProbe.gpuFrameNanoseconds / 1'000));
            updateMaximum(
                impl_->maximumLibplaceboGpuPassMicroseconds_,
                static_cast<std::int64_t>(
                    passProbe.maximumGpuPassNanoseconds / 1'000));
            if (passProbe.firstCallbackMicroseconds != 0) {
                updateMaximum(
                    impl_->maximumLibplaceboCallbackArrivalMicroseconds_,
                    passProbe.firstCallbackMicroseconds
                        - passProbe.cpuStartedMicroseconds);
                updateMaximum(
                    impl_->maximumLibplaceboPostCallbackMicroseconds_,
                    plRenderFinished
                        - passProbe.lastCallbackMicroseconds);
            }
            if (!impl_->havePreviousLibplaceboPassGraph_
                || impl_->previousLibplaceboPassGraphHash_
                    != passProbe.graphHash
                || impl_->previousLibplaceboPassCount_
                    != passProbe.passCount) {
                impl_->libplaceboPassGraphChanges_.fetch_add(
                    1,
                    std::memory_order_relaxed);
                impl_->previousLibplaceboPassGraphHash_ =
                    passProbe.graphHash;
                impl_->previousLibplaceboPassCount_ = passProbe.passCount;
                impl_->havePreviousLibplaceboPassGraph_ = true;
            }
            const auto completionEndStarted = steadyMicroseconds();
            impl_->context_.get()->End(completion.Get());
            updateMaximum(
                impl_->maximumCompletionQueryEndMicroseconds_,
                steadyMicroseconds() - completionEndStarted);
            const auto retentionStarted = steadyMicroseconds();
            Impl::InFlightRender inFlight;
            inFlight.completion = std::move(completion);
            inFlight.target = std::move(targetTextureNative);
            inFlight.source = frame;
            inFlight.imported = std::move(imported);
            inFlight.sourceTextures = hardwareTextures;
            hardwareTextures = {};
            inFlight.targetTexture = targetTexture;
            targetTexture = nullptr;
            impl_->inFlightRenders_.push_back(std::move(inFlight));
            updateMaximum(
                impl_->maximumInFlightRetentionMicroseconds_,
                steadyMicroseconds() - retentionStarted);
        }
        updateMaximum(
            impl_->maximumDrawMicroseconds_,
            steadyMicroseconds() - drawStarted);
        if (!rendered) {
            error = impl_->takeLogError(
                "libplacebo could not render the D3D11 video frame");
        }
    }

    if (mappedSoftware) {
        qtav_pl_unmap_avframe(impl_->d3d11_->gpu, &image);
    }
    if (!rendered && impl_->d3d11_) {
        // pl_render_image may have submitted partial work before reporting an
        // error. Complete it before releasing transient wrappers on this rare
        // failure path.
        pl_gpu_finish(impl_->d3d11_->gpu);
    }
    for (pl_tex& texture : hardwareTextures) {
        if (texture) {
            pl_tex_destroy(impl_->d3d11_->gpu, &texture);
        }
    }
    if (targetTexture) {
        pl_tex_destroy(impl_->d3d11_->gpu, &targetTexture);
    }

    const HRESULT drawReason =
        impl_->device_.get()->GetDeviceRemovedReason();
    if (detail::d3d11FailureEvent(drawReason)
        == VideoRenderEventType::SurfaceLost) {
        rendered = false;
        errorType = VideoRenderEventType::SurfaceLost;
        error = hresultText(
            "The D3D11 device was removed during rendering",
            drawReason);
    } else if (impl_->d3d11_
               && pl_gpu_is_failed(impl_->d3d11_->gpu)) {
        rendered = false;
        errorType = VideoRenderEventType::SurfaceLost;
        error = "libplacebo reported a failed D3D11 device";
    }

    if (advancedColorInfoResolved) {
        std::unique_lock<std::mutex> lock(
            impl_->stateMutex_,
            std::try_to_lock);
        if (lock.owns_lock()
            && !sameAdvancedColorInfo(
                impl_->advancedColorInfo_,
                advancedColorInfo)) {
            impl_->advancedColorInfo_ = advancedColorInfo;
        }
    }
    if (!rendered) {
        impl_->notify(
            errorType,
            error.empty() ? "libplacebo D3D11 rendering failed"
                          : std::move(error));
    } else if (!fallbackDetail.empty()) {
        impl_->notify(
            VideoRenderEventType::Error,
            std::move(fallbackDetail));
    }
    return rendered;
}

void D3D11VideoRenderer::close() noexcept
{
    if (impl_) {
        impl_->close();
    }
}

void D3D11VideoRenderer::flush() noexcept
{
    if (!impl_) {
        return;
    }
    std::lock_guard<std::mutex> renderLock(impl_->renderMutex_);
    if (!impl_->deviceAccess_ || !impl_->d3d11_) {
        return;
    }
    auto contextGuard = impl_->deviceAccess_->contextGuard();
    (void)contextGuard;
    pl_gpu_finish(impl_->d3d11_->gpu);
    impl_->destroyInFlightRenders();
}

BorrowedD3D11Device D3D11VideoRenderer::device() const noexcept
{
    return impl_ ? impl_->device_ : BorrowedD3D11Device {};
}

BorrowedD3D11DeviceContext D3D11VideoRenderer::context() const noexcept
{
    return impl_ ? impl_->context_ : BorrowedD3D11DeviceContext {};
}

std::shared_ptr<D3D11DeviceAccess>
D3D11VideoRenderer::deviceAccess() const noexcept
{
    return impl_ ? impl_->deviceAccess_
                 : std::shared_ptr<D3D11DeviceAccess> {};
}

void D3D11VideoRenderer::setCurrentTargetCallback(
    D3D11CurrentTargetCallback callback)
{
    if (!impl_) {
        return;
    }
    bool requestRedraw = false;
    {
        std::lock_guard<std::mutex> lock(impl_->stateMutex_);
        impl_->currentTarget_ = std::move(callback);
        requestRedraw = impl_->open_;
    }
    if (requestRedraw) {
        impl_->notify(VideoRenderEventType::RedrawRequested, {});
    }
}

void D3D11VideoRenderer::setHardwareFrameInterop(
    std::shared_ptr<D3D11HardwareFrameInterop> hardwareInterop)
{
    if (!impl_) {
        return;
    }
    bool requestRedraw = false;
    {
        std::lock_guard<std::mutex> lock(impl_->stateMutex_);
        impl_->hardwareInterop_ = std::move(hardwareInterop);
        requestRedraw = impl_->open_;
    }
    if (requestRedraw) {
        impl_->notify(VideoRenderEventType::RedrawRequested, {});
    }
}

std::shared_ptr<D3D11HardwareFrameInterop>
D3D11VideoRenderer::hardwareFrameInterop() const noexcept
{
    if (!impl_) {
        return {};
    }
    std::lock_guard<std::mutex> lock(impl_->stateMutex_);
    return impl_->hardwareInterop_;
}

void D3D11VideoRenderer::setAllowSoftwareMappingFallback(
    bool allow) noexcept
{
    if (!impl_) {
        return;
    }
    std::lock_guard<std::mutex> lock(impl_->stateMutex_);
    impl_->allowSoftwareMappingFallback_ = allow;
}

bool D3D11VideoRenderer::allowSoftwareMappingFallback() const noexcept
{
    if (!impl_) {
        return false;
    }
    std::lock_guard<std::mutex> lock(impl_->stateMutex_);
    return impl_->allowSoftwareMappingFallback_;
}

D3D11AdvancedColorInfo
D3D11VideoRenderer::advancedColorInfo() const noexcept
{
    if (!impl_) {
        return {};
    }
    std::lock_guard<std::mutex> lock(impl_->stateMutex_);
    return impl_->advancedColorInfo_;
}

D3D11VideoRendererStatistics
D3D11VideoRenderer::takeStatistics() noexcept
{
    if (!impl_) {
        return {};
    }
    D3D11VideoRendererStatistics result;
    result.decoderSurfaceCopies =
        impl_->decoderSurfaceCopies_.exchange(0);
    result.stateBusyRenderAttempts =
        impl_->stateBusyRenderAttempts_.exchange(0);
    result.serializationBusyRenderAttempts =
        impl_->serializationBusyRenderAttempts_.exchange(0);
    result.deviceContextBusyRenderAttempts =
        impl_->deviceContextBusyRenderAttempts_.exchange(0);
    result.reservationAwareContextBusyRenderAttempts =
        impl_->reservationAwareContextBusyRenderAttempts_.exchange(0);
    result.unreservedContextBusyRenderAttempts =
        impl_->unreservedContextBusyRenderAttempts_.exchange(0);
    result.inFlightBusyRenderAttempts =
        impl_->inFlightBusyRenderAttempts_.exchange(0);
    result.maximumColorSetupMicroseconds =
        impl_->maximumColorSetupMicroseconds_.exchange(0);
    result.maximumInteropMicroseconds =
        impl_->maximumInteropMicroseconds_.exchange(0);
    result.maximumBufferUpdateMicroseconds =
        impl_->maximumBufferUpdateMicroseconds_.exchange(0);
    result.maximumDrawMicroseconds =
        impl_->maximumDrawMicroseconds_.exchange(0);
    result.maximumRetireCompletedMicroseconds =
        impl_->maximumRetireCompletedMicroseconds_.exchange(0);
    result.maximumCompletionQueryAcquireMicroseconds =
        impl_->maximumCompletionQueryAcquireMicroseconds_.exchange(0);
    result.maximumClearMicroseconds =
        impl_->maximumClearMicroseconds_.exchange(0);
    result.maximumPlRenderImageMicroseconds =
        impl_->maximumPlRenderImageMicroseconds_.exchange(0);
    result.maximumCompletionQueryEndMicroseconds =
        impl_->maximumCompletionQueryEndMicroseconds_.exchange(0);
    result.maximumInFlightRetentionMicroseconds =
        impl_->maximumInFlightRetentionMicroseconds_.exchange(0);
    result.maximumLibplaceboPassesPerRender =
        impl_->maximumLibplaceboPassesPerRender_.exchange(0);
    result.libplaceboPassGraphChanges =
        impl_->libplaceboPassGraphChanges_.exchange(0);
    result.maximumLibplaceboGpuFrameMicroseconds =
        impl_->maximumLibplaceboGpuFrameMicroseconds_.exchange(0);
    result.maximumLibplaceboGpuPassMicroseconds =
        impl_->maximumLibplaceboGpuPassMicroseconds_.exchange(0);
    result.maximumLibplaceboCallbackArrivalMicroseconds =
        impl_->maximumLibplaceboCallbackArrivalMicroseconds_.exchange(0);
    result.maximumLibplaceboPostCallbackMicroseconds =
        impl_->maximumLibplaceboPostCallbackMicroseconds_.exchange(0);
    return result;
}

} // namespace qtav
