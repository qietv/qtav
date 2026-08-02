// SPDX-License-Identifier: LGPL-2.1-or-later

#include <qtav/frame.h>
#include <qtav/hardware_frame.h>

#include <algorithm>
#include <array>
#include <utility>

#include "frame_internal.h"

extern "C" {
#include <libavutil/channel_layout.h>
#include <libavutil/frame.h>
#include <libavutil/hwcontext.h>
#include <libavutil/mastering_display_metadata.h>
#include <libavutil/pixdesc.h>
#include <libavutil/pixfmt.h>
#include <libavutil/samplefmt.h>
}

namespace qtav {
namespace {

PixelFormat pixelFormatFromFFmpeg(AVPixelFormat value) noexcept
{
    switch (value) {
    case AV_PIX_FMT_YUV420P:
        return PixelFormat::YUV420P;
    case AV_PIX_FMT_YUV422P:
        return PixelFormat::YUV422P;
    case AV_PIX_FMT_YUV444P:
        return PixelFormat::YUV444P;
    case AV_PIX_FMT_YUV420P10LE:
    case AV_PIX_FMT_YUV420P10BE:
        return PixelFormat::YUV420P10;
    case AV_PIX_FMT_NV12:
        return PixelFormat::NV12;
    case AV_PIX_FMT_NV21:
        return PixelFormat::NV21;
    case AV_PIX_FMT_P010LE:
    case AV_PIX_FMT_P010BE:
        return PixelFormat::P010;
    case AV_PIX_FMT_RGB24:
        return PixelFormat::RGB24;
    case AV_PIX_FMT_BGR24:
        return PixelFormat::BGR24;
    case AV_PIX_FMT_RGBA:
        return PixelFormat::RGBA;
    case AV_PIX_FMT_BGRA:
        return PixelFormat::BGRA;
    case AV_PIX_FMT_ARGB:
        return PixelFormat::ARGB;
    case AV_PIX_FMT_GRAY8:
        return PixelFormat::Gray8;
    case AV_PIX_FMT_NONE:
        return PixelFormat::Unknown;
    default:
        return PixelFormat::Native;
    }
}

SampleFormat sampleFormatFromFFmpeg(AVSampleFormat value) noexcept
{
    switch (value) {
    case AV_SAMPLE_FMT_U8:
        return SampleFormat::U8;
    case AV_SAMPLE_FMT_S16:
        return SampleFormat::S16;
    case AV_SAMPLE_FMT_S32:
        return SampleFormat::S32;
    case AV_SAMPLE_FMT_FLT:
        return SampleFormat::Float;
    case AV_SAMPLE_FMT_DBL:
        return SampleFormat::Double;
    case AV_SAMPLE_FMT_U8P:
        return SampleFormat::U8Planar;
    case AV_SAMPLE_FMT_S16P:
        return SampleFormat::S16Planar;
    case AV_SAMPLE_FMT_S32P:
        return SampleFormat::S32Planar;
    case AV_SAMPLE_FMT_FLTP:
        return SampleFormat::FloatPlanar;
    case AV_SAMPLE_FMT_DBLP:
        return SampleFormat::DoublePlanar;
    case AV_SAMPLE_FMT_NONE:
        return SampleFormat::Unknown;
    default:
        return SampleFormat::Native;
    }
}

std::string safeName(const char* value)
{
    return value ? value : "";
}

int audioChannelCount(const AVFrame* frame) noexcept
{
    return frame->ch_layout.nb_channels;
}

double rationalValue(AVRational value) noexcept
{
    return value.den != 0
        ? static_cast<double>(value.num) / static_cast<double>(value.den)
        : 0.0;
}

ColorRange colorRangeFromFFmpeg(AVColorRange value) noexcept
{
    switch (value) {
    case AVCOL_RANGE_MPEG:
        return ColorRange::Limited;
    case AVCOL_RANGE_JPEG:
        return ColorRange::Full;
    default:
        return ColorRange::Unknown;
    }
}

ColorPrimaries colorPrimariesFromFFmpeg(AVColorPrimaries value) noexcept
{
    switch (value) {
    case AVCOL_PRI_BT709:
        return ColorPrimaries::BT709;
    case AVCOL_PRI_BT470M:
        return ColorPrimaries::BT470M;
    case AVCOL_PRI_BT470BG:
        return ColorPrimaries::BT470BG;
    case AVCOL_PRI_SMPTE170M:
        return ColorPrimaries::SMPTE170M;
    case AVCOL_PRI_SMPTE240M:
        return ColorPrimaries::SMPTE240M;
    case AVCOL_PRI_FILM:
        return ColorPrimaries::Film;
    case AVCOL_PRI_BT2020:
        return ColorPrimaries::BT2020;
    case AVCOL_PRI_SMPTE428:
        return ColorPrimaries::SMPTE428;
    case AVCOL_PRI_SMPTE431:
        return ColorPrimaries::SMPTE431;
    case AVCOL_PRI_SMPTE432:
        return ColorPrimaries::SMPTE432;
    case AVCOL_PRI_EBU3213:
        return ColorPrimaries::EBU3213;
    default:
        return ColorPrimaries::Unknown;
    }
}

ColorTransfer colorTransferFromFFmpeg(
    AVColorTransferCharacteristic value) noexcept
{
    switch (value) {
    case AVCOL_TRC_BT709:
        return ColorTransfer::BT709;
    case AVCOL_TRC_GAMMA22:
        return ColorTransfer::Gamma22;
    case AVCOL_TRC_GAMMA28:
        return ColorTransfer::Gamma28;
    case AVCOL_TRC_SMPTE170M:
        return ColorTransfer::SMPTE170M;
    case AVCOL_TRC_SMPTE240M:
        return ColorTransfer::SMPTE240M;
    case AVCOL_TRC_LINEAR:
        return ColorTransfer::Linear;
    case AVCOL_TRC_LOG:
        return ColorTransfer::Log;
    case AVCOL_TRC_LOG_SQRT:
        return ColorTransfer::LogSqrt;
    case AVCOL_TRC_IEC61966_2_4:
        return ColorTransfer::IEC61966_2_4;
    case AVCOL_TRC_BT1361_ECG:
        return ColorTransfer::BT1361;
    case AVCOL_TRC_IEC61966_2_1:
        return ColorTransfer::SRGB;
    case AVCOL_TRC_BT2020_10:
        return ColorTransfer::BT2020_10;
    case AVCOL_TRC_BT2020_12:
        return ColorTransfer::BT2020_12;
    case AVCOL_TRC_SMPTE2084:
        return ColorTransfer::PQ;
    case AVCOL_TRC_SMPTE428:
        return ColorTransfer::SMPTE428;
    case AVCOL_TRC_ARIB_STD_B67:
        return ColorTransfer::HLG;
    default:
        return ColorTransfer::Unknown;
    }
}

ColorMatrix colorMatrixFromFFmpeg(AVColorSpace value) noexcept
{
    switch (value) {
    case AVCOL_SPC_RGB:
        return ColorMatrix::RGB;
    case AVCOL_SPC_BT709:
        return ColorMatrix::BT709;
    case AVCOL_SPC_FCC:
        return ColorMatrix::FCC;
    case AVCOL_SPC_BT470BG:
        return ColorMatrix::BT470BG;
    case AVCOL_SPC_SMPTE170M:
        return ColorMatrix::SMPTE170M;
    case AVCOL_SPC_SMPTE240M:
        return ColorMatrix::SMPTE240M;
    case AVCOL_SPC_YCGCO:
        return ColorMatrix::YCgCo;
    case AVCOL_SPC_BT2020_NCL:
        return ColorMatrix::BT2020NCL;
    case AVCOL_SPC_BT2020_CL:
        return ColorMatrix::BT2020CL;
    case AVCOL_SPC_SMPTE2085:
        return ColorMatrix::SMPTE2085;
    case AVCOL_SPC_CHROMA_DERIVED_NCL:
        return ColorMatrix::ChromaDerivedNCL;
    case AVCOL_SPC_CHROMA_DERIVED_CL:
        return ColorMatrix::ChromaDerivedCL;
    case AVCOL_SPC_ICTCP:
        return ColorMatrix::ICtCp;
    default:
        return ColorMatrix::Unknown;
    }
}

ChromaLocation chromaLocationFromFFmpeg(AVChromaLocation value) noexcept
{
    switch (value) {
    case AVCHROMA_LOC_LEFT:
        return ChromaLocation::Left;
    case AVCHROMA_LOC_CENTER:
        return ChromaLocation::Center;
    case AVCHROMA_LOC_TOPLEFT:
        return ChromaLocation::TopLeft;
    case AVCHROMA_LOC_TOP:
        return ChromaLocation::Top;
    case AVCHROMA_LOC_BOTTOMLEFT:
        return ChromaLocation::BottomLeft;
    case AVCHROMA_LOC_BOTTOM:
        return ChromaLocation::Bottom;
    default:
        return ChromaLocation::Unknown;
    }
}

class FFmpegHardwareFrameMapping final : public HardwareFrameMapping {
public:
    explicit FFmpegHardwareFrameMapping(AVFrame* frame)
        : frame_(frame)
    {
    }

