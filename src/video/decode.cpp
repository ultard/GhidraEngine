#include <GhidraEngine/video/decode.hpp>

#include "../cpu/kernels.hpp"
#include "../decode_options.hpp"
#include "../hdr.hpp"
#include "seek.hpp"

extern "C" {
#include <libavcodec/avcodec.h>
#include <libavformat/avformat.h>
#include <libavutil/imgutils.h>
#include <libavutil/pixdesc.h>
#include <libswscale/swscale.h>
}

#include <algorithm>
#include <array>
#include <cerrno>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <fstream>
#include <limits>
#include <new>
#include <stdexcept>
#include <string>
#include <string_view>
#include <system_error>
#include <utility>

namespace GhidraEngine {

namespace {

void free_format(AVFormatContext *p) {
    avformat_close_input(&p);
}

void free_codec(AVCodecContext *p) {
    avcodec_free_context(&p);
}

void free_frame(AVFrame *p) {
    av_frame_free(&p);
}

void free_packet(AVPacket *p) {
    av_packet_free(&p);
}

void free_io(AVIOContext *p) {
    av_free(p->buffer);
    avio_context_free(&p);
}

template <class T, auto Free>
using Owner = std::unique_ptr<T, decltype(Free)>;

}

struct VideoDecoder::Impl {
    VideoDecodeOptions options;
    std::filesystem::path path;
    std::stop_token stop;
    std::ifstream input;
    std::int64_t file_size = 0;

    Owner<AVIOContext, free_io> io{nullptr, free_io};
    Owner<AVFormatContext, free_format> format{nullptr, free_format};
    Owner<AVCodecContext, free_codec> codec{nullptr, free_codec};
    Owner<AVFrame, free_frame> frame{nullptr, free_frame};
    Owner<AVPacket, free_packet> packet{nullptr, free_packet};
    Owner<SwsContext, sws_freeContext> scaler{nullptr, sws_freeContext};

    int stream = -1;
    bool draining = false;
    bool ended = false;
    bool failed = false;
    bool input_error = false;
    bool frame_limit_error = false;
    bool pending = false;

    std::vector<std::uint8_t> rgba;
    std::optional<Timestamp> previous;
    std::optional<Timestamp> selected;

    [[noreturn]] void fail(std::string_view context, int error = AVERROR_INVALIDDATA) const {
        cancel();
        if (error == AVERROR(ENOMEM)) {
            throw std::bad_alloc();
        }

        std::array<char, AV_ERROR_MAX_STRING_SIZE> text{};
        av_strerror(error, text.data(), text.size());

        const auto limit =
            frame_limit_error ? " (native frame allocation exceeds configured limits)" : "";

        throw std::filesystem::filesystem_error(
            std::string(context) + limit + ": " + text.data(),
            path,
            std::make_error_code(std::errc::io_error)
        );
    }

    void cancel() const {
        if (stop.stop_requested()) {
            throw std::system_error(
                std::make_error_code(std::errc::operation_canceled),
                "Video decode canceled"
            );
        }
    }

    bool dimensions(const int width, const int height) const noexcept {
        return width > 0 && height > 0 &&
               static_cast<std::size_t>(width) <= options.max_dimension &&
               static_cast<std::size_t>(height) <= options.max_dimension &&
               static_cast<std::size_t>(width) <=
                   options.max_pixels / static_cast<std::size_t>(height);
    }

    static int interrupt(void *opaque) noexcept {
        return static_cast<Impl *>(opaque)->stop.stop_requested() ? 1 : 0;
    }

    static int read(void *opaque, std::uint8_t *buffer, int size) noexcept {
        auto &self = *static_cast<Impl *>(opaque);

        if (self.stop.stop_requested()) {
            return AVERROR_EXIT;
        }

        try {
            self.input.read(reinterpret_cast<char *>(buffer), size);
            const auto count = self.input.gcount();

            if (self.input.bad() || (!self.input.eof() && self.input.fail())) {
                return AVERROR(EIO);
            }

            return count > 0 ? static_cast<int>(count) : AVERROR_EOF;
        } catch (...) {
            return AVERROR(EIO);
        }
    }

