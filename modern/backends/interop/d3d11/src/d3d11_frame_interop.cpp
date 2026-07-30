// SPDX-License-Identifier: LGPL-2.1-or-later

#if !defined(NOMINMAX)
#  define NOMINMAX
#endif

#include <d3d11_1.h>
#include <wrl/client.h>

#include <qtav/d3d11_frame_interop.h>
#include <qtav/d3d11va_hardware_decoder.h>

#include <utility>

namespace qtav {
namespace {

using Microsoft::WRL::ComPtr;

constexpr DXGI_FORMAT outputFormat =
    DXGI_FORMAT_B8G8R8A8_UNORM;
constexpr DXGI_COLOR_SPACE_TYPE outputColorSpace =
    DXGI_COLOR_SPACE_RGB_FULL_G22_NONE_P709;

DXGI_FORMAT sourceFormat(PixelFormat format) noexcept
{
    switch (format) {
    case PixelFormat::NV12:
        return DXGI_FORMAT_NV12;
    case PixelFormat::P010:
        return DXGI_FORMAT_P010;
    default:
        return DXGI_FORMAT_UNKNOWN;
    }
}

enum class MatrixFamily {
    BT601,
    BT709,
    BT2020,
};

MatrixFamily matrixFamily(
    const VideoColorSpace& color,
    int height) noexcept
{
    switch (color.matrix) {
    case ColorMatrix::BT2020NCL:
    case ColorMatrix::BT2020CL:
    case ColorMatrix::ChromaDerivedNCL:
    case ColorMatrix::ChromaDerivedCL:
        return MatrixFamily::BT2020;
    case ColorMatrix::BT470BG:
    case ColorMatrix::SMPTE170M:
    case ColorMatrix::FCC:
        return MatrixFamily::BT601;
    case ColorMatrix::BT709:
        return MatrixFamily::BT709;
    default:
        return height > 576 ? MatrixFamily::BT709
                            : MatrixFamily::BT601;
    }
}

bool isTopLeft(const VideoColorSpace& color) noexcept
{
    return color.chromaLocation == ChromaLocation::TopLeft
        || color.chromaLocation == ChromaLocation::Top;
}

bool extendedColorRequired(
    const VideoColorSpace& color,
    int height) noexcept
{
    return matrixFamily(color, height) == MatrixFamily::BT2020
        || color.primaries == ColorPrimaries::BT2020
        || color.transfer == ColorTransfer::PQ
        || color.transfer == ColorTransfer::HLG;
}

bool dxgiInputColorSpace(
    const VideoColorSpace& color,
    int height,
    DXGI_COLOR_SPACE_TYPE& result) noexcept
{
    const bool full = color.range == ColorRange::Full;
    switch (matrixFamily(color, height)) {
    case MatrixFamily::BT601:
        if (color.transfer == ColorTransfer::PQ
            || color.transfer == ColorTransfer::HLG) {
            return false;
        }
        result = full
            ? DXGI_COLOR_SPACE_YCBCR_FULL_G22_LEFT_P601
            : DXGI_COLOR_SPACE_YCBCR_STUDIO_G22_LEFT_P601;
        return true;
    case MatrixFamily::BT709:
        if (color.transfer == ColorTransfer::PQ
            || color.transfer == ColorTransfer::HLG) {
            return false;
        }
        result = full
            ? DXGI_COLOR_SPACE_YCBCR_FULL_G22_LEFT_P709
            : DXGI_COLOR_SPACE_YCBCR_STUDIO_G22_LEFT_P709;
        return true;
    case MatrixFamily::BT2020:
        if (color.transfer == ColorTransfer::PQ) {
            if (full) {
                return false;
            }
            result = isTopLeft(color)
                ? DXGI_COLOR_SPACE_YCBCR_STUDIO_G2084_TOPLEFT_P2020
                : DXGI_COLOR_SPACE_YCBCR_STUDIO_G2084_LEFT_P2020;
            return true;
        }
        if (color.transfer == ColorTransfer::HLG) {
            result = full
                ? DXGI_COLOR_SPACE_YCBCR_FULL_GHLG_TOPLEFT_P2020
                : DXGI_COLOR_SPACE_YCBCR_STUDIO_GHLG_TOPLEFT_P2020;
            return true;
        }
        if (!full && isTopLeft(color)) {
            result =
                DXGI_COLOR_SPACE_YCBCR_STUDIO_G22_TOPLEFT_P2020;
        } else {
            result = full
                ? DXGI_COLOR_SPACE_YCBCR_FULL_G22_LEFT_P2020
                : DXGI_COLOR_SPACE_YCBCR_STUDIO_G22_LEFT_P2020;
        }
        return true;
    }
    return false;
}

D3D11_VIDEO_PROCESSOR_COLOR_SPACE legacyInputColorSpace(
    const VideoColorSpace& color,
    int height) noexcept
{
    D3D11_VIDEO_PROCESSOR_COLOR_SPACE result {};
    result.YCbCr_Matrix =
        matrixFamily(color, height) == MatrixFamily::BT709 ? 1U : 0U;
    result.Nominal_Range =
        color.range == ColorRange::Full
        ? D3D11_VIDEO_PROCESSOR_NOMINAL_RANGE_0_255
        : D3D11_VIDEO_PROCESSOR_NOMINAL_RANGE_16_235;
    return result;
}

D3D11_VIDEO_PROCESSOR_COLOR_SPACE
legacyOutputColorSpace() noexcept
{
    D3D11_VIDEO_PROCESSOR_COLOR_SPACE result {};
    result.RGB_Range = 0;
    result.Nominal_Range =
        D3D11_VIDEO_PROCESSOR_NOMINAL_RANGE_0_255;
    return result;
}

bool supportedSource(
    const D3D11VAFrame& frame,
    ID3D11Device* targetDevice) noexcept
{
    if (!frame || !targetDevice
        || frame.device() != targetDevice
        || sourceFormat(frame.softwareFormat())
            == DXGI_FORMAT_UNKNOWN) {
        return false;
    }

    D3D11_TEXTURE2D_DESC description {};
    frame.texture()->GetDesc(&description);
    return description.Usage == D3D11_USAGE_DEFAULT
        && description.SampleDesc.Count == 1
        && description.MipLevels == 1
        && frame.arraySlice() < description.ArraySize
        && static_cast<UINT>(frame.width()) <= description.Width
        && static_cast<UINT>(frame.height()) <= description.Height;
}

class ImportedD3D11TextureFrame final
    : public D3D11TextureFrame {
public:
    ImportedD3D11TextureFrame(
        HardwareFrame source,
        ComPtr<ID3D11VideoProcessorInputView> inputView,
        ComPtr<ID3D11VideoProcessorOutputView> outputView,
        ComPtr<ID3D11Texture2D> texture,
        ComPtr<ID3D11ShaderResourceView> shaderView)
        : source_(std::move(source))
        , inputView_(std::move(inputView))
        , outputView_(std::move(outputView))
        , texture_(std::move(texture))
        , shaderView_(std::move(shaderView))
    {
    }

    int width() const noexcept override
    {
        return source_.width();
    }

    int height() const noexcept override
    {
        return source_.height();
    }

    PixelFormat format() const noexcept override
    {
        return PixelFormat::BGRA;
    }

    ID3D11Texture2D* texture() const noexcept override
    {
        return texture_.Get();
    }

    ID3D11ShaderResourceView*
    shaderResourceView() const noexcept override
    {
        return shaderView_.Get();
    }

private:
    HardwareFrame source_;
    ComPtr<ID3D11VideoProcessorInputView> inputView_;
    ComPtr<ID3D11VideoProcessorOutputView> outputView_;
    ComPtr<ID3D11Texture2D> texture_;
    ComPtr<ID3D11ShaderResourceView> shaderView_;
};

struct ProcessorState {
    int width = 0;
    int height = 0;
    PixelFormat format = PixelFormat::Unknown;
    ComPtr<ID3D11VideoProcessorEnumerator> enumerator;
    ComPtr<ID3D11VideoProcessor> processor;