    ~FFmpegHardwareFrameMapping() override
    {
        av_frame_free(&frame_);
    }

    int width() const noexcept override
    {
        return frame_ ? frame_->width : 0;
    }

    int height() const noexcept override
    {
        return frame_ ? frame_->height : 0;
    }

    PixelFormat format() const noexcept override
    {
        return frame_
            ? pixelFormatFromFFmpeg(
                  static_cast<AVPixelFormat>(frame_->format))
            : PixelFormat::Unknown;
    }

    int planeCount() const noexcept override
    {
        if (!frame_) {
            return 0;
        }
        return std::max(
            0,
            av_pix_fmt_count_planes(
                static_cast<AVPixelFormat>(frame_->format)));
    }

    const std::uint8_t* data(int plane) const noexcept override
    {
        return frame_ && plane >= 0 && plane < planeCount()
            ? frame_->data[plane]
            : nullptr;
    }

    std::uint8_t* writableData(int) noexcept override
    {
        return nullptr;
    }

    int lineSize(int plane) const noexcept override
    {
        return frame_ && plane >= 0 && plane < planeCount()
            ? frame_->linesize[plane]
            : 0;
    }

private:
    AVFrame* frame_ = nullptr;
};

class FFmpegHardwareFrameData final : public HardwareFrameData {
public:
    FFmpegHardwareFrameData(
        const AVFrame* source,
        HardwareDeviceType deviceType,
        std::uintptr_t nativeIdentity,
        std::uint32_t surfaceGeneration,
        std::shared_ptr<void> decoderLifetime)
        : frame_(av_frame_clone(source))
        , deviceType_(deviceType)
        , nativeIdentity_(nativeIdentity)
        , surfaceGeneration_(surfaceGeneration)
        , decoderLifetime_(std::move(decoderLifetime))
    {
    }

