#include "video/video_signature.hpp"

#include <algorithm>
#include <cmath>
#include <vector>

#include "core/platform.hpp"
#include "decode/ffmpeg_common.hpp"
#include "hash/phash.hpp"

namespace ghidraengine {
namespace {

// Keyframe-only demuxing usually yields a frame on the first packet; the slack
// covers codecs that need a parameter-set packet first.
constexpr int kMaxPacketsPerSeek = 24;

// Fades and black frames hash to values matching every other flat frame.
constexpr int kMaxVarianceRetries = 3;

std::int64_t container_duration_ms(const AVFormatContext& context, const AVStream& stream) {
    if (context.duration != AV_NOPTS_VALUE && context.duration > 0) {
        return context.duration * 1000 / AV_TIME_BASE;
    }
    if (stream.duration != AV_NOPTS_VALUE && stream.duration > 0) {
        return static_cast<std::int64_t>(static_cast<double>(stream.duration) *
                                         av_q2d(stream.time_base) * 1000.0);
    }
    return 0;
}

struct Decoder {
    FormatContextPtr context;
    CodecContextPtr codec;
    int stream_index = -1;
    AVStream* stream = nullptr;
};

Result<Decoder> open_video(const std::filesystem::path& path) {
    init_ffmpeg();

    AVFormatContext* raw = nullptr;
    // FFmpeg expects UTF-8 and widens it internally on Windows.
    const std::string utf8 = platform::to_utf8(path);

    if (const int status = avformat_open_input(&raw, utf8.c_str(), nullptr, nullptr);
        status < 0) {
        return ffmpeg_error(status, "open " + utf8);
    }
    FormatContextPtr context(raw);

    if (const int status = avformat_find_stream_info(context.get(), nullptr); status < 0) {
        return ffmpeg_error(status, "find stream info");
    }

    const int index =
        av_find_best_stream(context.get(), AVMEDIA_TYPE_VIDEO, -1, -1, nullptr, 0);
    if (index < 0) {
        return Error{ErrorCode::UnsupportedFormat, "file contains no video stream"};
    }

    AVStream* stream = context->streams[index];

    // Cover art presents as a single-frame video stream and would match all other
    // cover art indiscriminately.
    if ((stream->disposition & AV_DISPOSITION_ATTACHED_PIC) != 0) {
        return Error{ErrorCode::UnsupportedFormat, "video stream is attached cover art"};
    }

    const AVCodec* codec = avcodec_find_decoder(stream->codecpar->codec_id);
    if (codec == nullptr) {
        return Error{ErrorCode::UnsupportedFormat, "no decoder for this video codec"};
    }

    CodecContextPtr decoder(avcodec_alloc_context3(codec));
    if (!decoder) {
        return Error{ErrorCode::OutOfMemory, "could not allocate a decoder"};
    }
    if (const int status = avcodec_parameters_to_context(decoder.get(), stream->codecpar);
        status < 0) {
        return ffmpeg_error(status, "configure decoder");
    }

    decoder->thread_count = 1;
    // A partially corrupt video still yields a signature from surviving keyframes.
    decoder->flags2 |= AV_CODEC_FLAG2_FAST;
    decoder->skip_loop_filter = AVDISCARD_NONREF;

    if (const int status = avcodec_open2(decoder.get(), codec, nullptr); status < 0) {
        return ffmpeg_error(status, "open decoder");
    }

    stream->discard = AVDISCARD_NONKEY; // dropped before reaching the decoder

    Decoder result;
    result.context = std::move(context);
    result.codec = std::move(decoder);
    result.stream_index = index;
    result.stream = stream;
    return Result<Decoder>{std::move(result)};
}

// Seeks to `timestamp_ms` and decodes the first keyframe at or before it.
bool grab_frame_at(Decoder& decoder, std::int64_t timestamp_ms, AVPacket* packet,
                   AVFrame* frame) {
    const std::int64_t target = av_rescale_q(timestamp_ms, AVRational{1, 1000},
                                             decoder.stream->time_base);

    if (av_seek_frame(decoder.context.get(), decoder.stream_index, target,
                      AVSEEK_FLAG_BACKWARD) < 0) {
        return false;
    }
    avcodec_flush_buffers(decoder.codec.get());

    for (int attempt = 0; attempt < kMaxPacketsPerSeek; ++attempt) {
        if (av_read_frame(decoder.context.get(), packet) < 0) {
            break;
        }
        if (packet->stream_index != decoder.stream_index) {
            av_packet_unref(packet);
            continue;
        }

        const int status = avcodec_send_packet(decoder.codec.get(), packet);
        av_packet_unref(packet);
        if (status < 0 && status != AVERROR(EAGAIN)) {
            continue;
        }
        if (avcodec_receive_frame(decoder.codec.get(), frame) == 0) {
            return true;
        }
    }

    // The decoder may still be holding the frame.
    avcodec_send_packet(decoder.codec.get(), nullptr);
    const bool flushed = avcodec_receive_frame(decoder.codec.get(), frame) == 0;
    avcodec_flush_buffers(decoder.codec.get());
    return flushed;
}

} // namespace

Result<VideoSignature> extract_video_signature(const std::filesystem::path& path,
                                               const VideoMatchConfig& config) {
    auto opened = open_video(path);
    if (!opened) {
        return opened.error();
    }
    Decoder decoder = std::move(opened.value());

    VideoSignature signature;
    signature.duration_ms = container_duration_ms(*decoder.context, *decoder.stream);
    signature.width = static_cast<std::uint32_t>(decoder.stream->codecpar->width);
    signature.height = static_cast<std::uint32_t>(decoder.stream->codecpar->height);

    if (signature.width == 0 || signature.height == 0) {
        return Error{ErrorCode::CorruptFile, "video reports zero dimensions"};
    }

    PacketPtr packet(av_packet_alloc());
    FramePtr frame(av_frame_alloc());
    if (!packet || !frame) {
        return Error{ErrorCode::OutOfMemory, "could not allocate decode buffers"};
    }

    const std::uint32_t requested =
        std::min<std::uint32_t>(config.frame_samples, static_cast<std::uint32_t>(kMaxVideoFrames));

    // Head and tail are skipped: intros, fades and credits look alike across
    // unrelated videos.
    const double span = 1.0 - 2.0 * config.edge_skip_fraction;
    std::vector<std::int64_t> timestamps;
    timestamps.reserve(requested);

    if (signature.duration_ms <= 0) {
        // Damaged container: take whatever keyframes appear from the start.
        for (std::uint32_t i = 0; i < requested; ++i) {
            timestamps.push_back(static_cast<std::int64_t>(i) * 1000);
        }
    } else {
        for (std::uint32_t i = 0; i < requested; ++i) {
            const double position =
                config.edge_skip_fraction +
                span * (static_cast<double>(i) + 0.5) / static_cast<double>(requested);
            timestamps.push_back(
                static_cast<std::int64_t>(position * static_cast<double>(signature.duration_ms)));
        }
    }

    const std::int64_t retry_step = // nudge when a sample lands on a flat frame
        signature.duration_ms > 0
            ? std::max<std::int64_t>(1, signature.duration_ms / (requested * 8 + 1))
            : 500;

    std::uint32_t collected = 0;
    SwsPtr scaler;
    Thumbnail thumb;

    for (const std::int64_t base : timestamps) {
        bool accepted = false;

        for (int retry = 0; retry <= kMaxVarianceRetries && !accepted; ++retry) {
            const std::int64_t timestamp = base + retry * retry_step;
            if (signature.duration_ms > 0 && timestamp >= signature.duration_ms) {
                break;
            }
            if (!grab_frame_at(decoder, timestamp, packet.get(), frame.get())) {
                continue;
            }
            if (auto converted = frame_to_thumbnail(*frame, scaler, thumb); !converted) {
                av_frame_unref(frame.get());
                continue;
            }
            av_frame_unref(frame.get());

            if (luma_variance(thumb.gray) < config.min_frame_variance) {
                continue; // flat frame; try slightly later
            }

            signature.frames[collected++] = phash64_of_gray(thumb.gray);
            accepted = true;
        }

        if (collected >= requested) {
            break;
        }
    }

    if (collected == 0) {
        return Error{ErrorCode::DecodeFailed, "no usable keyframes could be sampled"};
    }

    signature.frame_count = collected;
    std::copy_n(signature.frames.begin(), collected, signature.sorted.begin());
    std::sort(signature.sorted.begin(),
              signature.sorted.begin() + static_cast<std::ptrdiff_t>(collected));

    return signature;
}

} // namespace ghidraengine