    static std::int64_t seek(void *opaque, const std::int64_t offset, int whence) noexcept {
        auto &self = *static_cast<Impl *>(opaque);

        if (self.stop.stop_requested()) {
            return AVERROR_EXIT;
        }

        if ((static_cast<unsigned>(whence) & static_cast<unsigned>(AVSEEK_SIZE)) != 0) {
            return self.file_size;
        }

        whence =
            static_cast<int>(static_cast<unsigned>(whence) & ~static_cast<unsigned>(AVSEEK_FORCE));

        if (whence != SEEK_SET && whence != SEEK_CUR && whence != SEEK_END) {
            return AVERROR(EINVAL);
        }

        try {
            const auto position = detail::seek_video_input(
                self.input, self.file_size, offset, whence);
            if (position < 0) {
                self.input_error = true;
                return AVERROR(EIO);
            }
            return position;
        } catch (...) {
            return AVERROR(EIO);
        }
    }

    static int allocate_frame(AVCodecContext *context, AVFrame *output, int flags) noexcept {
        auto &self = *static_cast<Impl *>(context->opaque);

        if (self.stop.stop_requested()) {
            return AVERROR_EXIT;
        }

        if (!self.dimensions(output->width, output->height)) {
            self.frame_limit_error = true;

            return AVERROR(EINVAL);
        }

        return avcodec_default_get_buffer2(context, output, flags);
    }

    Impl(std::filesystem::path file, const VideoDecodeOptions &settings, std::stop_token token)
        : options(settings), path(std::move(file)), stop(std::move(token))
    {
        detail::check_decode_options(options);
        cancel();
        open_file();
        open_input();
        discover_stream();
        open_codec();
    }

    void open_file() {
        if (path.empty() ||
            path.native().find(std::filesystem::path::value_type{}) !=
            std::filesystem::path::string_type::npos
        ) {
            throw std::filesystem::filesystem_error(
                "Invalid video file path",
                path,
                std::make_error_code(std::errc::invalid_argument)
            );
        }

        std::error_code error;

        if (!std::filesystem::is_regular_file(path, error)) {
            throw std::filesystem::filesystem_error(
                "Video input must be a regular file",
                path,
                error ? error : std::make_error_code(std::errc::invalid_argument)
            );
        }

        const auto bytes = std::filesystem::file_size(path, error);

        if (error ||
            bytes > static_cast<std::uintmax_t>(std::numeric_limits<std::int64_t>::max())
        ) {
            throw std::filesystem::filesystem_error(
                "Invalid video file size",
                path,
                error ? error : std::make_error_code(std::errc::value_too_large)
            );
        }

        file_size = static_cast<std::int64_t>(bytes);
        input.open(path, std::ios::binary);

        if (!input) {
            fail("Open video file", AVERROR(EIO));
        }
    }

    void open_input() {
        auto *buffer = static_cast<unsigned char *>(av_malloc(65536));

        if (buffer == nullptr) {
            throw std::bad_alloc();
        }

        io.reset(avio_alloc_context(buffer, 65536, 0, this, read, nullptr, seek));

        if (!io) {
            av_free(buffer);
            throw std::bad_alloc();
        }

        format.reset(avformat_alloc_context());

        if (!format) {
            throw std::bad_alloc();
        }

        format->pb = io.get();

        format->flags = static_cast<int>(
            static_cast<unsigned>(format->flags) | static_cast<unsigned>(AVFMT_FLAG_CUSTOM_IO)
        );
        format->interrupt_callback = {interrupt, this};
        format->probesize = 1'048'576;
        format->max_streams = 64;
        format->protocol_whitelist = av_strdup("none");

        if (format->protocol_whitelist == nullptr) {
            throw std::bad_alloc();
        }

        auto *raw = format.release();
        const int opened = avformat_open_input(&raw, nullptr, nullptr, nullptr);
        format.reset(raw);

        if (opened < 0) {
            fail("Probe video contents", opened);
        }

        if (input_error) {
            fail("Video container references data outside the file");
        }

        cancel();
        frame.reset(av_frame_alloc());
        packet.reset(av_packet_alloc());

        if (!frame || !packet) {
            throw std::bad_alloc();
        }
    }