    ~FFmpegHardwareFrameData() override
    {
        av_frame_free(&frame_);
    }

    HardwareDeviceType deviceType() const noexcept override
    {
        return frame_ ? deviceType_ : HardwareDeviceType::Unknown;
    }

    int width() const noexcept override
    {
        return frame_ ? frame_->width : 0;
    }

    int height() const noexcept override
    {
        return frame_ ? frame_->height : 0;
    }

    PixelFormat softwareFormat() const noexcept override
    {
        if (frame_ && deviceType_ == HardwareDeviceType::MediaCodec) {
            return PixelFormat::Native;
        }
        if (!frame_ || !frame_->hw_frames_ctx) {
            return PixelFormat::Unknown;
        }
        const auto* frames = reinterpret_cast<const AVHWFramesContext*>(
            frame_->hw_frames_ctx->data);
        return frames
            ? pixelFormatFromFFmpeg(frames->sw_format)
            : PixelFormat::Unknown;
    }

    NativeHandle nativeHandle(
        HardwareHandleType type) const noexcept override
    {
        if (!frame_) {
            return { type, 0, 0 };
        }
        if (deviceType_ == HardwareDeviceType::D3D11
            && type == HardwareHandleType::Texture) {
            return {
                type,
                reinterpret_cast<std::uintptr_t>(frame_->data[0]),
                static_cast<std::uint32_t>(
                    reinterpret_cast<std::uintptr_t>(frame_->data[1])),
            };
        }
        if (deviceType_ == HardwareDeviceType::MediaCodec) {
            if (type == HardwareHandleType::Frame) {
                return {
                    type,
                    reinterpret_cast<std::uintptr_t>(frame_->data[3]),
                    surfaceGeneration_,
                };
            }
            if (type == HardwareHandleType::Surface) {
                return {
                    type,
                    nativeIdentity_,
                    surfaceGeneration_,
                };
            }
        }
        return { type, 0, 0 };
    }

