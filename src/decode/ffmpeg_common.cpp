#include "decode/ffmpeg_common.hpp"

#include <algorithm>
#include <cstring>
#include <mutex>
#include <string>

extern "C" {
#include <libavutil/error.h>
#include <libavutil/log.h>
#include <libavutil/pixdesc.h>
}

#include "decode/resample.hpp"

namespace ghidraengine {
namespace {

constexpr int kIoBufferSize = 32 * 1024;

void silent_log_callback(void*, int, const char*, va_list) {
}

}

void init_ffmpeg() {
    static std::once_flag flag;
    std::call_once(flag, [] {
        av_log_set_level(AV_LOG_QUIET);
        av_log_set_callback(&silent_log_callback);
    });
}

Error ffmpeg_error(int code, std::string_view context) {
    char buffer[AV_ERROR_MAX_STRING_SIZE] = {};
    av_strerror(code, buffer, sizeof(buffer));

    ErrorCode kind = ErrorCode::DecodeFailed;
    if (code == AVERROR(ENOENT)) {
        kind = ErrorCode::NotFound;
    } else if (code == AVERROR(EACCES) || code == AVERROR(EPERM)) {
        kind = ErrorCode::AccessDenied;
    } else if (code == AVERROR(ENOMEM)) {
        kind = ErrorCode::OutOfMemory;
    } else if (code == AVERROR_INVALIDDATA) {
        kind = ErrorCode::CorruptFile;
    } else if (code == AVERROR_DECODER_NOT_FOUND || code == AVERROR_DEMUXER_NOT_FOUND) {
        kind = ErrorCode::UnsupportedFormat;
    }

    return Error{kind, std::string(context) + ": " + buffer};
}

void FormatContextDeleter::operator()(AVFormatContext* context) const noexcept {
    if (context != nullptr) {
        avformat_close_input(&context);
    }
}

MemoryReader::MemoryReader(std::span<const std::uint8_t> data) : data_(data) {}

MemoryReader::~MemoryReader() {
    if (io_ != nullptr) {
        av_freep(&io_->buffer);
        avio_context_free(&io_);
    }
}

int MemoryReader::read_packet(void* opaque, std::uint8_t* buffer, int size) {
    auto* self = static_cast<MemoryReader*>(opaque);
    if (self->position_ >= self->data_.size()) {
        return AVERROR_EOF;
    }
    const std::size_t available = self->data_.size() - self->position_;
    const std::size_t count = std::min<std::size_t>(available, static_cast<std::size_t>(size));
    std::memcpy(buffer, self->data_.data() + self->position_, count);
    self->position_ += count;
    return static_cast<int>(count);
}

std::int64_t MemoryReader::seek(void* opaque, std::int64_t offset, int whence) {
    auto* self = static_cast<MemoryReader*>(opaque);
    const auto size = static_cast<std::int64_t>(self->data_.size());

    if (whence == AVSEEK_SIZE) {
        return size;
    }

    std::int64_t target = offset;
    if (whence == SEEK_CUR) {
        target = static_cast<std::int64_t>(self->position_) + offset;
    } else if (whence == SEEK_END) {
        target = size + offset;
    }
    if (target < 0 || target > size) {
        return AVERROR(EINVAL);
    }
    self->position_ = static_cast<std::size_t>(target);
    return target;
}

Result<FormatContextPtr> MemoryReader::open() {
    auto* buffer = static_cast<std::uint8_t*>(av_malloc(kIoBufferSize));
    if (buffer == nullptr) {
        return Error{ErrorCode::OutOfMemory, "could not allocate an AVIO buffer"};
    }

    io_ = avio_alloc_context(buffer, kIoBufferSize, 0, this, &read_packet,
                             nullptr, &seek);
    if (io_ == nullptr) {
        av_free(buffer);
        return Error{ErrorCode::OutOfMemory, "could not allocate an AVIO context"};
    }

    AVFormatContext* raw = avformat_alloc_context();
    if (raw == nullptr) {
        return Error{ErrorCode::OutOfMemory, "could not allocate a format context"};
    }
    raw->pb = io_;
    raw->flags |= AVFMT_FLAG_CUSTOM_IO;

    if (const int status = avformat_open_input(&raw, nullptr, nullptr, nullptr); status < 0) {
        return ffmpeg_error(status, "open input");
    }

    FormatContextPtr context(raw);
    if (const int status = avformat_find_stream_info(context.get(), nullptr); status < 0) {
        return ffmpeg_error(status, "find stream info");
    }
    return Result<FormatContextPtr>{std::move(context)};
}

bool pixel_format_is_gray(AVPixelFormat format) noexcept {
    const AVPixFmtDescriptor* descriptor = av_pix_fmt_desc_get(format);
    if (descriptor == nullptr) {
        return false;
    }
    const bool paletted = (descriptor->flags & AV_PIX_FMT_FLAG_PAL) != 0;
    return !paletted && descriptor->nb_components <= 2;
}

Result<void> frame_to_thumbnail(const AVFrame& frame, SwsPtr& scaler, Thumbnail& thumb) {
    if (frame.width <= 0 || frame.height <= 0) {
        return Error{ErrorCode::DecodeFailed, "frame has no dimensions"};
    }

    const auto source_format = static_cast<AVPixelFormat>(frame.format);
    if (source_format == AV_PIX_FMT_NONE) {
        return Error{ErrorCode::DecodeFailed, "frame has no pixel format"};
    }

    constexpr int kTarget = static_cast<int>(kThumbSize);

    SwsContext* updated = sws_getCachedContext(
        scaler.release(), frame.width, frame.height, source_format, kTarget, kTarget,
        AV_PIX_FMT_YUV444P, SWS_AREA, nullptr, nullptr, nullptr);
    if (updated == nullptr) {
        return Error{ErrorCode::DecodeFailed, "could not create a scaler"};
    }
    scaler.reset(updated);

    std::array<std::uint8_t, kThumbSize * kThumbSize> planes[3];
    std::uint8_t* destination[4] = {planes[0].data(), planes[1].data(), planes[2].data(),
                                    nullptr};
    int strides[4] = {kTarget, kTarget, kTarget, 0};

    const int rows = sws_scale(scaler.get(), frame.data, frame.linesize, 0, frame.height,
                               destination, strides);
    if (rows <= 0) {
        return Error{ErrorCode::DecodeFailed, "scaling produced no rows"};
    }

    thumb.gray = planes[0];
    thumb.source_width = static_cast<std::uint32_t>(frame.width);
    thumb.source_height = static_cast<std::uint32_t>(frame.height);
    thumb.has_color = !pixel_format_is_gray(source_format);

    if (thumb.has_color) {
        box_resample_channel(planes[1].data(), kThumbSize, kThumbSize, kThumbSize, 1, 0,
                             thumb.cb.data(), kChromaSize, kChromaSize);
        box_resample_channel(planes[2].data(), kThumbSize, kThumbSize, kThumbSize, 1, 0,
                             thumb.cr.data(), kChromaSize, kChromaSize);
    }

    return {};
}

}
