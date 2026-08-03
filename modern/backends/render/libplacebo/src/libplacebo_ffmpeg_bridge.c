// SPDX-License-Identifier: LGPL-2.1-or-later

#define PL_LIBAV_IMPLEMENTATION 1
#include <libplacebo/utils/libav.h>

#include "qtav_libplacebo_ffmpeg_bridge.h"

bool qtav_pl_map_avframe(
    pl_gpu gpu,
    struct pl_frame* out,
    pl_tex textures[4],
    const AVFrame* frame)
{
    return pl_map_avframe_ex(
        gpu,
        out,
        pl_avframe_params(
            .frame = frame,
            .tex = textures,
            .map_dovi = true));
}

void qtav_pl_unmap_avframe(pl_gpu gpu, struct pl_frame* frame)
{
    pl_unmap_avframe(gpu, frame);
}

bool qtav_pl_map_dovi(
    struct pl_frame* frame,
    struct pl_dovi_metadata* metadata,
    const AVFrame* avframe)
{
#ifdef PL_HAVE_LAV_DOLBY_VISION
    const AVFrameSideData* side_data = av_frame_get_side_data(
        avframe,
        AV_FRAME_DATA_DOVI_METADATA);
    if (!side_data || side_data->size == 0)
        return false;
    const AVDOVIMetadata* dovi = (const AVDOVIMetadata*) side_data->data;
    const AVDOVIRpuDataHeader* header = av_dovi_get_header(dovi);
    if (!header || !header->disable_residual_flag)
        return false;
    pl_map_avdovi_metadata(&frame->color, &frame->repr, metadata, dovi);
    return frame->repr.sys == PL_COLOR_SYSTEM_DOLBYVISION
        && frame->repr.dovi == metadata;
#else
    (void) frame;
    (void) metadata;
    (void) avframe;
    return false;
#endif
}

int qtav_pl_dovi_bit_depth(const AVFrame* avframe)
{
#ifdef PL_HAVE_LAV_DOLBY_VISION
    const AVFrameSideData* side_data = av_frame_get_side_data(
        avframe,
        AV_FRAME_DATA_DOVI_METADATA);
    if (!side_data || side_data->size == 0)
        return 0;
    const AVDOVIMetadata* dovi = (const AVDOVIMetadata*) side_data->data;
    const AVDOVIRpuDataHeader* header = av_dovi_get_header(dovi);
    if (!header || !header->disable_residual_flag)
        return 0;
    return header->bl_bit_depth;
#else
    (void) avframe;
    return 0;
#endif
}