    bool isMappable(HardwareMapMode mode) const noexcept override
    {
        return frame_ && frame_->hw_frames_ctx
            && mode == HardwareMapMode::Read;
    }

    std::shared_ptr<HardwareFrameMapping> map(
        HardwareMapMode mode) const override
    {
        if (!isMappable(mode)) {
            return {};
        }
        auto* mapped = av_frame_alloc();
        if (!mapped) {
            return {};
        }
        if (av_hwframe_transfer_data(mapped, frame_, 0) < 0) {
            av_frame_free(&mapped);
            return {};
        }
        av_frame_copy_props(mapped, frame_);
        return std::make_shared<FFmpegHardwareFrameMapping>(mapped);
    }

private:
    AVFrame* frame_ = nullptr;
    HardwareDeviceType deviceType_ = HardwareDeviceType::Unknown;
    std::uintptr_t nativeIdentity_ = 0;
    std::uint32_t surfaceGeneration_ = 0;
    std::shared_ptr<void> decoderLifetime_;
};

} // namespace

struct VideoFrame::Storage {
    Storage(
        const AVFrame* source,
        std::int64_t timestamp,
        std::int64_t duration,
        HardwareDeviceType hardwareDeviceType,
        std::uintptr_t hardwareNativeIdentity,
        std::uint32_t hardwareSurfaceGeneration,
        std::shared_ptr<void> decoderLifetime)
        : frame(av_frame_clone(source))
        , timestampMs(timestamp)
        , durationMs(duration)
    {
        if (frame && hardwareDeviceType != HardwareDeviceType::Unknown) {
            hardwareFrame = HardwareFrame(
                std::make_shared<FFmpegHardwareFrameData>(
                    frame,
                    hardwareDeviceType,
                    hardwareNativeIdentity,
                    hardwareSurfaceGeneration,
                    std::move(decoderLifetime)));
        }
    }

    Storage(
        HardwareFrame source,
        std::int64_t timestamp,
        std::int64_t duration)
        : frame(av_frame_alloc())
        , hardwareFrame(std::move(source))
        , timestampMs(timestamp)
        , durationMs(duration)
    {
        if (!frame || !hardwareFrame) {
            av_frame_free(&frame);
            return;
        }
        frame->width = hardwareFrame.width();
        frame->height = hardwareFrame.height();
        frame->format = AV_PIX_FMT_NONE;
    }

    ~Storage()
    {
        av_frame_free(&frame);
    }

    AVFrame* frame = nullptr;
    HardwareFrame hardwareFrame;
    std::int64_t timestampMs = 0;
    std::int64_t durationMs = 0;
};

struct AudioFrame::Storage {
    Storage(const AVFrame* source, std::int64_t timestamp, std::int64_t duration)
        : frame(av_frame_clone(source))
        , timestampMs(timestamp)
        , durationMs(duration)
    {
    }

    ~Storage()
    {
        av_frame_free(&frame);
    }

