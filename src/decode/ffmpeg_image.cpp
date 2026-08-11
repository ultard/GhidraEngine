#include "decode/ffmpeg_common.hpp"
#include "decode/image_decoder.hpp"

#include "core/enumerate.hpp"

namespace ghidraengine {
namespace {

Result<Thumbnail> decode_first_frame(std::span<const std::uint8_t> data) {
    init_ffmpeg();

    MemoryReader reader(data);
    auto context = reader.open();
    if (!context) {
        return context.error();
    }

    const int stream_index =
        av_find_best_stream(context->get(), AVMEDIA_TYPE_VIDEO, -1, -1, nullptr, 0);
    if (stream_index < 0) {
        return Error{ErrorCode::UnsupportedFormat, "no image stream found"};
    }

    AVStream* stream = context->get()->streams[stream_index];
    const AVCodec* codec = avcodec_find_decoder(stream->codecpar->codec_id);
    if (codec == nullptr) {
        return Error{ErrorCode::UnsupportedFormat, "no decoder for this format"};
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

    if (const int status = avcodec_open2(decoder.get(), codec, nullptr); status < 0) {
        return ffmpeg_error(status, "open decoder");
    }

    PacketPtr packet(av_packet_alloc());
    FramePtr frame(av_frame_alloc());
    if (!packet || !frame) {
        return Error{ErrorCode::OutOfMemory, "could not allocate decode buffers"};
    }

    const auto receive = [&]() -> Result<bool> {
        const int status = avcodec_receive_frame(decoder.get(), frame.get());
        if (status == 0) {
            return true;
        }
        if (status == AVERROR(EAGAIN) || status == AVERROR_EOF) {
            return false;
        }
        return ffmpeg_error(status, "decode");
    };

    while (av_read_frame(context->get(), packet.get()) >= 0) {
        if (packet->stream_index != stream_index) {
            av_packet_unref(packet.get());
            continue;
        }
        const int status = avcodec_send_packet(decoder.get(), packet.get());
        av_packet_unref(packet.get());
        if (status < 0 && status != AVERROR(EAGAIN)) {
            return ffmpeg_error(status, "send packet");
        }

        auto got = receive();
        if (!got) {
            return got.error();
        }
        if (got.value()) {
            SwsPtr scaler;
            Thumbnail thumb;
            if (auto converted = frame_to_thumbnail(*frame, scaler, thumb); !converted) {
                return converted.error();
            }
            return thumb;
        }
    }

    avcodec_send_packet(decoder.get(), nullptr);
    auto got = receive();
    if (!got) {
        return got.error();
    }
    if (got.value()) {
        SwsPtr scaler;
        Thumbnail thumb;
        if (auto converted = frame_to_thumbnail(*frame, scaler, thumb); !converted) {
            return converted.error();
        }
        return thumb;
    }

    return Error{ErrorCode::DecodeFailed, "no frame was produced"};
}

}

Result<Thumbnail> decode_with_ffmpeg(std::span<const std::uint8_t> data) {
    return decode_first_frame(data);
}

Result<Thumbnail> decode_image(std::span<const std::uint8_t> data) {
    if (data.size() < 4) {
        return Error{ErrorCode::CorruptFile, "file too short to identify"};
    }

    if (data[0] == 0xFF && data[1] == 0xD8 && data[2] == 0xFF) {
        auto thumb = decode_jpeg(data);
        if (thumb) {
            return thumb;
        }
    }

    if (classify_header(data) != MediaKind::Image) {
        return Error{ErrorCode::UnsupportedFormat, "not a recognised image format"};
    }

    return decode_with_ffmpeg(data);
}

}
