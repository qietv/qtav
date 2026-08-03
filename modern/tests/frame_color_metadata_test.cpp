// SPDX-License-Identifier: LGPL-2.1-or-later

#include "frame_internal.h"

#include <qtav/color.h>

#include <cassert>
#include <cmath>
#include <cstring>

extern "C" {
#include <libavutil/dovi_meta.h>
#include <libavutil/frame.h>
#include <libavutil/mastering_display_metadata.h>
#include <libavutil/mem.h>
#include <libavutil/pixfmt.h>
}

namespace {

bool closeTo(double actual, double expected)
{
    return std::abs(actual - expected) < 0.000001;
}

} // namespace

int main()
{
    AVFrame* native = av_frame_alloc();
    assert(native);
    native->width = 3840;
    native->height = 2160;
    native->format = AV_PIX_FMT_YUV420P10LE;
    assert(av_frame_get_buffer(native, 32) == 0);
    native->color_range = AVCOL_RANGE_MPEG;
    native->color_primaries = AVCOL_PRI_BT2020;
    native->color_trc = AVCOL_TRC_SMPTE2084;
    native->colorspace = AVCOL_SPC_BT2020_NCL;
    native->chroma_location = AVCHROMA_LOC_LEFT;

    AVMasteringDisplayMetadata* mastering =
        av_mastering_display_metadata_create_side_data(native);
    assert(mastering);
    mastering->has_primaries = 1;
    mastering->display_primaries[0][0] = { 34000, 50000 };
    mastering->display_primaries[0][1] = { 16000, 50000 };
    mastering->display_primaries[1][0] = { 13250, 50000 };
    mastering->display_primaries[1][1] = { 34500, 50000 };
    mastering->display_primaries[2][0] = { 7500, 50000 };
    mastering->display_primaries[2][1] = { 3000, 50000 };
    mastering->white_point[0] = { 15635, 50000 };
    mastering->white_point[1] = { 16450, 50000 };
    mastering->has_luminance = 1;
    mastering->min_luminance = { 1, 10000 };
    mastering->max_luminance = { 1000, 1 };

    AVContentLightMetadata* light =
        av_content_light_metadata_create_side_data(native);
    assert(light);
    light->MaxCLL = 1000;
    light->MaxFALL = 400;

    std::size_t doviSize = 0;
    AVDOVIMetadata* dovi = av_dovi_metadata_alloc(&doviSize);
    assert(dovi);
    AVFrameSideData* doviSideData = av_frame_new_side_data(
        native,
        AV_FRAME_DATA_DOVI_METADATA,
        doviSize);
    assert(doviSideData);
    std::memcpy(doviSideData->data, dovi, doviSize);
    av_free(dovi);

    qtav::VideoFrame frame =
        qtav::detail::FrameFactory::video(native, 42, 24);
    av_frame_free(&native);

    assert(frame);
    assert(frame.width() == 3840);
    assert(frame.height() == 2160);
    assert(frame.timestamp() == 42);
    assert(frame.duration() == 24);
    assert(frame.format() == qtav::PixelFormat::YUV420P10);
    assert(frame.formatName() == "yuv420p10le");

    const qtav::VideoColorSpace color = frame.colorSpaceInfo();
    assert(color.isSpecified());
    assert(color.isHdr());
    assert(color.range == qtav::ColorRange::Limited);
    assert(color.primaries == qtav::ColorPrimaries::BT2020);
    assert(color.transfer == qtav::ColorTransfer::PQ);
    assert(color.matrix == qtav::ColorMatrix::BT2020NCL);
    assert(color.chromaLocation == qtav::ChromaLocation::Left);

    const qtav::MasteringDisplayMetadata display =
        frame.masteringDisplayMetadata();
    assert(display.isValid());
    assert(display.hasPrimaries);
    assert(display.hasLuminance);
    assert(closeTo(display.primaries[0].x, 0.68));
    assert(closeTo(display.primaries[0].y, 0.32));
    assert(closeTo(display.whitePoint.x, 0.3127));
    assert(closeTo(display.whitePoint.y, 0.329));
    assert(closeTo(display.minimumLuminance, 0.0001));
    assert(closeTo(display.maximumLuminance, 1000.0));

    const qtav::ContentLightMetadata content =
        frame.contentLightMetadata();
    assert(content.isValid());
    assert(content.maximumContentLightLevel == 1000);
    assert(content.maximumFrameAverageLightLevel == 400);
    assert(frame.hasDolbyVisionMetadata());

    assert(!qtav::VideoFrame {}.colorSpaceInfo().isSpecified());
    assert(!qtav::VideoFrame {}.masteringDisplayMetadata().isValid());
    assert(!qtav::VideoFrame {}.contentLightMetadata().isValid());
    assert(!qtav::VideoFrame {}.hasDolbyVisionMetadata());
    return 0;
}