    bool matches(
        const D3D11VAFrame& frame) const noexcept
    {
        return enumerator && processor
            && width == frame.width()
            && height == frame.height()
            && format == frame.softwareFormat();
    }

    void reset() noexcept
    {
        width = 0;
        height = 0;
        format = PixelFormat::Unknown;
        processor.Reset();
        enumerator.Reset();
    }
};

} // namespace

class D3D11FrameInterop::Impl {
public:
    explicit Impl(
        std::shared_ptr<D3D11DeviceAccess> selectedAccess)
        : deviceAccess_(std::move(selectedAccess))
    {
        if (!deviceAccess_ || !deviceAccess_->device()
            || !deviceAccess_->immediateContext()) {
            return;
        }
        deviceAccess_->device().get()->QueryInterface(
            IID_PPV_ARGS(&videoDevice_));
        deviceAccess_->immediateContext().get()->QueryInterface(
            IID_PPV_ARGS(&videoContext_));
        if (videoContext_) {
            videoContext_.As(&videoContext1_);
        }
    }

    bool available() const noexcept
    {
        return deviceAccess_ && videoDevice_ && videoContext_
            && deviceAccess_->device().get()->GetDeviceRemovedReason()
                == S_OK;
    }

    bool ensureProcessor(const D3D11VAFrame& frame)
    {
        if (processor_.matches(frame)) {
            return true;
        }
        processor_.reset();

        D3D11_VIDEO_PROCESSOR_CONTENT_DESC content {};
        content.InputFrameFormat =
            D3D11_VIDEO_FRAME_FORMAT_PROGRESSIVE;
        content.InputFrameRate = { 1, 1 };
        content.InputWidth = static_cast<UINT>(frame.width());
        content.InputHeight = static_cast<UINT>(frame.height());
        content.OutputFrameRate = { 1, 1 };
        content.OutputWidth = static_cast<UINT>(frame.width());
        content.OutputHeight = static_cast<UINT>(frame.height());
        content.Usage = D3D11_VIDEO_USAGE_PLAYBACK_NORMAL;

        ComPtr<ID3D11VideoProcessorEnumerator> enumerator;
        if (FAILED(videoDevice_->CreateVideoProcessorEnumerator(
                &content,
                &enumerator))) {
            return false;
        }

        UINT inputSupport = 0;
        UINT outputSupport = 0;
        if (FAILED(enumerator->CheckVideoProcessorFormat(
                sourceFormat(frame.softwareFormat()),
                &inputSupport))
            || !(inputSupport
                & D3D11_VIDEO_PROCESSOR_FORMAT_SUPPORT_INPUT)
            || FAILED(enumerator->CheckVideoProcessorFormat(
                outputFormat,
                &outputSupport))
            || !(outputSupport
                & D3D11_VIDEO_PROCESSOR_FORMAT_SUPPORT_OUTPUT)) {
            return false;
        }

        D3D11_VIDEO_PROCESSOR_CAPS capabilities {};
        if (FAILED(enumerator->GetVideoProcessorCaps(&capabilities))
            || capabilities.RateConversionCapsCount == 0) {
            return false;
        }

        ComPtr<ID3D11VideoProcessor> processor;
        if (FAILED(videoDevice_->CreateVideoProcessor(
                enumerator.Get(),
                0,
                &processor))) {
            return false;
        }

        processor_.width = frame.width();
        processor_.height = frame.height();
        processor_.format = frame.softwareFormat();
        processor_.enumerator = std::move(enumerator);
        processor_.processor = std::move(processor);
        return true;
    }