    void discover_stream() {
        std::size_t discovered_bytes = 0;

        for (unsigned probe = 0; probe < 2500; ++probe) {
            for (unsigned i = 0; i < format->nb_streams; ++i) {
                const auto *s = format->streams[i];

                if (s->codecpar->codec_type == AVMEDIA_TYPE_VIDEO &&
                    (static_cast<unsigned>(s->disposition) &
                     static_cast<unsigned>(AV_DISPOSITION_ATTACHED_PIC)) == 0) {
                    stream = static_cast<int>(i);
                    break;
                }
            }

            if (stream >= 0) {
                pending = packet->size > 0 && packet->stream_index == stream;

                if (!pending) {
                    av_packet_unref(packet.get());
                }

                break;
            }

            if ((static_cast<unsigned>(format->ctx_flags) &
                 static_cast<unsigned>(AVFMTCTX_NOHEADER)) == 0 ||
                discovered_bytes >= 1'048'576) {
                break;
            }

            av_packet_unref(packet.get());
            cancel();
            const int status = av_read_frame(format.get(), packet.get());

            if (status < 0) {
                fail("Discover video stream", status);
            }

            discovered_bytes += static_cast<std::size_t>(packet->size);
        }

        if (stream < 0) {
            fail("No video stream within discovery limits");
        }
    }

    void open_codec() {
        auto *s = format->streams[stream];

        if (s->time_base.num <= 0 || s->time_base.den <= 0) {
            fail("Invalid video stream time base");
        }

        const auto *decoder = avcodec_find_decoder(s->codecpar->codec_id);

        if (decoder == nullptr) {
            fail("Unsupported video codec", AVERROR_DECODER_NOT_FOUND);
        }

        codec.reset(avcodec_alloc_context3(decoder));

        if (!codec) {
            throw std::bad_alloc();
        }

        const int parameters = avcodec_parameters_to_context(codec.get(), s->codecpar);

        if (parameters < 0) {
            fail("Copy video codec parameters", parameters);
        }

        if ((codec->width != 0 || codec->height != 0) && !dimensions(codec->width, codec->height)) {
            fail("Video dimensions exceed configured limits");
        }

        codec->thread_count = 1;
        codec->thread_type = 0;
        codec->pkt_timebase = s->time_base;
        codec->max_pixels = static_cast<std::int64_t>(options.max_pixels);
        codec->err_recognition = AV_EF_CAREFUL | AV_EF_EXPLODE | AV_EF_CRCCHECK;
        codec->opaque = this;
        codec->get_buffer2 = allocate_frame;
        const int initialized = avcodec_open2(codec.get(), decoder, nullptr);

        if (initialized < 0) {
            fail("Open video codec", initialized);
        }
    }

    Timestamp timestamp() const {
        const auto ticks = frame->best_effort_timestamp;

        if (ticks == AV_NOPTS_VALUE) {
            fail("Missing video presentation timestamp");
        }

        const auto micros = av_rescale_q(ticks, format->streams[stream]->time_base, {1, 1'000'000});

        if (micros == std::numeric_limits<std::int64_t>::min()) {
            fail("Video timestamp cannot be represented in microseconds");
        }

        return Timestamp{micros};
    }

