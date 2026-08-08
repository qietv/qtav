// SPDX-License-Identifier: LGPL-2.1-or-later

#include <qtav/libass_subtitle_renderer.h>

#include <algorithm>
#include <array>
#include <cstddef>
#include <cstdio>
#include <limits>
#include <mutex>
#include <sstream>
#include <utility>

extern "C" {
#include <ass/ass.h>
}

namespace qtav {
namespace {

std::string assStyleField(std::string value)
{
    if (value.empty()) {
        return "sans-serif";
    }
    std::replace_if(
        value.begin(),
        value.end(),
        [](char character) {
            return character == ',' || character == '\r'
                || character == '\n';
        },
        ' ');
    return value;
}

std::string defaultAssHeader(
    int width,
    int height,
    const std::string& defaultFamily)
{
    std::ostringstream stream;
    stream
        << "[Script Info]\n"
        << "ScriptType: v4.00+\n"
        << "PlayResX: " << width << "\n"
        << "PlayResY: " << height << "\n"
        << "ScaledBorderAndShadow: yes\n\n"
        << "[V4+ Styles]\n"
        << "Format: Name, Fontname, Fontsize, PrimaryColour, "
           "SecondaryColour, OutlineColour, BackColour, Bold, Italic, "
           "Underline, StrikeOut, ScaleX, ScaleY, Spacing, Angle, "
           "BorderStyle, Outline, Shadow, Alignment, MarginL, MarginR, "
           "MarginV, Encoding\n"
        << "Style: Default," << assStyleField(defaultFamily)
        << ",24,&H00FFFFFF,&H000000FF,"
           "&H00000000,&H00000000,0,0,0,0,100,100,0,0,1,2,0,2,"
           "10,10,10,1\n\n"
        << "[Events]\n"
        << "Format: Layer, Start, End, Style, Name, MarginL, MarginR, "
           "MarginV, Effect, Text\n";
    return stream.str();
}

std::string escapePlainText(std::string text)
{
    std::string result;
    result.reserve(text.size());
    for (const char character : text) {
        switch (character) {
        case '\\':
            result += "\\\\";
            break;
        case '{':
            result += "\\{";
            break;
        case '}':
            result += "\\}";
            break;
        case '\r':
            break;
        case '\n':
            result += "\\N";
            break;
        default:
            result.push_back(character);
            break;
        }
    }
    return result;
}

} // namespace

bool LibassSubtitleRendererConfig::isValid() const noexcept
{
    const bool validStorage =
        (storageWidth == 0 && storageHeight == 0)
        || (storageWidth > 0 && storageHeight > 0);
    return frameWidth > 0 && frameHeight > 0 && validStorage
        && fontScale > 0.0;
}

bool LibassSubtitleImage::isValid() const noexcept
{
    if (width <= 0 || height <= 0 || stride < width) {
        return false;
    }
    const auto rowSize = static_cast<std::size_t>(stride);
    const auto rowCount = static_cast<std::size_t>(height);
    return rowCount <= std::numeric_limits<std::size_t>::max() / rowSize
        && bitmap.size() >= rowSize * rowCount;
}

class LibassSubtitleRenderer::Impl {
public:
    Impl()
        : library_(ass_library_init())
    {
        if (!library_) {
            setError("libass could not initialize its library context");
        } else {
            ass_set_message_cb(library_, &Impl::messageCallback, this);
        }
    }

    ~Impl()
    {
        resetTrack();
        if (renderer_) {
            ass_renderer_done(renderer_);
        }
        if (library_) {
            ass_library_done(library_);
        }
    }

    void setError(std::string error)
    {
        std::lock_guard<std::mutex> lock(errorMutex_);
        lastError_ = std::move(error);
    }

    static void messageCallback(
        int level,
        const char* format,
        va_list arguments,
        void* opaque)
    {
        // Keep libass from writing to stderr. Only fatal/error diagnostics
        // become lastError(); ordinary font-selection warnings remain quiet.
        if (!opaque || !format || level > 1) {
            return;
        }
        std::array<char, 1024> message {};
        std::vsnprintf(
            message.data(), message.size(), format, arguments);
        static_cast<Impl*>(opaque)->setError(message.data());
    }

    void clearError()
    {
        std::lock_guard<std::mutex> lock(errorMutex_);
        lastError_.clear();
    }

    std::string error() const
    {
        std::lock_guard<std::mutex> lock(errorMutex_);
        return lastError_;
    }

    void resetTrack() noexcept
    {
        if (track_) {
            ass_free_track(track_);
            track_ = nullptr;
        }
        trackIndex_ = -1;
        presentationGeneration_ = 0;
        header_.clear();
        nextReadOrder_ = 0;
    }