    bool configureColor(
        const D3D11VAFrame& frame,
        const VideoColorSpace& color)
    {
        DXGI_COLOR_SPACE_TYPE input {};
        if (!dxgiInputColorSpace(color, frame.height(), input)) {
            return false;
        }

        ComPtr<ID3D11VideoProcessorEnumerator1> enumerator1;
        if (videoContext1_
            && SUCCEEDED(processor_.enumerator.As(&enumerator1))) {
            BOOL supported = FALSE;
            if (FAILED(
                    enumerator1->CheckVideoProcessorFormatConversion(
                        sourceFormat(frame.softwareFormat()),
                        input,
                        outputFormat,
                        outputColorSpace,
                        &supported))
                || !supported) {
                return false;
            }
            videoContext1_->VideoProcessorSetStreamColorSpace1(
                processor_.processor.Get(),
                0,
                input);
            videoContext1_->VideoProcessorSetOutputColorSpace1(
                processor_.processor.Get(),
                outputColorSpace);
            return true;
        }

        if (extendedColorRequired(color, frame.height())) {
            return false;
        }
        const auto inputLegacy =
            legacyInputColorSpace(color, frame.height());
        const auto outputLegacy = legacyOutputColorSpace();
        videoContext_->VideoProcessorSetStreamColorSpace(
            processor_.processor.Get(),
            0,
            &inputLegacy);
        videoContext_->VideoProcessorSetOutputColorSpace(
            processor_.processor.Get(),
            &outputLegacy);
        return true;
    }