    std::array<int, 4> orientation() const {
        const auto *data = av_frame_get_side_data(frame.get(), AV_FRAME_DATA_DISPLAYMATRIX);
        const auto *par = format->streams[stream]->codecpar;
        const auto *side = av_packet_side_data_get(
            par->coded_side_data,
            par->nb_coded_side_data,
            AV_PKT_DATA_DISPLAYMATRIX
        );

        if (data == nullptr && side == nullptr) {
            return {1, 0, 0, 1};
        }

        const auto size = data != nullptr ? data->size : side->size;
        const auto *bytes = data != nullptr ? data->data : side->data;
        std::array<std::int32_t, 9> matrix{};

        if (size < sizeof(matrix)) {
            fail("Invalid video display matrix");
        }

        std::memcpy(matrix.data(), bytes, sizeof(matrix));

        if (matrix[2] != 0 || matrix[5] != 0 || matrix[8] != (1'073'741'824)) {
            fail("Unsupported video display perspective");
        }

        std::array axes{matrix[0], matrix[1], matrix[3], matrix[4]};

        for (auto &axis : axes) {
            if (axis >= -1 && axis <= 1) {
                axis = 0;
            } else if (axis >= 65535 && axis <= 65537) {
                axis = 1;
            } else if (axis >= -65537 && axis <= -65535) {
                axis = -1;
            } else {
                fail("Unsupported non-orthogonal video display matrix");
            }
        }

        if (std::abs(axes[0]) + std::abs(axes[2]) != 1 ||
            std::abs(axes[1]) + std::abs(axes[3]) != 1 ||
            std::abs(axes[0]) + std::abs(axes[1]) != 1) {
            fail("Invalid video display axes");
        }

        return axes;
    }

    AVPixelFormat validate_pixel_format() const {
        const auto source = static_cast<AVPixelFormat>(frame->format);
        const auto *description = av_pix_fmt_desc_get(source);

        if (description == nullptr ||
            (description->flags & (static_cast<std::uint64_t>(AV_PIX_FMT_FLAG_HWACCEL) |
                                   static_cast<std::uint64_t>(AV_PIX_FMT_FLAG_FLOAT) |
                                   static_cast<std::uint64_t>(AV_PIX_FMT_FLAG_BAYER))) != 0) {
            fail("Unsupported video pixel format");
        }

        if (is_hdr()) {
            const auto primaries = color_primaries();

            if (primaries != AVCOL_PRI_BT2020 && primaries != AVCOL_PRI_BT709 &&
                primaries != AVCOL_PRI_UNSPECIFIED) {
                fail("Unsupported HDR video color primaries");
            }
        }

        return source;
    }

    AVColorTransferCharacteristic transfer() const {
        if (frame->color_trc != AVCOL_TRC_UNSPECIFIED) {
            return frame->color_trc;
        }

        return codec->color_trc == AVCOL_TRC_UNSPECIFIED ?
            format->streams[stream]->codecpar->color_trc : codec->color_trc;
    }

    bool is_hdr() const {
        return transfer() == AVCOL_TRC_SMPTE2084 || transfer() == AVCOL_TRC_ARIB_STD_B67;
    }

    AVColorPrimaries color_primaries() const {
        if (frame->color_primaries != AVCOL_PRI_UNSPECIFIED) {
            return frame->color_primaries;
        }

        return codec->color_primaries == AVCOL_PRI_UNSPECIFIED ?
            format->streams[stream]->codecpar->color_primaries : codec->color_primaries;
    }

    int color_matrix() const {
        switch (frame->colorspace) {
            case AVCOL_SPC_UNSPECIFIED:

                if (is_hdr()) {
                    return SWS_CS_BT2020;
                }

                return frame->height > 576 ? SWS_CS_ITU709 : SWS_CS_ITU601;
            case AVCOL_SPC_RGB:
            case AVCOL_SPC_BT470BG:
            case AVCOL_SPC_SMPTE170M:

                return SWS_CS_ITU601;
            case AVCOL_SPC_BT709:

                return SWS_CS_ITU709;
            case AVCOL_SPC_FCC:

                return SWS_CS_FCC;
            case AVCOL_SPC_SMPTE240M:

                return SWS_CS_SMPTE240M;
            case AVCOL_SPC_BT2020_NCL:

                return SWS_CS_BT2020;
            default:
                fail("Unsupported video color matrix");
        }
    }

    DecodedImage pixels() {
        if (!dimensions(frame->width, frame->height)) {
            fail("Decoded video dimensions exceed configured limits");
        }

        const auto source = validate_pixel_format();
        const auto target = is_hdr() ? AV_PIX_FMT_RGBA64LE : AV_PIX_FMT_RGBA;
        const auto *description = av_pix_fmt_desc_get(source);
        const bool rgb = (description->flags & AV_PIX_FMT_FLAG_RGB) != 0;

        const int matrix = color_matrix();
        const int full_range = rgb || frame->color_range == AVCOL_RANGE_JPEG ? 1 : 0;
        auto *raw = scaler.release();

        raw = sws_getCachedContext(
            raw,
            frame->width,
            frame->height,
            source,
            frame->width,
            frame->height,
            target,
            SWS_BILINEAR | SWS_BITEXACT,
            nullptr,
            nullptr,
            nullptr
        );
        scaler.reset(raw);

        if (!scaler) {
            fail("Initialize video RGB conversion");
        }

        const auto *coefficients = sws_getCoefficients(matrix);

        if (sws_setColorspaceDetails(
                scaler.get(),
                coefficients,
                full_range,
                coefficients,
                1,
                0,
                65536,
                65536
            ) < 0) {
            fail("Configure video color conversion");
        }

        const auto width = static_cast<std::size_t>(frame->width);
        const auto height = static_cast<std::size_t>(frame->height);
        const int bytes = av_image_get_buffer_size(target, frame->width, frame->height, 32);

        if (bytes < 0) {
            fail("Video RGB allocation size exceeds native limits", bytes);
        }

        rgba.resize(static_cast<std::size_t>(bytes) + AV_INPUT_BUFFER_PADDING_SIZE);
        std::array<std::uint8_t *, 4> output{};
        std::array<int, 4> stride{};

        if (av_image_fill_arrays(
                output.data(),
                stride.data(),
                rgba.data(),
                target,
                frame->width,
                frame->height,
                32
            ) < 0) {
            fail("Initialize video RGB layout");
        }

        const int rows = sws_scale(
            scaler.get(),
            frame->data,
            frame->linesize,
            0,
            frame->height,
            output.data(),
            stride.data()
        );

        if (rows != frame->height) {
            fail("Convert video pixels to RGB");
        }

        if (target == AV_PIX_FMT_RGBA64LE) {
            return convert_luma<true>(width, height, stride);
        }

        return convert_luma<false>(width, height, stride);
    }

    template <bool Hdr>
    DecodedImage convert_luma(
        const std::size_t width,
        const std::size_t height,
        const std::array<int, 4> &stride
    ) const {
        const auto axes = orientation();
        const bool hlg = Hdr && transfer() == AVCOL_TRC_ARIB_STD_B67;
        const bool bt2020 = Hdr && color_primaries() != AVCOL_PRI_BT709;
        const auto *table = Hdr ? &detail::hdr_transfer_table(hlg) : nullptr;
        const auto gamma = hlg ? 1.2 + 0.42 * std::log10(options.hdr.peak_nits / 1000.0) : 0.0;
        const auto peak_scale = options.hdr.peak_nits / 203.0;
        const auto white_squared = peak_scale * peak_scale;
        const auto luma8 = detail::cpu_kernels().luma8;
        const bool direct = axes == std::array{1, 0, 0, 1};
        DecodedImage result{
            axes[0] == 0 ? height : width,
            axes[0] == 0 ? width : height,
            std::vector<float>(width * height)
        };

        for (std::size_t y = 0; y < height; ++y) {
            cancel();

            if constexpr (!Hdr) {
                if (direct) {
                    luma8(
                        rgba.data() + y * static_cast<std::size_t>(stride[0]),
                        std::span<float>(result.luma).subspan(y * width, width),
                        4,
                        options.alpha_background
                    );
                    continue;
                }
            }

            for (std::size_t x = 0; x < width; ++x) {
                const auto offset = y * static_cast<std::size_t>(stride[0]) + x * (Hdr ? 8 : 4);
                std::array<float, 3> channels{};

                if constexpr (Hdr) {
                    const auto sample = [&](std::size_t c) -> unsigned {
                        return static_cast<unsigned>(rgba[offset + c * 2]) |
                               (static_cast<unsigned>(rgba[offset + c * 2 + 1]) << 8U);
                    };
                    const unsigned alpha = sample(3);
                    std::array<unsigned, 3> rgb{sample(0), sample(1), sample(2)};
                    std::array<double, 3> linear{
                        (*table)[rgb[0]],
                        (*table)[rgb[1]],
                        (*table)[rgb[2]]
                    };

                    if (hlg) {
                        const auto luminance =
                            bt2020 ? 0.2627 * linear[0] + 0.6780 * linear[1] + 0.0593 * linear[2] :
                                     0.2126 * linear[0] + 0.7152 * linear[1] + 0.0722 * linear[2];
                        const auto scale =
                            luminance > 0 ? peak_scale * std::pow(luminance, gamma - 1.0) : 0.0;

                        for (auto &c : linear) {
                            c *= scale;
                        }
                    }

                    if (bt2020) {
                        linear = detail::bt2020_to_srgb(linear);
                    }

                    rgb = detail::tone_map(linear, options.hdr, white_squared);

                    for (std::size_t c = 0; c < 3; ++c) {
                        const unsigned blended =
                            rgb[c] * alpha + options.alpha_background[c] * (65535U - alpha) +
                            32767U;

                        channels[c] = static_cast<float>(blended / 65535U);
                    }
                } else {
                    const unsigned alpha = rgba[offset + 3];

                    for (std::size_t c = 0; c < 3; ++c) {
                        const unsigned blended =
                            rgba[offset + c] * alpha +
                            options.alpha_background[c] * (255U - alpha) + 127U;

                        channels[c] = static_cast<float>(blended / 255U);
                    }
                }

                const auto dx = axes[0] != 0 ? (axes[0] > 0 ? x : width - 1 - x)
                    : (axes[2] > 0 ? y : height - 1 - y);
                const auto dy = axes[1] != 0 ? (axes[1] > 0 ? x : width - 1 - x)
                    : (axes[3] > 0 ? y : height - 1 - y);

                result.luma[dy * result.width + dx] =
                    0.299F * channels[0] + 0.587F * channels[1] + 0.114F * channels[2];
            }
        }

        return result;
    }

    void drain_codec() {
        if (io->error < 0 && io->error != AVERROR_EOF) {
            fail("Read video input", io->error);
        }

        const int sent = avcodec_send_packet(codec.get(), nullptr);

        if (sent < 0) {
            fail("Drain video codec", sent);
        }

        draining = true;
    }

    void send_next_packet() {
        int read_status = 0;

        if (pending) {
            pending = false;
        } else {
            do {
                av_packet_unref(packet.get());
                cancel();
                read_status = av_read_frame(format.get(), packet.get());
            } while (read_status >= 0 && packet->stream_index != stream);
        }

        if (input_error) {
            fail("Video container references data outside the file");
        }

        if (read_status == AVERROR_EOF) {
            drain_codec();

            return;
        }

        if (read_status < 0) {
            fail("Demux video packet", read_status);
        }

        if ((static_cast<unsigned>(packet->flags) & static_cast<unsigned>(AV_PKT_FLAG_CORRUPT)) !=
            0) {
            fail("Corrupt video packet");
        }

        const int sent = avcodec_send_packet(codec.get(), packet.get());
        av_packet_unref(packet.get());

        if (sent < 0) {
            fail("Send video packet", sent);
        }
    }

    std::optional<DecodedVideoFrame> select_frame() {
        if (frame->decode_error_flags != 0 || (static_cast<unsigned>(frame->flags) &
                                               static_cast<unsigned>(AV_FRAME_FLAG_CORRUPT)) != 0) {
            fail("Corrupt decoded video frame");
        }

        const auto time = timestamp();

        if (previous && time < *previous) {
            fail("Backward video presentation timestamp");
        }

        previous = time;
        const auto gap = selected ? static_cast<std::uint64_t>(time.count()) -
                                        static_cast<std::uint64_t>(selected->count())
                                  : 0;
        const bool keep =
            !selected || gap >= static_cast<std::uint64_t>(options.sample_interval.count());

        if (!keep) {
            av_frame_unref(frame.get());

            return std::nullopt;
        }

        auto image = pixels();
        cancel();
        selected = time;
        av_frame_unref(frame.get());

        return DecodedVideoFrame{std::move(image), time};
    }

    std::optional<DecodedVideoFrame> next() {
        for (;;) {
            cancel();
            const int received = avcodec_receive_frame(codec.get(), frame.get());

            if (received == 0) {
                if (auto result = select_frame()) {
                    return result;
                }

                continue;
            }

            if (received == AVERROR_EOF) {
                ended = true;

                return std::nullopt;
            }

            if (received != AVERROR(EAGAIN) || draining) {
                fail("Receive video frame", received);
            }

            send_next_packet();
        }
    }
};

VideoDecoder::VideoDecoder(
    const std::filesystem::path &path,
    const VideoDecodeOptions &options,
    const std::stop_token &stop
)
    : impl_(std::make_unique<Impl>(path, options, stop)) {
}

VideoDecoder::~VideoDecoder() = default;

VideoDecoder::VideoDecoder(VideoDecoder &&) noexcept = default;

VideoDecoder &VideoDecoder::operator=(VideoDecoder &&) noexcept = default;

std::optional<DecodedVideoFrame> VideoDecoder::next() const {
    if (!impl_ || impl_->failed) {
        throw std::logic_error("Video decoder is moved-from or failed");
    }

    if (impl_->ended) {
        return std::nullopt;
    }

    try {
        return impl_->next();
    } catch (...) {
        impl_->failed = true;
        throw;
    }
}
}
