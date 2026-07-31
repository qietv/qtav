// SPDX-License-Identifier: LGPL-2.1-or-later

#if !defined(NOMINMAX)
#  define NOMINMAX
#endif

#include <d3d11_1.h>
#include <wrl/client.h>

#include <qtav/d3d11_frame_interop.h>
#include <qtav/d3d11va_hardware_decoder.h>

#include <algorithm>
#include <array>
#include <iterator>
#include <memory>
#include <utility>
#include <vector>

namespace qtav {
namespace {

using Microsoft::WRL::ComPtr;

struct OutputProfile {
    DXGI_FORMAT format = DXGI_FORMAT_B8G8R8A8_UNORM;
    DXGI_COLOR_SPACE_TYPE colorSpace =
        DXGI_COLOR_SPACE_RGB_FULL_G22_NONE_P709;
    PixelFormat pixelFormat = PixelFormat::BGRA;
};

constexpr OutputProfile sdrOutput {};
constexpr OutputProfile scRgbOutput {
    DXGI_FORMAT_R16G16B16A16_FLOAT,
    DXGI_COLOR_SPACE_RGB_FULL_G10_NONE_P709,
    PixelFormat::RGBA,
};
constexpr OutputProfile hdr10Output {
    DXGI_FORMAT_R10G10B10A2_UNORM,
    DXGI_COLOR_SPACE_RGB_FULL_G2084_NONE_P2020,
    PixelFormat::RGBA,
};

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
    struct OutputResource {
        int width = 0;
        int height = 0;
        OutputProfile output;
        ComPtr<ID3D11VideoProcessorOutputView> outputView;
        ComPtr<ID3D11Texture2D> texture;
        ComPtr<ID3D11ShaderResourceView> shaderView;

        bool matches(
            int requestedWidth,
            int requestedHeight,
            const OutputProfile& requestedOutput) const noexcept
        {
            return width == requestedWidth
                && height == requestedHeight
                && output.format == requestedOutput.format
                && output.colorSpace == requestedOutput.colorSpace
                && output.pixelFormat == requestedOutput.pixelFormat;
        }
    };

    ImportedD3D11TextureFrame(
        HardwareFrame source,
        ComPtr<ID3D11VideoProcessorInputView> inputView,
        std::shared_ptr<OutputResource> output)
        : source_(std::move(source))
        , inputView_(std::move(inputView))
        , output_(std::move(output))
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
        return output_ ? output_->output.pixelFormat
                       : PixelFormat::Unknown;
    }

    ID3D11Texture2D* texture() const noexcept override
    {
        return output_ ? output_->texture.Get() : nullptr;
    }

    ID3D11ShaderResourceView*
    shaderResourceView() const noexcept override
    {
        return output_ ? output_->shaderView.Get() : nullptr;
    }

    DXGI_FORMAT dxgiFormat() const noexcept override
    {
        return output_ ? output_->output.format
                       : DXGI_FORMAT_UNKNOWN;
    }

    DXGI_COLOR_SPACE_TYPE colorSpace() const noexcept override
    {
        return output_
            ? output_->output.colorSpace
            : DXGI_COLOR_SPACE_RGB_FULL_G22_NONE_P709;
    }

private:
    HardwareFrame source_;
    ComPtr<ID3D11VideoProcessorInputView> inputView_;
    std::shared_ptr<OutputResource> output_;
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
    using OutputResource =
        ImportedD3D11TextureFrame::OutputResource;

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