    AVFrame* frame = nullptr;
    std::int64_t timestampMs = 0;
    std::int64_t durationMs = 0;
};

VideoFrame::VideoFrame(std::shared_ptr<const Storage> storage)
    : storage_(std::move(storage))
{
}

VideoFrame::operator bool() const noexcept
{
    return isValid();
}

bool VideoFrame::isValid() const noexcept
{
    return storage_ && storage_->frame && storage_->frame->width > 0
        && storage_->frame->height > 0;
}

int VideoFrame::width() const noexcept
{
    return isValid() ? storage_->frame->width : 0;
}

int VideoFrame::height() const noexcept
{
    return isValid() ? storage_->frame->height : 0;
}

PixelFormat VideoFrame::format() const noexcept
{
    if (hasHardwareFrame()) {
        return storage_->hardwareFrame.softwareFormat();
    }
    return isValid()
        ? pixelFormatFromFFmpeg(static_cast<AVPixelFormat>(storage_->frame->format))
        : PixelFormat::Unknown;
}

int VideoFrame::nativeFormat() const noexcept
{
    return isValid() ? storage_->frame->format : AV_PIX_FMT_NONE;
}

std::string VideoFrame::formatName() const
{
    return isValid()
        ? safeName(av_get_pix_fmt_name(
              static_cast<AVPixelFormat>(storage_->frame->format)))
        : std::string {};
}

int VideoFrame::planeCount() const noexcept
{
    if (!isValid() || hasHardwareFrame()) {
        return 0;
    }
    return std::max(
        0,
        av_pix_fmt_count_planes(
            static_cast<AVPixelFormat>(storage_->frame->format)));
}

const std::uint8_t* VideoFrame::data(int plane) const noexcept
{
    if (!isValid() || hasHardwareFrame() || plane < 0
        || plane >= AV_NUM_DATA_POINTERS) {
        return nullptr;
    }
    return storage_->frame->data[plane];
}

int VideoFrame::lineSize(int plane) const noexcept
{
    if (!isValid() || hasHardwareFrame() || plane < 0
        || plane >= AV_NUM_DATA_POINTERS) {
        return 0;
    }
    return storage_->frame->linesize[plane];
}

std::int64_t VideoFrame::timestamp() const noexcept
{
    return storage_ ? storage_->timestampMs : 0;
}

std::int64_t VideoFrame::duration() const noexcept
{
    return storage_ ? storage_->durationMs : 0;
}

std::string VideoFrame::colorSpace() const
{
    if (!isValid()) {
        return {};
    }

    const auto* frame = storage_->frame;
    std::string result = safeName(av_color_space_name(frame->colorspace));
    const auto primaries = safeName(av_color_primaries_name(frame->color_primaries));
    const auto transfer = safeName(av_color_transfer_name(frame->color_trc));
    if (!primaries.empty()) {
        result += result.empty() ? primaries : "/" + primaries;
    }
    if (!transfer.empty()) {
        result += result.empty() ? transfer : "/" + transfer;
    }
    return result;
}

VideoColorSpace VideoFrame::colorSpaceInfo() const noexcept
{
    if (!isValid()) {
        return {};
    }
    const auto* frame = storage_->frame;
    return {
        colorRangeFromFFmpeg(frame->color_range),
        colorPrimariesFromFFmpeg(frame->color_primaries),
        colorTransferFromFFmpeg(frame->color_trc),
        colorMatrixFromFFmpeg(frame->colorspace),
        chromaLocationFromFFmpeg(frame->chroma_location),
    };
}

MasteringDisplayMetadata
VideoFrame::masteringDisplayMetadata() const noexcept
{
    MasteringDisplayMetadata result;
    if (!isValid()) {
        return result;
    }
    const AVFrameSideData* sideData = av_frame_get_side_data(
        storage_->frame,
        AV_FRAME_DATA_MASTERING_DISPLAY_METADATA);
    if (!sideData
        || sideData->size < sizeof(AVMasteringDisplayMetadata)) {
        return result;
    }
    const auto* metadata =
        reinterpret_cast<const AVMasteringDisplayMetadata*>(sideData->data);
    result.hasPrimaries = metadata->has_primaries != 0;
    result.hasLuminance = metadata->has_luminance != 0;
    if (result.hasPrimaries) {
        for (std::size_t index = 0; index < result.primaries.size(); ++index) {
            result.primaries[index] = {
                rationalValue(metadata->display_primaries[index][0]),
                rationalValue(metadata->display_primaries[index][1]),
            };
        }
        result.whitePoint = {
            rationalValue(metadata->white_point[0]),
            rationalValue(metadata->white_point[1]),
        };
    }
    if (result.hasLuminance) {
        result.minimumLuminance =
            rationalValue(metadata->min_luminance);
        result.maximumLuminance =
            rationalValue(metadata->max_luminance);
    }
    return result;
}

ContentLightMetadata VideoFrame::contentLightMetadata() const noexcept
{
    ContentLightMetadata result;
    if (!isValid()) {
        return result;
    }
    const AVFrameSideData* sideData = av_frame_get_side_data(
        storage_->frame,
        AV_FRAME_DATA_CONTENT_LIGHT_LEVEL);
    if (!sideData || sideData->size < sizeof(AVContentLightMetadata)) {
        return result;
    }
    const auto* metadata =
        reinterpret_cast<const AVContentLightMetadata*>(sideData->data);
    result.maximumContentLightLevel = metadata->MaxCLL;
    result.maximumFrameAverageLightLevel = metadata->MaxFALL;
    return result;
}

bool VideoFrame::hasHardwareFrame() const noexcept
{
    return storage_ && storage_->hardwareFrame.isValid();
}

HardwareFrame VideoFrame::hardwareFrame() const
{
    return storage_ ? storage_->hardwareFrame : HardwareFrame {};
}

AudioFrame::AudioFrame(std::shared_ptr<const Storage> storage)
    : storage_(std::move(storage))
{
}

AudioFrame::operator bool() const noexcept
{
    return isValid();
}

bool AudioFrame::isValid() const noexcept
{
    return storage_ && storage_->frame && storage_->frame->sample_rate > 0
        && storage_->frame->nb_samples > 0
        && audioChannelCount(storage_->frame) > 0;
}

int AudioFrame::sampleRate() const noexcept
{
    return isValid() ? storage_->frame->sample_rate : 0;
}

int AudioFrame::channels() const noexcept
{
    return isValid() ? audioChannelCount(storage_->frame) : 0;
}

int AudioFrame::samplesPerChannel() const noexcept
{
    return isValid() ? storage_->frame->nb_samples : 0;
}

SampleFormat AudioFrame::format() const noexcept
{
    return isValid()
        ? sampleFormatFromFFmpeg(
              static_cast<AVSampleFormat>(storage_->frame->format))
        : SampleFormat::Unknown;
}

int AudioFrame::nativeFormat() const noexcept
{
    return isValid() ? storage_->frame->format : AV_SAMPLE_FMT_NONE;
}

std::string AudioFrame::formatName() const
{
    return isValid()
        ? safeName(av_get_sample_fmt_name(
              static_cast<AVSampleFormat>(storage_->frame->format)))
        : std::string {};
}

std::string AudioFrame::channelLayout() const
{
    if (!isValid()) {
        return {};
    }
    std::array<char, 128> description {};
    if (av_channel_layout_describe(
            &storage_->frame->ch_layout,
            description.data(),
            description.size())
        < 0) {
        return {};
    }
    return description.data();
}

int AudioFrame::planeCount() const noexcept
{
    if (!isValid()) {
        return 0;
    }
    return av_sample_fmt_is_planar(
               static_cast<AVSampleFormat>(storage_->frame->format))
        ? channels()
        : 1;
}

const std::uint8_t* AudioFrame::data(int plane) const noexcept
{
    if (!isValid() || plane < 0 || plane >= planeCount()) {
        return nullptr;
    }
    return storage_->frame->extended_data[plane];
}

int AudioFrame::lineSize(int plane) const noexcept
{
    if (!isValid() || plane < 0 || plane >= planeCount()) {
        return 0;
    }
    const int lineSize = plane < AV_NUM_DATA_POINTERS
        ? storage_->frame->linesize[plane]
        : 0;
    return lineSize != 0 ? lineSize : storage_->frame->linesize[0];
}

std::int64_t AudioFrame::timestamp() const noexcept
{
    return storage_ ? storage_->timestampMs : 0;
}

std::int64_t AudioFrame::duration() const noexcept
{
    return storage_ ? storage_->durationMs : 0;
}

VideoFrame detail::FrameFactory::video(
    const AVFrame* frame,
    std::int64_t timestampMs,
    std::int64_t durationMs,
    HardwareDeviceType hardwareDeviceType,
    std::uintptr_t hardwareNativeIdentity,
    std::uint32_t hardwareSurfaceGeneration,
    std::shared_ptr<void> decoderLifetime)
{
    if (!frame) {
        return {};
    }
    auto storage =
        std::make_shared<VideoFrame::Storage>(
            frame,
            timestampMs,
            durationMs,
            hardwareDeviceType,
            hardwareNativeIdentity,
            hardwareSurfaceGeneration,
            std::move(decoderLifetime));
    return storage->frame ? VideoFrame(std::move(storage)) : VideoFrame {};
}

AudioFrame detail::FrameFactory::audio(
    const AVFrame* frame,
    std::int64_t timestampMs,
    std::int64_t durationMs)
{
    if (!frame) {
        return {};
    }
    auto storage =
        std::make_shared<AudioFrame::Storage>(frame, timestampMs, durationMs);
    return storage->frame ? AudioFrame(std::move(storage)) : AudioFrame {};
}

VideoFrame detail::FrameFactory::hardware(
    HardwareFrame frame,
    std::int64_t timestampMs,
    std::int64_t durationMs)
{
    auto storage = std::make_shared<VideoFrame::Storage>(
        std::move(frame),
        timestampMs,
        durationMs);
    return storage->frame ? VideoFrame(std::move(storage)) : VideoFrame {};
}

} // namespace qtav
