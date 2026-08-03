// SPDX-License-Identifier: LGPL-2.1-or-later
#pragma once

#include <stdbool.h>

#include <libplacebo/renderer.h>

struct AVFrame;

#ifdef __cplusplus
extern "C" {
#endif

bool qtav_pl_map_avframe(
    pl_gpu gpu,
    struct pl_frame* out,
    pl_tex textures[4],
    const struct AVFrame* frame);

void qtav_pl_unmap_avframe(
    pl_gpu gpu,
    struct pl_frame* frame);

bool qtav_pl_map_dovi(
    struct pl_frame* frame,
    struct pl_dovi_metadata* metadata,
    const struct AVFrame* avframe);

int qtav_pl_dovi_bit_depth(const struct AVFrame* avframe);

#ifdef __cplusplus
}
#endif
