// SPDX-License-Identifier: LGPL-2.1-or-later
#pragma once

#include <cstdint>
#include <memory>
#include <string>
#include <vector>

#include <qtav/frame.h>
#include <qtav/subtitle_libass_export.h>

namespace qtav {

struct QTAV_SUBTITLE_LIBASS_EXPORT LibassSubtitleRendererConfig {
    int frameWidth = 0;
    int frameHeight = 0;
    // The uncropped source-video size. Zero selects frameWidth/frameHeight.
    int storageWidth = 0;
    int storageHeight = 0;
    double fontScale = 1.0;
    double lineSpacing = 0.0;
    std::string defaultFont;
    std::string defaultFamily = "sans-serif";
    std::string fontsDirectory;

    bool isValid() const noexcept;
};

// One owning 8-bit coverage bitmap from libass. Images must be composited in
// vector order. Effective source alpha is bitmap[x] * opacity / 255.
struct QTAV_SUBTITLE_LIBASS_EXPORT LibassSubtitleImage {
    int x = 0;
    int y = 0;
    int width = 0;
    int height = 0;
    int stride = 0;
    std::uint8_t red = 0;
    std::uint8_t green = 0;
    std::uint8_t blue = 0;
    std::uint8_t opacity = 0;
    std::vector<std::uint8_t> bitmap;

    bool isValid() const noexcept;
};

enum class LibassSubtitleChange {
    None,
    Position,
    Content,
};

struct QTAV_SUBTITLE_LIBASS_EXPORT LibassSubtitleRenderResult {
    std::vector<LibassSubtitleImage> images;
    LibassSubtitleChange change = LibassSubtitleChange::None;
};

// Thread-safe, caller-driven ASS/SSA subtitle rasterizer. Feed each published
// SubtitleFrame exactly once with add(), then call render() at the current
// media position (normally once per video redraw). A track switch or seek is
// detected through the frame's track/generation identity and resets old cues.
class QTAV_SUBTITLE_LIBASS_EXPORT LibassSubtitleRenderer {
public:
    LibassSubtitleRenderer();
    ~LibassSubtitleRenderer();

    LibassSubtitleRenderer(LibassSubtitleRenderer&&) noexcept;
    LibassSubtitleRenderer& operator=(LibassSubtitleRenderer&&) noexcept;
    LibassSubtitleRenderer(const LibassSubtitleRenderer&) = delete;
    LibassSubtitleRenderer& operator=(const LibassSubtitleRenderer&) = delete;

    bool configure(const LibassSubtitleRendererConfig& config);
    LibassSubtitleRendererConfig config() const;
    bool isConfigured() const noexcept;

    bool add(const SubtitleFrame& frame);
    LibassSubtitleRenderResult render(std::int64_t positionMs);
    void flush() noexcept;

    std::string lastError() const;

private:
    class Impl;
    std::unique_ptr<Impl> impl_;
};

} // namespace qtav
