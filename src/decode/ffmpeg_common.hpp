#pragma once

#include <array>
#include <cstdint>
#include <memory>
#include <span>
#include <string_view>

extern "C" {
#include <libavcodec/avcodec.h>
#include <libavformat/avformat.h>
#include <libavutil/imgutils.h>
#include <libswscale/swscale.h>
}

#include "ghidraengine/error.hpp"
#include "decode/thumbnail.hpp"

namespace ghidraengine {

// Routes libav diagnostics away from stderr, where a few corrupt files would
// otherwise bury the caller's output. Idempotent and thread-safe.
void init_ffmpeg();

Error ffmpeg_error(int code, std::string_view context);

struct FormatContextDeleter {
    void operator()(AVFormatContext* context) const noexcept;
};
using FormatContextPtr = std::unique_ptr<AVFormatContext, FormatContextDeleter>;

struct CodecContextDeleter {
    void operator()(AVCodecContext* context) const noexcept { avcodec_free_context(&context); }
};
using CodecContextPtr = std::unique_ptr<AVCodecContext, CodecContextDeleter>;

struct FrameDeleter {
    void operator()(AVFrame* frame) const noexcept { av_frame_free(&frame); }
};
using FramePtr = std::unique_ptr<AVFrame, FrameDeleter>;

struct PacketDeleter {
    void operator()(AVPacket* packet) const noexcept { av_packet_free(&packet); }
};
using PacketPtr = std::unique_ptr<AVPacket, PacketDeleter>;

struct SwsDeleter {
    void operator()(SwsContext* context) const noexcept { sws_freeContext(context); }
};
using SwsPtr = std::unique_ptr<SwsContext, SwsDeleter>;

// Feeds a buffer to libavformat without a temporary file. The buffer must outlive
// the reader.
class MemoryReader {
public:
    explicit MemoryReader(std::span<const std::uint8_t> data);
    ~MemoryReader();

    MemoryReader(const MemoryReader&) = delete;
    MemoryReader& operator=(const MemoryReader&) = delete;

    // The caller owns the context, but the reader must still outlive it.
    Result<FormatContextPtr> open();

private:
    static int read_packet(void* opaque, std::uint8_t* buffer, int size);
    static std::int64_t seek(void* opaque, std::int64_t offset, int whence);

    std::span<const std::uint8_t> data_;
    std::size_t position_ = 0;
    AVIOContext* io_ = nullptr;
};

// Luma at 32x32, chroma at 8x8. `scaler` is reused: building an SwsContext costs
// far more than scaling a single 32x32 output.
Result<void> frame_to_thumbnail(const AVFrame& frame, SwsPtr& scaler, Thumbnail& thumb);

bool pixel_format_is_gray(AVPixelFormat format) noexcept;

} // namespace ghidraengine