    bool createTrack(
        int trackIndex,
        std::uint64_t generation,
        std::string header)
    {
        resetTrack();
        track_ = ass_new_track(library_);
        if (!track_) {
            setError("libass could not allocate a subtitle track");
            return false;
        }

        if (header.empty()) {
            const int width = config_.storageWidth > 0
                ? config_.storageWidth
                : config_.frameWidth;
            const int height = config_.storageHeight > 0
                ? config_.storageHeight
                : config_.frameHeight;
            header = defaultAssHeader(
                width, height, config_.defaultFamily);
        }
        if (header.size()
            > static_cast<std::size_t>(std::numeric_limits<int>::max())) {
            setError("The ASS codec-private header is too large");
            resetTrack();
            return false;
        }

        ass_process_codec_private(
            track_,
            header.data(),
            static_cast<int>(header.size()));
        trackIndex_ = trackIndex;
        presentationGeneration_ = generation;
        header_ = std::move(header);
        return true;
    }

    mutable std::mutex mutex_;
    mutable std::mutex errorMutex_;
    ASS_Library* library_ = nullptr;
    ASS_Renderer* renderer_ = nullptr;
    ASS_Track* track_ = nullptr;
    LibassSubtitleRendererConfig config_;
    int trackIndex_ = -1;
    std::uint64_t presentationGeneration_ = 0;
    std::string header_;
    std::uint64_t nextReadOrder_ = 0;
    bool configured_ = false;
    std::string lastError_;
};

LibassSubtitleRenderer::LibassSubtitleRenderer()
    : impl_(std::make_unique<Impl>())
{
}

LibassSubtitleRenderer::~LibassSubtitleRenderer() = default;
LibassSubtitleRenderer::LibassSubtitleRenderer(
    LibassSubtitleRenderer&&) noexcept = default;
LibassSubtitleRenderer& LibassSubtitleRenderer::operator=(
    LibassSubtitleRenderer&&) noexcept = default;

bool LibassSubtitleRenderer::configure(
    const LibassSubtitleRendererConfig& config)
{
    if (!impl_ || !config.isValid()) {
        if (impl_) {
            impl_->setError("The libass renderer configuration is invalid");
        }
        return false;
    }

    std::lock_guard<std::mutex> lock(impl_->mutex_);
    if (!impl_->library_) {
        impl_->setError("The libass library context is unavailable");
        return false;
    }
    impl_->clearError();
    if (!impl_->renderer_) {
        impl_->renderer_ = ass_renderer_init(impl_->library_);
        if (!impl_->renderer_) {
            impl_->setError("libass could not initialize its renderer");
            return false;
        }
    }

    const bool defaultFamilyChanged = impl_->configured_
        && config.defaultFamily != impl_->config_.defaultFamily;
    const bool fontSourceChanged = !impl_->configured_
        || config.defaultFont != impl_->config_.defaultFont
        || defaultFamilyChanged
        || config.fontsDirectory != impl_->config_.fontsDirectory;
    ass_set_frame_size(
        impl_->renderer_, config.frameWidth, config.frameHeight);
    ass_set_storage_size(
        impl_->renderer_,
        config.storageWidth > 0 ? config.storageWidth : config.frameWidth,
        config.storageHeight > 0 ? config.storageHeight : config.frameHeight);
    ass_set_font_scale(impl_->renderer_, config.fontScale);
    ass_set_line_spacing(impl_->renderer_, config.lineSpacing);
    if (fontSourceChanged) {
        ass_set_fonts_dir(
            impl_->library_,
            config.fontsDirectory.empty()
                ? nullptr
                : config.fontsDirectory.c_str());
        ass_set_fonts(
            impl_->renderer_,
            config.defaultFont.empty() ? nullptr : config.defaultFont.c_str(),
            config.defaultFamily.empty()
                ? nullptr
                : config.defaultFamily.c_str(),
            ASS_FONTPROVIDER_AUTODETECT,
            nullptr,
            1);
    }

    const int oldStorageWidth = impl_->config_.storageWidth > 0
        ? impl_->config_.storageWidth
        : impl_->config_.frameWidth;
    const int oldStorageHeight = impl_->config_.storageHeight > 0
        ? impl_->config_.storageHeight
        : impl_->config_.frameHeight;
    const int newStorageWidth = config.storageWidth > 0
        ? config.storageWidth
        : config.frameWidth;
    const int newStorageHeight = config.storageHeight > 0
        ? config.storageHeight
        : config.frameHeight;
    const bool storageChanged = impl_->configured_
        && (newStorageWidth != oldStorageWidth
            || newStorageHeight != oldStorageHeight);
    impl_->config_ = config;
    impl_->configured_ = true;
    if (storageChanged || defaultFamilyChanged) {
        impl_->resetTrack();
    }
    return true;
}

LibassSubtitleRendererConfig LibassSubtitleRenderer::config() const
{
    if (!impl_) {
        return {};
    }
    std::lock_guard<std::mutex> lock(impl_->mutex_);
    return impl_->config_;
}

bool LibassSubtitleRenderer::isConfigured() const noexcept
{
    if (!impl_) {
        return false;
    }
    std::lock_guard<std::mutex> lock(impl_->mutex_);
    return impl_->configured_;
}

bool LibassSubtitleRenderer::add(const SubtitleFrame& frame)
{
    if (!impl_ || !frame) {
        if (impl_) {
            impl_->setError("A valid subtitle frame is required");
        }
        return false;
    }

    std::lock_guard<std::mutex> lock(impl_->mutex_);
    if (!impl_->configured_ || !impl_->library_ || !impl_->renderer_) {
        impl_->setError("The libass renderer is not configured");
        return false;
    }
    impl_->clearError();

    const auto header = frame.assHeader();
    if (!impl_->track_
        || frame.track() != impl_->trackIndex_
        || frame.presentationGeneration()
            != impl_->presentationGeneration_
        || (!header.empty() && header != impl_->header_)) {
        if (!impl_->createTrack(
                frame.track(),
                frame.presentationGeneration(),
                header)) {
            return false;
        }
    }

    auto events = frame.assEvents();
    if (events.empty() && !frame.text().empty()) {
        std::ostringstream event;
        event << impl_->nextReadOrder_++
              << ",0,Default,,0,0,0,,"
              << escapePlainText(frame.text());
        events.push_back(event.str());
    }
    if (events.empty()) {
        impl_->setError("The subtitle frame contains no renderable text");
        return false;
    }

    const auto duration = std::max<std::int64_t>(1, frame.duration());
    for (const auto& event : events) {
        if (event.size()
            > static_cast<std::size_t>(std::numeric_limits<int>::max())) {
            impl_->setError("An ASS subtitle event is too large");
            return false;
        }
        ass_process_chunk(
            impl_->track_,
            event.data(),
            static_cast<int>(event.size()),
            frame.timestamp(),
            duration);
    }
    return true;
}

LibassSubtitleRenderResult LibassSubtitleRenderer::render(
    std::int64_t positionMs)
{
    LibassSubtitleRenderResult result;
    if (!impl_) {
        return result;
    }

    std::lock_guard<std::mutex> lock(impl_->mutex_);
    if (!impl_->configured_ || !impl_->renderer_) {
        impl_->setError("The libass renderer is not configured");
        return result;
    }
    if (!impl_->track_) {
        return result;
    }
    impl_->clearError();

    int change = 0;
    const auto time = std::max<std::int64_t>(0, positionMs);
    for (const ASS_Image* source = ass_render_frame(
             impl_->renderer_, impl_->track_, time, &change);
         source;
         source = source->next) {
        if (!source->bitmap || source->w <= 0 || source->h <= 0
            || source->stride < source->w) {
            continue;
        }

        LibassSubtitleImage image;
        image.x = source->dst_x;
        image.y = source->dst_y;
        image.width = source->w;
        image.height = source->h;
        image.stride = source->w;
        image.red = static_cast<std::uint8_t>(source->color >> 24U);
        image.green = static_cast<std::uint8_t>(source->color >> 16U);
        image.blue = static_cast<std::uint8_t>(source->color >> 8U);
        image.opacity = static_cast<std::uint8_t>(
            255U - (source->color & 0xFFU));
        image.bitmap.resize(
            static_cast<std::size_t>(image.stride)
            * static_cast<std::size_t>(image.height));
        for (int row = 0; row < image.height; ++row) {
            std::copy_n(
                source->bitmap
                    + static_cast<std::ptrdiff_t>(row) * source->stride,
                image.width,
                image.bitmap.data()
                    + static_cast<std::ptrdiff_t>(row) * image.stride);
        }
        result.images.push_back(std::move(image));
    }

    result.change = change >= 2
        ? LibassSubtitleChange::Content
        : change == 1 ? LibassSubtitleChange::Position
                      : LibassSubtitleChange::None;
    return result;
}

void LibassSubtitleRenderer::flush() noexcept
{
    if (!impl_) {
        return;
    }
    std::lock_guard<std::mutex> lock(impl_->mutex_);
    impl_->resetTrack();
}

std::string LibassSubtitleRenderer::lastError() const
{
    return impl_ ? impl_->error() : std::string {};
}

} // namespace qtav