    ~Impl()
    {
        flush();
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
        retireOutputPool();
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
        if (FAILED(enumerator->CheckVideoProcessorFormat(
                sourceFormat(frame.softwareFormat()),
                &inputSupport))
            || !(inputSupport
                & D3D11_VIDEO_PROCESSOR_FORMAT_SUPPORT_INPUT)) {
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
        const VideoColorSpace& color,
        OutputProfile& selectedOutput)
    {
        DXGI_COLOR_SPACE_TYPE input {};
        if (!dxgiInputColorSpace(color, frame.height(), input)) {
            return false;
        }

        ComPtr<ID3D11VideoProcessorEnumerator1> enumerator1;
        if (videoContext1_
            && SUCCEEDED(processor_.enumerator.As(&enumerator1))) {
            std::array<OutputProfile, 2> candidates {
                sdrOutput,
                sdrOutput,
            };
            std::size_t candidateCount = 1;
            if (color.transfer == ColorTransfer::PQ
                && color.primaries == ColorPrimaries::BT2020) {
                candidates[0] = hdr10Output;
                candidates[1] = scRgbOutput;
                candidateCount = 2;
            } else if (
                color.transfer == ColorTransfer::HLG
                && color.primaries == ColorPrimaries::BT2020) {
                candidates[0] = scRgbOutput;
                candidates[1] = hdr10Output;
                candidateCount = 2;
            } else if (extendedColorRequired(
                           color,
                           frame.height())) {
                candidates[0] = scRgbOutput;
            }

            for (std::size_t index = 0;
                 index < candidateCount;
                 ++index) {
                const OutputProfile candidate = candidates[index];
                UINT outputSupport = 0;
                BOOL conversionSupported = FALSE;
                if (FAILED(
                        processor_.enumerator
                            ->CheckVideoProcessorFormat(
                                candidate.format,
                                &outputSupport))
                    || !(outputSupport
                        & D3D11_VIDEO_PROCESSOR_FORMAT_SUPPORT_OUTPUT)
                    || FAILED(
                        enumerator1
                            ->CheckVideoProcessorFormatConversion(
                                sourceFormat(frame.softwareFormat()),
                                input,
                                candidate.format,
                                candidate.colorSpace,
                                &conversionSupported))
                    || !conversionSupported) {
                    continue;
                }
                videoContext1_->VideoProcessorSetStreamColorSpace1(
                    processor_.processor.Get(),
                    0,
                    input);
                videoContext1_->VideoProcessorSetOutputColorSpace1(
                    processor_.processor.Get(),
                    candidate.colorSpace);
                selectedOutput = candidate;
                return true;
            }
            return false;
        }

        if (extendedColorRequired(color, frame.height())) {
            return false;
        }
        UINT outputSupport = 0;
        if (FAILED(processor_.enumerator->CheckVideoProcessorFormat(
                sdrOutput.format,
                &outputSupport))
            || !(outputSupport
                & D3D11_VIDEO_PROCESSOR_FORMAT_SUPPORT_OUTPUT)) {
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
        selectedOutput = sdrOutput;
        return true;
    }

    void collectRetiredOutputs() noexcept
    {
        for (auto iterator = retiredOutputs_.begin();
             iterator != retiredOutputs_.end();) {
            if ((*iterator).use_count() == 1) {
                iterator = retiredOutputs_.erase(iterator);
            } else {
                ++iterator;
            }
        }
    }

    void retireOutputPool()
    {
        retiredOutputs_.insert(
            retiredOutputs_.end(),
            std::make_move_iterator(outputPool_.begin()),
            std::make_move_iterator(outputPool_.end()));
        outputPool_.clear();
        collectRetiredOutputs();
    }

    std::shared_ptr<OutputResource> createOutput(
        int width,
        int height,
        const OutputProfile& output)
    {
        D3D11_TEXTURE2D_DESC description {};
        description.Width = static_cast<UINT>(width);
        description.Height = static_cast<UINT>(height);
        description.MipLevels = 1;
        description.ArraySize = 1;
        description.Format = output.format;
        description.SampleDesc.Count = 1;
        description.Usage = D3D11_USAGE_DEFAULT;
        description.BindFlags =
            D3D11_BIND_RENDER_TARGET
            | D3D11_BIND_SHADER_RESOURCE;

        auto resource = std::make_shared<OutputResource>();
        resource->width = width;
        resource->height = height;
        resource->output = output;
        if (FAILED(
                deviceAccess_->device().get()->CreateTexture2D(
                    &description,
                    nullptr,
                    &resource->texture))) {
            return {};
        }

        D3D11_VIDEO_PROCESSOR_OUTPUT_VIEW_DESC viewDescription {};
        viewDescription.ViewDimension =
            D3D11_VPOV_DIMENSION_TEXTURE2D;
        viewDescription.Texture2D.MipSlice = 0;
        if (FAILED(videoDevice_->CreateVideoProcessorOutputView(
                resource->texture.Get(),
                processor_.enumerator.Get(),
                &viewDescription,
                &resource->outputView))) {
            return {};
        }

        if (FAILED(
                deviceAccess_->device()
                    .get()
                    ->CreateShaderResourceView(
                        resource->texture.Get(),
                        nullptr,
                        &resource->shaderView))) {
            return {};
        }

        return resource;
    }

    std::shared_ptr<OutputResource> acquireOutput(
        int width,
        int height,
        const OutputProfile& output)
    {
        collectRetiredOutputs();
        for (const auto& resource : outputPool_) {
            if (resource.use_count() == 1
                && resource->matches(width, height, output)) {
                return resource;
            }
        }

        constexpr std::size_t maximumPooledOutputs = 3;
        if (outputPool_.size() < maximumPooledOutputs) {
            auto resource = createOutput(width, height, output);
            if (!resource) {
                return {};
            }
            outputPool_.push_back(resource);
            return resource;
        }

        auto reusable = std::find_if(
            outputPool_.begin(),
            outputPool_.end(),
            [](const auto& candidate) {
                return candidate.use_count() == 1;
            });

        if (reusable != outputPool_.end()) {
            if ((*reusable)->matches(width, height, output)) {
                return *reusable;
            }
            auto resource = createOutput(width, height, output);
            if (!resource) {
                return {};
            }
            *reusable = resource;
            return resource;
        }

        auto resource = createOutput(width, height, output);
        if (!resource) {
            return {};
        }
        // All pooled textures are still retained by callers. Keep this
        // transient output until its caller releases it.
        retiredOutputs_.push_back(resource);
        return resource;
    }

    void flush() noexcept
    {
        if (!deviceAccess_) {
            outputPool_.clear();
            retiredOutputs_.clear();
            processor_.reset();
            return;
        }
        try {
            auto contextGuard = deviceAccess_->contextGuard();
            (void)contextGuard;
            if (deviceAccess_->immediateContext()) {
                deviceAccess_->immediateContext().get()->Flush();
            }
            outputPool_.clear();
            retiredOutputs_.clear();
            processor_.reset();
        } catch (...) {
            outputPool_.clear();
            retiredOutputs_.clear();
            processor_.reset();
        }
    }

    std::shared_ptr<D3D11DeviceAccess> deviceAccess_;
    ComPtr<ID3D11VideoDevice> videoDevice_;
    ComPtr<ID3D11VideoContext> videoContext_;
    ComPtr<ID3D11VideoContext1> videoContext1_;
    ProcessorState processor_;
    std::vector<std::shared_ptr<OutputResource>> outputPool_;
    std::vector<std::shared_ptr<OutputResource>> retiredOutputs_;
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
    OutputProfile output;
    if (!impl_->available()
        || !impl_->ensureProcessor(native)
        || !impl_->configureColor(native, color, output)) {
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

    auto outputResource = impl_->acquireOutput(
        native.width(),
        native.height(),
        output);
    if (!outputResource) {
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
            outputResource->outputView.Get(),
            0,
            1,
            &stream))) {
        return {};
    }
    return std::make_shared<ImportedD3D11TextureFrame>(
        frame,
        std::move(inputView),
        std::move(outputResource));
}

void D3D11FrameInterop::flush() noexcept
{
    if (!impl_ || !impl_->deviceAccess_) {
        return;
    }
    impl_->flush();
}

} // namespace qtav
