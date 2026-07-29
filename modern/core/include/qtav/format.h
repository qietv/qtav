// SPDX-License-Identifier: LGPL-2.1-or-later
#pragma once

namespace qtav {

enum class PixelFormat {
    Unknown,
    YUV420P,
    YUV422P,
    YUV444P,
    NV12,
    NV21,
    P010,
    RGB24,
    BGR24,
    RGBA,
    BGRA,
    ARGB,
    Gray8,
    Native,
};

enum class SampleFormat {
    Unknown,
    U8,
    S16,
    S32,
    Float,
    Double,
    U8Planar,
    S16Planar,
    S32Planar,
    FloatPlanar,
    DoublePlanar,
    Native,
};

} // namespace qtav