    std::shared_ptr<D3D11DeviceAccess> deviceAccess_;
    ComPtr<ID3D11VideoDevice> videoDevice_;
    ComPtr<ID3D11VideoContext> videoContext_;
    ComPtr<ID3D11VideoContext1> videoContext1_;
    ProcessorState processor_;
};

D3D11FrameInterop::D3D11FrameInterop(
    std::shared_ptr<D3D11DeviceAccess> deviceAccess)
    : impl_(std::make_unique<Impl>(std::move(deviceAccess)))
{
}

D3D11FrameInterop::~D3D11FrameInterop() = default;
D3D11FrameInterop::D3D11FrameInterop(
    D3D11FrameInterop&&) noexcept = default;
D3D11FrameInterop& D3D11FrameInterop::operator=(
    D3D11FrameInterop&&) noexcept = default;

std::shared_ptr<D3D11DeviceAccess>
D3D11FrameInterop::deviceAccess() const noexcept
{
    return impl_ ? impl_->deviceAccess_
                 : std::shared_ptr<D3D11DeviceAccess> {};
}

HardwareInteropCapabilities
D3D11FrameInterop::capabilities() const
{
    HardwareInteropCapabilities result;
    if (impl_ && impl_->available()) {
        result.sourceDevices = { HardwareDeviceType::D3D11 };
        result.targetDevice = HardwareDeviceType::D3D11;
        result.zeroCopy = true;
    }
    return result;
}

bool D3D11FrameInterop::supports(
    const HardwareFrame& frame) const noexcept
{
    if (!impl_ || !impl_->available()) {
        return false;
    }
    return supportedSource(
        d3d11vaFrame(frame),
        impl_->deviceAccess_->device().get());
}

std::shared_ptr<D3D11TextureFrame>
D3D11FrameInterop::importFrame(
    const HardwareFrame& frame)
{
    return importFrame(frame, {});
}

std::shared_ptr<D3D11TextureFrame>
D3D11FrameInterop::importFrame(
    const HardwareFrame& frame,
    const VideoColorSpace& color)
{
    if (!impl_ || !impl_->available()) {
        return {};
    }

    const D3D11VAFrame native = d3d11vaFrame(frame);
    if (!supportedSource(
            native,
            impl_->deviceAccess_->device().get())) {
        return {};
    }

    auto contextGuard = impl_->deviceAccess_->contextGuard();
    (void)contextGuard;
    if (!impl_->available()
        || !impl_->ensureProcessor(native)
        || !impl_->configureColor(native, color)) {
        return {};
    }

    D3D11_VIDEO_PROCESSOR_INPUT_VIEW_DESC inputDescription {};
    inputDescription.ViewDimension =
        D3D11_VPIV_DIMENSION_TEXTURE2D;
    inputDescription.Texture2D.MipSlice = 0;
    inputDescription.Texture2D.ArraySlice =
        native.arraySlice();
    ComPtr<ID3D11VideoProcessorInputView> inputView;
    if (FAILED(impl_->videoDevice_->CreateVideoProcessorInputView(
            native.texture(),
            impl_->processor_.enumerator.Get(),
            &inputDescription,
            &inputView))) {
        return {};
    }

    D3D11_TEXTURE2D_DESC outputDescription {};
    outputDescription.Width =
        static_cast<UINT>(native.width());
    outputDescription.Height =
        static_cast<UINT>(native.height());
    outputDescription.MipLevels = 1;
    outputDescription.ArraySize = 1;
    outputDescription.Format = outputFormat;
    outputDescription.SampleDesc.Count = 1;
    outputDescription.Usage = D3D11_USAGE_DEFAULT;
    outputDescription.BindFlags =
        D3D11_BIND_RENDER_TARGET
        | D3D11_BIND_SHADER_RESOURCE;

    ComPtr<ID3D11Texture2D> outputTexture;
    if (FAILED(
            impl_->deviceAccess_->device().get()->CreateTexture2D(
                &outputDescription,
                nullptr,
                &outputTexture))) {
        return {};
    }

    D3D11_VIDEO_PROCESSOR_OUTPUT_VIEW_DESC
        outputViewDescription {};
    outputViewDescription.ViewDimension =
        D3D11_VPOV_DIMENSION_TEXTURE2D;
    outputViewDescription.Texture2D.MipSlice = 0;
    ComPtr<ID3D11VideoProcessorOutputView> outputView;
    if (FAILED(impl_->videoDevice_->CreateVideoProcessorOutputView(
            outputTexture.Get(),
            impl_->processor_.enumerator.Get(),
            &outputViewDescription,
            &outputView))) {
        return {};
    }

    ComPtr<ID3D11ShaderResourceView> shaderView;
    if (FAILED(
            impl_->deviceAccess_->device()
                .get()
                ->CreateShaderResourceView(
                    outputTexture.Get(),
                    nullptr,
                    &shaderView))) {
        return {};
    }

    const RECT sourceRectangle {
        0,
        0,
        native.width(),
        native.height(),
    };
    impl_->videoContext_->VideoProcessorSetOutputTargetRect(
        impl_->processor_.processor.Get(),
        TRUE,
        &sourceRectangle);
    impl_->videoContext_->VideoProcessorSetStreamFrameFormat(
        impl_->processor_.processor.Get(),
        0,
        D3D11_VIDEO_FRAME_FORMAT_PROGRESSIVE);
    impl_->videoContext_->VideoProcessorSetStreamSourceRect(
        impl_->processor_.processor.Get(),
        0,
        TRUE,
        &sourceRectangle);
    impl_->videoContext_->VideoProcessorSetStreamDestRect(
        impl_->processor_.processor.Get(),
        0,
        TRUE,
        &sourceRectangle);
    impl_->videoContext_->VideoProcessorSetStreamAutoProcessingMode(
        impl_->processor_.processor.Get(),
        0,
        FALSE);

    D3D11_VIDEO_PROCESSOR_STREAM stream {};
    stream.Enable = TRUE;
    stream.pInputSurface = inputView.Get();
    if (FAILED(impl_->videoContext_->VideoProcessorBlt(
            impl_->processor_.processor.Get(),
            outputView.Get(),
            0,
            1,
            &stream))) {
        return {};
    }

    return std::make_shared<ImportedD3D11TextureFrame>(
        frame,
        std::move(inputView),
        std::move(outputView),
        std::move(outputTexture),
        std::move(shaderView));
}

void D3D11FrameInterop::flush() noexcept
{
    if (!impl_ || !impl_->deviceAccess_) {
        return;
    }
    auto contextGuard = impl_->deviceAccess_->contextGuard();
    (void)contextGuard;
    impl_->processor_.reset();
}

} // namespace qtav
