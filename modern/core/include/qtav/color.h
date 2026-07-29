// SPDX-License-Identifier: LGPL-2.1-or-later
#pragma once

#include <array>
#include <cstdint>

namespace qtav {

enum class ColorRange {
    Unknown,
    Limited,
    Full,
};

enum class ColorPrimaries {
    Unknown,
    BT709,
    BT470M,
    BT470BG,
    SMPTE170M,
    SMPTE240M,
    Film,
    BT2020,
    SMPTE428,
    SMPTE431,
    SMPTE432,
    EBU3213,
};

enum class ColorTransfer {
    Unknown,
    BT709,
    Gamma22,
    Gamma28,
    SMPTE170M,
    SMPTE240M,
    Linear,
    Log,
    LogSqrt,
    IEC61966_2_4,
    BT1361,
    SRGB,
    BT2020_10,
    BT2020_12,
    PQ,
    SMPTE428,
    HLG,
};

enum class ColorMatrix {
    Unknown,
    RGB,
    BT709,
    FCC,
    BT470BG,
    SMPTE170M,
    SMPTE240M,
    YCgCo,
    BT2020NCL,
    BT2020CL,
    SMPTE2085,
    ChromaDerivedNCL,
    ChromaDerivedCL,
    ICtCp,
};

enum class ChromaLocation {
    Unknown,
    Left,
    Center,
    TopLeft,
    Top,
    BottomLeft,
    Bottom,
};

struct VideoColorSpace {
    ColorRange range = ColorRange::Unknown;
    ColorPrimaries primaries = ColorPrimaries::Unknown;
    ColorTransfer transfer = ColorTransfer::Unknown;
    ColorMatrix matrix = ColorMatrix::Unknown;
    ChromaLocation chromaLocation = ChromaLocation::Unknown;

    bool isSpecified() const noexcept
    {
        return range != ColorRange::Unknown
            || primaries != ColorPrimaries::Unknown
            || transfer != ColorTransfer::Unknown
            || matrix != ColorMatrix::Unknown
            || chromaLocation != ChromaLocation::Unknown;
    }

    bool isHdr() const noexcept
    {
        return transfer == ColorTransfer::PQ
            || transfer == ColorTransfer::HLG;
    }
};

struct Chromaticity {
    double x = 0.0;
    double y = 0.0;
};

struct MasteringDisplayMetadata {
    std::array<Chromaticity, 3> primaries {};
    Chromaticity whitePoint;
    double minimumLuminance = 0.0;
    double maximumLuminance = 0.0;
    bool hasPrimaries = false;
    bool hasLuminance = false;

    bool isValid() const noexcept
    {
        return hasPrimaries || hasLuminance;
    }
};

struct ContentLightMetadata {
    std::uint32_t maximumContentLightLevel = 0;
    std::uint32_t maximumFrameAverageLightLevel = 0;

    bool isValid() const noexcept
    {
        return maximumContentLightLevel != 0
            || maximumFrameAverageLightLevel != 0;
    }
};

} // namespace qtav
